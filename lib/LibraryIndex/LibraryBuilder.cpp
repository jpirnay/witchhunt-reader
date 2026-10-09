#include "LibraryBuilder.h"

#include <Logging.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include "LibraryBlob.h"
#include "LibraryJoin.h"
#include "LibraryKeys.h"
#include "LibraryPublish.h"

namespace {

constexpr uint32_t FNV_BASIS = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(uint32_t hash, const void* data, const size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// What a book and its metadata sidecar share: the filename before its extension.
uint32_t stemHash(const char* name) {
  const char* dot = std::strrchr(name, '.');
  const size_t len = (dot != nullptr && dot != name) ? static_cast<size_t>(dot - name) : std::strlen(name);
  return fnv1a(FNV_BASIS, name, len);
}

// The extensions SidecarFiles::kMetadataExtensions resolves.
bool isMetadataSidecar(const char* name) {
  const size_t len = std::strlen(name);
  return len > 4 && (std::strcmp(name + len - 4, ".opf") == 0 || std::strcmp(name + len - 4, ".OPF") == 0);
}

// The later of an entry's modified and created times, as FAT (date << 16) | time.
uint32_t packDate(HalFile& entry) {
  uint16_t date = 0;
  uint16_t time = 0;
  uint32_t latest = 0;
  if (entry.getModifyDateTime(&date, &time)) latest = (uint32_t{date} << 16) | time;
  if (entry.getCreateDateTime(&date, &time)) latest = std::max(latest, (uint32_t{date} << 16) | time);
  return latest;
}

std::string childPath(const std::string& parent, const char* name) {
  return (parent.empty() || parent.back() == '/') ? parent + name : parent + '/' + name;
}

}  // namespace

LibraryBuilder::LibraryBuilder(Config config) : config_(std::move(config)) { levels_.reserve(MAX_DEPTH + 1); }

std::string LibraryBuilder::work(const char* name) const { return config_.workDir + "/" + name; }

bool LibraryBuilder::takePublished() {
  const bool was = published_;
  published_ = false;
  return was;
}

void LibraryBuilder::fail([[maybe_unused]] const char* what) {  // only the log reads it
  LOG_ERR("LIB", "library build failed: %s", what);
  levels_.clear();
  if (resolveOpen_) closeResolveFiles();
  phase_ = Phase::Failed;
}

LibraryBuilder::Phase LibraryBuilder::step(BuildArena* arena) {
  if (finished()) return phase_;
  if (needsArena() && (arena == nullptr || !arena->valid())) return phase_;
  switch (phase_) {
    case Phase::Walk:
      walkStep();
      break;
    case Phase::Join:
      join(*arena);
      break;
    case Phase::Publish:
      publish(*arena);
      break;
    case Phase::Resolve:
      resolveStep(*arena);
      break;
    default:
      break;
  }
  return phase_;
}

bool LibraryBuilder::startWalk() {
  started_ = true;
  if (!Storage.exists(config_.workDir.c_str())) Storage.mkdir(config_.workDir.c_str());
  if (!Storage.openFileForWrite("LIB", work("stage.bin"), stage_) ||
      !Storage.openFileForWrite("LIB", work("paths.bin"), paths_)) {
    return false;
  }
  HalFile root = Storage.open(config_.root.c_str());
  if (!root || !root.isDirectory()) return false;
  root.rewindDirectory();
  levels_.push_back(Level{std::move(root), config_.root});
  return true;
}

bool LibraryBuilder::listable(const char* name, const bool atRoot) const {
  if (name[0] == '.') {
    if (!config_.showHidden) return false;
    if (atRoot && std::strcmp(name, ".crosspoint") == 0) return false;  // the firmware's own cache
  }
  return std::strcmp(name, "System Volume Information") != 0;
}

void LibraryBuilder::noteSidecar(Level& level, HalFile& entry) {
  if (level.sidecars.size() >= MAX_SIDECARS) {
    level.overflow = true;
    return;
  }
  const auto size = static_cast<uint32_t>(entry.fileSize());
  const uint32_t date = packDate(entry);
  uint32_t sig = fnv1a(fnv1a(FNV_BASIS, &size, sizeof(size)), &date, sizeof(date));
  if (sig == library::SIDECAR_NONE || sig == library::SIDECAR_UNKNOWN) sig = 1;
  level.sidecars.push_back({stemHash(name_), sig});
}

uint32_t LibraryBuilder::sidecarFor(const Level& level) const {
  const uint32_t stem = stemHash(name_);
  for (const Sidecar& sidecar : level.sidecars) {
    if (sidecar.stemHash == stem) return sidecar.sig;
  }
  return level.overflow ? library::SIDECAR_UNKNOWN : library::SIDECAR_NONE;
}

bool LibraryBuilder::stageBook(const Level& level, HalFile& entry) {
  const std::string path = childPath(level.path, name_);
  if (path.size() > library::MAX_STRING) return true;  // a path the index cannot hold is left out
  const auto size = static_cast<uint32_t>(entry.fileSize());
  const library::StagedBook book{LibraryKeys::bookIdentity(name_, size), packDate(entry), sidecarFor(level),
                                 pathsBytes_};
  if (!library::writeBlobString(paths_, path) || !library::writeExact(stage_, &book, sizeof(book))) return false;
  pathsBytes_ += static_cast<uint32_t>(sizeof(uint16_t) + path.size());
  ++found_;
  return true;
}

void LibraryBuilder::walkStep() {
  if (!started_ && !startWalk()) return fail("cannot start the walk");
  for (int budget = WALK_BUDGET; budget > 0 && !levels_.empty(); --budget) {
    Level& level = levels_.back();
    HalFile entry = level.dir.openNextFile();
    if (!entry) {
      if (!level.booksPass) {
        level.booksPass = true;  // the folder's sidecars are known: now its books
        level.dir.rewindDirectory();
      } else {
        levels_.pop_back();
      }
      continue;
    }
    entry.getName(name_, sizeof(name_));
    const bool isDir = entry.isDirectory();
    if (!listable(name_, levels_.size() == 1)) continue;
    if (!level.booksPass) {
      if (!isDir && isMetadataSidecar(name_)) noteSidecar(level, entry);
      continue;
    }
    if (isDir) {
      if (levels_.size() > MAX_DEPTH) continue;
      std::string child = childPath(level.path, name_);
      HalFile sub = Storage.open(child.c_str());
      if (!sub || !sub.isDirectory()) continue;
      sub.rewindDirectory();
      levels_.push_back(Level{std::move(sub), std::move(child)});  // reserved: `level` stays valid
      continue;
    }
    if (config_.isBook == nullptr || !config_.isBook(name_)) continue;
    if (!stageBook(level, entry)) return fail("cannot stage a book");
    if (found_ >= config_.maxBooks) {
      partial_ = true;
      levels_.clear();
    }
  }
  if (!levels_.empty()) return;
  if (!stage_.close() || !paths_.close()) return fail("cannot finish the walk");
  phase_ = Phase::Join;
}

void LibraryBuilder::join(BuildArena& arena) {
  LibraryJoin::Input in;
  in.stagePath = work("stage.bin");
  in.stageCount = found_;
  in.previousPath = config_.indexPath;
  in.resolveAll = config_.resolveAll;
  in.recordsPath = work("records.bin");
  in.namesPath = work("names.bin");
  LibraryJoin::Result result;
  if (!LibraryJoin::join(in, arena, result)) return fail("join");
  pending_ = result.pending;
  toResolve_ = result.pending;
  buildGen_ = result.buildGen;
  phase_ = Phase::Publish;
}

void LibraryBuilder::publish(BuildArena& arena) {
  LibraryPublish::Input in;
  in.recordsPath = work("records.bin");
  in.pathsPath = work("paths.bin");
  in.namesPath = work("names.bin");
  in.bookCount = found_;
  in.buildGen = buildGen_;
  in.acceptRules = config_.showHidden ? 1 : 0;
  in.partial = partial_;
  if (!LibraryPublish::publish(in, arena, config_.indexPath)) return fail("publish");
  published_ = true;
  sincePublish_ = 0;
  if (pending_ > 0 && cursor_ < found_) {
    phase_ = Phase::Resolve;
    return;
  }
  removeWorkingFiles();
  phase_ = Phase::Done;
}

bool LibraryBuilder::openResolveFiles() {
  if (!Storage.openFileForUpdate("LIB", work("records.bin"), records_) ||
      !Storage.openFileForRead("LIB", work("paths.bin"), paths_) ||
      !Storage.openFileForUpdate("LIB", work("names.bin"), names_)) {
    return false;
  }
  resolveOpen_ = true;
  return names_.seek(names_.fileSize());  // names are appended
}

void LibraryBuilder::closeResolveFiles() {
  records_.close();
  paths_.close();
  names_.close();
  resolveOpen_ = false;
}

void LibraryBuilder::resolveStep(BuildArena& arena) {
  if (!resolveOpen_ && !openResolveFiles()) return fail("cannot open the working files");
  library::BookRecord record{};
  while (cursor_ < found_) {
    if (!records_.seek(uint32_t{cursor_} * sizeof(record)) || !library::readExact(records_, &record, sizeof(record))) {
      return fail("cannot read a record");
    }
    if (record.authorHash == library::AUTHOR_PENDING) break;
    ++cursor_;
  }
  if (cursor_ >= found_) {  // every pending book has had its turn
    closeResolveFiles();
    phase_ = Phase::Publish;
    return;
  }
  std::string path;
  if (!paths_.seek(record.pathOff) || !library::readBlobString(paths_, path)) return fail("cannot read a path");
  uint32_t size = 0;
  {
    HalFile book = Storage.open(path.c_str());
    if (book) size = static_cast<uint32_t>(book.fileSize());
  }
  const uint16_t at = cursor_++;
  Author author;
  arena.reset();
  if (config_.resolve == nullptr || !config_.resolve(config_.resolveUser, path, size, author, &arena)) return;
  const uint32_t hash = LibraryKeys::authorHash(author.name);
  if (!records_.seek(uint32_t{at} * sizeof(record) + offsetof(library::BookRecord, authorHash)) ||
      !library::writeExact(records_, &hash, sizeof(hash))) {
    return fail("cannot patch a record");
  }
  if (hash != library::AUTHOR_UNKNOWN &&
      !library::appendNameEntry(names_, hash, !author.fileAs.empty(), author.name,
                                LibraryKeys::authorFilingName(author.name, author.fileAs))) {
    return fail("cannot record a name");
  }
  --pending_;
  ++resolved_;
  if (++sincePublish_ >= config_.republishEvery) {
    closeResolveFiles();
    phase_ = Phase::Publish;
  }
}

void LibraryBuilder::removeWorkingFiles() const {
  for (const char* name : {"stage.bin", "paths.bin", "records.bin", "names.bin"}) Storage.remove(work(name).c_str());
}
