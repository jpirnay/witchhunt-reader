// Ours (writing modes step 2). Nothrow: chunks come from makeUniqueNoThrow, the vectors grow behind a heap check, and
// a refusal is a false return.
#include "ShapedStore.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "HashUtils.h"

namespace {
// The allocator's header and rounding on top of a request, as ParsedText::reserveWords allows for.
constexpr size_t kAllocSlack = 16;
}  // namespace

bool ShapedStore::heapAllows(const size_t bytes) { return ESP.getMaxAllocHeap() >= bytes + kAllocSlack; }

template <typename T>
bool ShapedStore::grow(std::vector<T>& v, const size_t extra) {
  if (v.capacity() >= v.size() + extra) return true;
  const size_t wanted = std::max<size_t>({16, v.capacity() * 2, v.size() + extra});
  // reserve() aborts under -fno-exceptions when the heap refuses; the new block lives beside the old one while it
  // moves, so both are asked for.
  if (!heapAllows(wanted * sizeof(T) + v.capacity() * sizeof(T))) return false;
  v.reserve(wanted);
  return true;
}

bool ShapedStore::allocChunk(Chunk& into, const size_t size) {
  std::unique_ptr<uint8_t[]> data;
  if (heapAllows(size)) data = makeUniqueNoThrow<uint8_t[]>(size);  // `size` bytes: one chunk, or one big piece's own
  if (!data) {
    if (!refusalLogged_) {
      refusalLogged_ = true;
      LOG_ERR("SHP", "Shaped store: no room for %u B; tokens are laid out as code points", static_cast<unsigned>(size));
    }
    return false;
  }
  into.data = std::move(data);
  into.size = static_cast<uint16_t>(size);
  into.used = 0;
  return true;
}

size_t ShapedStore::chunkBytes() const {
  size_t total = 0;
  for (size_t i = 0; i < nChunks_; i++) total += chunks_[i].size;
  return total;
}

// The empty chunk the next piece goes to, at least `size` bytes: the current one when it is empty, else the one after
// it (every chunk after the current is empty), appended when none is held. npos when the cap or the heap refuses.
size_t ShapedStore::acquire(const size_t size) {
  constexpr size_t kNone = static_cast<size_t>(-1);
  const size_t target = nChunks_ == 0 ? 0 : (chunks_[cur_].used == 0 ? cur_ : cur_ + 1);
  if (target >= kMaxChunks) {
    if (!refusalLogged_) {
      refusalLogged_ = true;
      LOG_ERR("SHP", "Shaped store: %u chunks held; tokens are laid out as code points",
              static_cast<unsigned>(kMaxChunks));
    }
    return kNone;
  }
  if (target < nChunks_) {
    if (chunks_[target].size < size) {
      Chunk bigger;
      if (!allocChunk(bigger, size)) return kNone;
      chunks_[target] = std::move(bigger);
    }
  } else {
    if (!allocChunk(chunks_[target], size)) return kNone;
    nChunks_ = target + 1;
  }
  cur_ = target;
  return target;
}

// Room for `extra` more bytes of the open piece. A piece is placed on its first byte, in the current chunk when it
// fits there; one that outgrows its chunk moves to a chunk of its own (grown in place when it begins its chunk).
bool ShapedStore::makeRoom(const size_t extra) {
  Piece& piece = pieces_.back();
  if (!placed_) {
    if (extra > UINT16_MAX) return false;
    if (nChunks_ > 0 && chunks_[cur_].size == kChunkBytes && kChunkBytes - chunks_[cur_].used >= extra) {
      piece.chunk = static_cast<uint8_t>(cur_);
      piece.at = chunks_[cur_].used;
    } else {
      const size_t chunk = acquire(std::max<size_t>(kChunkBytes, extra));
      if (chunk == static_cast<size_t>(-1)) return false;
      piece.chunk = static_cast<uint8_t>(chunk);
      piece.at = 0;
    }
    placed_ = true;
    return true;
  }
  Chunk& chunk = chunks_[piece.chunk];
  if (static_cast<size_t>(chunk.size - chunk.used) >= extra) return true;
  const size_t need = static_cast<size_t>(piece.bytes) + extra;
  if (need > UINT16_MAX) return false;
  if (piece.at == 0) {
    // Alone in its chunk: the chunk becomes the piece's own, twice as large for the pieces still to come.
    Chunk bigger;
    if (!allocChunk(bigger, std::max<size_t>(need, std::min<size_t>(UINT16_MAX, chunk.size * 2u)))) return false;
    memcpy(bigger.data.get(), chunk.data.get(), piece.bytes);
    bigger.used = chunk.used;
    chunk = std::move(bigger);
    return true;
  }
  const size_t target = acquire(std::max<size_t>(kChunkBytes, need));
  if (target == static_cast<size_t>(-1)) return false;
  // `chunk` is the one before the target: the piece leaves it, and its tail is free again.
  Chunk& from = chunks_[piece.chunk];
  memcpy(chunks_[target].data.get(), from.data.get() + piece.at, piece.bytes);
  from.used = piece.at;
  chunks_[target].used = piece.bytes;
  piece.chunk = static_cast<uint8_t>(target);
  piece.at = 0;
  return true;
}

bool ShapedStore::open(const size_t token, const std::string_view text, const uint32_t key) {
  if (pending_ || token > entries_.size() || text.size() > UINT32_MAX) return false;
  pendToken_ = token;
  pendStart_ = pieces_.size();
  pendHash_ = HashUtils::fnvHash64(text.data(), text.size());
  pendKey_ = key;
  pendLength_ = static_cast<uint32_t>(text.size());
  savedCur_ = cur_;
  savedUsed_ = nChunks_ > 0 ? chunks_[cur_].used : 0;
  placed_ = false;
  if (token < entries_.size()) entries_[token].valid = false;  // stale from here on, whatever happens to the rewrite
  pending_ = true;
  return true;
}

bool ShapedStore::beginPiece(const IndicShaper::PieceKind kind, const uint16_t begin, const uint16_t end) {
  if (!pending_ || !grow(pieces_, 1)) return false;
  Piece piece;
  piece.begin = begin;
  piece.end = end;
  piece.kind = kind;
  pieces_.push_back(piece);
  placed_ = false;
  return true;
}

bool ShapedStore::addGlyph(const GlyphRecord& record) {
  if (!pending_ || pieces_.size() == pendStart_ || pieces_.back().kind != IndicShaper::PieceKind::Glyphs) return false;
  uint8_t form[6];
  size_t length = 0;
  if (!glyph_run::encode(record, form, sizeof(form), &length)) return false;
  if (pieces_.back().count == UINT16_MAX || !makeRoom(length)) return false;
  Piece& piece = pieces_.back();  // makeRoom may have moved the piece, never the vector
  Chunk& chunk = chunks_[piece.chunk];
  memcpy(chunk.data.get() + piece.at + piece.bytes, form, length);
  piece.bytes = static_cast<uint16_t>(piece.bytes + length);
  piece.count++;
  chunk.used = static_cast<uint16_t>(chunk.used + length);
  return true;
}

bool ShapedStore::addBytes(const char* bytes, const size_t length) {
  if (!pending_ || pieces_.size() == pendStart_ || pieces_.back().kind != IndicShaper::PieceKind::Reordered)
    return false;
  if (length == 0) return true;
  if (!makeRoom(length)) return false;
  Piece& piece = pieces_.back();
  Chunk& chunk = chunks_[piece.chunk];
  memcpy(chunk.data.get() + piece.at + piece.bytes, bytes, length);
  piece.bytes = static_cast<uint16_t>(piece.bytes + length);
  chunk.used = static_cast<uint16_t>(chunk.used + length);
  return true;
}

void ShapedStore::eraseRange(Entry& entry) {
  if (entry.count == 0) return;
  const uint32_t begin = entry.begin;
  const uint32_t count = entry.count;
  pieces_.erase(pieces_.begin() + begin, pieces_.begin() + begin + count);
  for (Entry& other : entries_) {
    if (other.count > 0 && other.begin > begin) other.begin -= count;
  }
  entry.count = 0;
}

void ShapedStore::commit() {
  if (!pending_) return;
  if (pendToken_ == entries_.size()) {  // a scratch entry keeps nothing
    abandon();
    return;
  }
  Entry& entry = entries_[pendToken_];
  const uint32_t oldCount = entry.count;
  eraseRange(entry);  // the earlier pieces of this token; the open ones lie after every committed piece
  pendStart_ -= oldCount;
  entry.begin = static_cast<uint32_t>(pendStart_);
  entry.count = static_cast<uint32_t>(pieces_.size() - pendStart_);
  entry.hash = pendHash_;
  entry.key = pendKey_;
  entry.length = pendLength_;
  entry.valid = true;
  pending_ = false;
}

void ShapedStore::abandon() {
  if (!pending_) return;
  pieces_.resize(pendStart_);
  if (pendToken_ < entries_.size()) eraseRange(entries_[pendToken_]);
  // The chunks go back to what they held: bytes written since are free again.
  for (size_t i = 0; i < nChunks_; i++) {
    if (i == savedCur_)
      chunks_[i].used = savedUsed_;
    else if (i > savedCur_)
      chunks_[i].used = 0;
  }
  pending_ = false;
  removeEmptyOversize();
  recomputeCursor();
}

// Frees the chunks that hold no piece and are not chunk-sized (a big piece's own); the chunks after it move down.
void ShapedStore::removeEmptyOversize() {
  for (size_t i = 0; i < nChunks_;) {
    if (!oversize(i) || chunks_[i].used != 0) {
      i++;
      continue;
    }
    for (size_t j = i + 1; j < nChunks_; j++) chunks_[j - 1] = std::move(chunks_[j]);
    chunks_[--nChunks_] = Chunk();
    for (Piece& piece : pieces_) {
      if (piece.bytes > 0 && piece.chunk > i) piece.chunk--;
    }
  }
}

void ShapedStore::recomputeCursor() {
  cur_ = 0;
  for (size_t i = 0; i < nChunks_; i++) {
    if (chunks_[i].used > 0) cur_ = i;
  }
}

std::optional<std::span<const ShapedStore::Piece>> ShapedStore::pieces(const size_t token, const std::string_view text,
                                                                       const uint32_t key) const {
  if (token >= entries_.size()) return std::nullopt;
  const Entry& entry = entries_[token];
  if (!entry.valid || entry.length != text.size() || entry.key != key ||
      entry.hash != HashUtils::fnvHash64(text.data(), text.size())) {
    return std::nullopt;
  }
  if (entry.count == 0) return std::span<const Piece>();
  return std::span<const Piece>(pieces_.data() + entry.begin, entry.count);
}

void ShapedStore::setWidth(const size_t token, const size_t piece, const uint16_t width) {
  if (token >= entries_.size() || piece >= entries_[token].count) return;
  pieces_[entries_[token].begin + piece].width = width;
}

GlyphRun ShapedStore::glyphs(const Piece& piece) const {
  GlyphRun run;
  if (piece.kind != IndicShaper::PieceKind::Glyphs || piece.bytes == 0) return run;
  run.records = chunks_[piece.chunk].data.get() + piece.at;
  run.count = piece.count;
  run.bytes = piece.bytes;
  return run;
}

std::string_view ShapedStore::form(const Piece& piece) const {
  if (piece.kind != IndicShaper::PieceKind::Reordered || piece.bytes == 0) return {};
  return std::string_view(reinterpret_cast<const char*>(chunks_[piece.chunk].data.get() + piece.at), piece.bytes);
}

bool ShapedStore::insert(const size_t token) {
  if (pending_ || token > entries_.size() || !grow(entries_, 1)) return false;
  entries_.insert(entries_.begin() + token, Entry());
  return true;
}

void ShapedStore::erase(const size_t token) {
  if (pending_ || token >= entries_.size()) return;
  eraseRange(entries_[token]);
  entries_.erase(entries_.begin() + token);
}

void ShapedStore::dropFront(size_t count) {
  if (pending_) return;
  count = std::min(count, entries_.size());
  // The consumed entries' pieces are marked dead. Their ranges, sorted by begin and with a running total in `count`
  // (they are about to go), say how far each kept range moves down once the dead pieces are squeezed out.
  std::sort(entries_.begin(), entries_.begin() + count,
            [](const Entry& a, const Entry& b) { return a.begin < b.begin; });
  // Entries with no pieces carry no range: sorted by a stale begin they add 0 to the total, which is harmless.
  uint32_t dead = 0;
  for (size_t i = 0; i < count; i++) {
    Entry& entry = entries_[i];
    for (uint32_t p = 0; p < entry.count; p++) pieces_[entry.begin + p].chunk = kDead;
    dead += entry.count;
    entry.count = dead;
  }
  for (size_t i = count; i < entries_.size(); i++) {
    Entry& entry = entries_[i];
    if (entry.count == 0) continue;
    const auto before = std::partition_point(entries_.begin(), entries_.begin() + count,
                                             [&](const Entry& e) { return e.begin < entry.begin; });
    if (before != entries_.begin()) entry.begin -= (before - 1)->count;
  }
  entries_.erase(entries_.begin(), entries_.begin() + count);
  pieces_.erase(std::remove_if(pieces_.begin(), pieces_.end(), [](const Piece& p) { return p.chunk == kDead; }),
                pieces_.end());

  // The kept forms move down into the chunk-sized chunks, in the order they lie: each lands at or before where it was,
  // so nothing unread is overwritten. A big piece's own chunk stays where it is while a piece uses it.
  for (size_t i = 0; i < nChunks_; i++) chunks_[i].used = 0;
  size_t dest = 0;
  uint16_t destAt = 0;
  for (Piece& piece : pieces_) {
    if (piece.bytes == 0) continue;
    if (oversize(piece.chunk)) {
      chunks_[piece.chunk].used = static_cast<uint16_t>(piece.at + piece.bytes);
      continue;
    }
    while (dest < nChunks_ && (oversize(dest) || destAt + piece.bytes > kChunkBytes)) {
      dest++;
      destAt = 0;
    }
    uint8_t* from = chunks_[piece.chunk].data.get() + piece.at;
    uint8_t* to = chunks_[dest].data.get() + destAt;
    if (to != from) memmove(to, from, piece.bytes);
    piece.chunk = static_cast<uint8_t>(dest);
    piece.at = destAt;
    destAt = static_cast<uint16_t>(destAt + piece.bytes);
    chunks_[dest].used = destAt;
  }
  removeEmptyOversize();
  recomputeCursor();
}

bool ShapedStore::reserve(const size_t tokens, const size_t bytes) {
  if (!grow(entries_, tokens) || !grow(pieces_, tokens)) return false;
  size_t room = 0;
  for (size_t i = 0; i < nChunks_; i++) {
    if (!oversize(i)) room += chunks_[i].size - chunks_[i].used;
  }
  while (room < bytes && nChunks_ < kMaxChunks) {  // past the cap the estimate is simply not met
    if (!allocChunk(chunks_[nChunks_], kChunkBytes)) return false;
    nChunks_++;
    room += kChunkBytes;
  }
  return true;
}

void ShapedStore::reset() {
  pending_ = false;
  entries_.clear();
  pieces_.clear();
  for (size_t i = 0; i < nChunks_; i++) chunks_[i].used = 0;
  removeEmptyOversize();
  cur_ = 0;
}

void ShapedStore::release() {
  pending_ = false;
  std::vector<Entry>().swap(entries_);
  std::vector<Piece>().swap(pieces_);
  for (Chunk& chunk : chunks_) chunk = Chunk();
  nChunks_ = 0;
  cur_ = 0;
  refusalLogged_ = false;
}
