#include "FootnotePreviews.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SaxParser/SaxParser.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../Epub.h"

namespace {

constexpr uint32_t CACHE_MAGIC = 0x31504E46;  // "FNP1"
constexpr uint16_t CACHE_VERSION = 1;
constexpr size_t STREAM_CHUNK_BYTES = 1024;
constexpr size_t MAX_HREF_BYTES = 191;
constexpr size_t MAX_MARKER_BYTES = 15;

uint32_t fnv1a32(const char* s) {
  uint32_t h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

// Key = "<targetSpineIndex>#<fragment>": identical construction at gather and lookup
// time, so resolution differences can never desynchronise the two sides.
uint32_t makeKeyHash(const int spineIndex, const char* fragment) {
  char buf[16 + MAX_HREF_BYTES + 1];
  snprintf(buf, sizeof(buf), "%d#%s", spineIndex, fragment);
  return fnv1a32(buf);
}

bool isSpaceChar(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

const char* getAttribute(const char** atts, const char* name) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i] && atts[i + 1]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool hasAttributeToken(const char* value, const char* token) {
  if (!value) return false;
  const size_t tokenLen = strlen(token);
  const char* cursor = value;
  while (*cursor != '\0') {
    while (*cursor != '\0' && isSpaceChar(*cursor)) ++cursor;
    const char* start = cursor;
    while (*cursor != '\0' && !isSpaceChar(*cursor)) ++cursor;
    if (static_cast<size_t>(cursor - start) == tokenLen && strncmp(start, token, tokenLen) == 0) return true;
  }
  return false;
}

// True when the trimmed link text looks like a footnote marker: 1-4 codepoints, each a
// digit, *, dagger/double-dagger, section/pilcrow sign, dot or bracket. Catches "*",
// "12", "[3]", "†" — the shapes untagged reference links actually use — while a normal
// word link ("see chapter 2") never qualifies.
bool isMarkerText(const char* text, size_t len) {
  while (len > 0 && isSpaceChar(*text)) {
    ++text;
    --len;
  }
  while (len > 0 && isSpaceChar(text[len - 1])) --len;
  if (len == 0) return false;

  int codepoints = 0;
  size_t i = 0;
  while (i < len) {
    uint32_t cp = static_cast<unsigned char>(text[i]);
    size_t adv = 1;
    if (cp >= 0xF0 && i + 3 < len) {
      cp = ((cp & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
      adv = 4;
    } else if (cp >= 0xE0 && i + 2 < len) {
      cp = ((cp & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
      adv = 3;
    } else if (cp >= 0xC0 && i + 1 < len) {
      cp = ((cp & 0x1F) << 6) | (text[i + 1] & 0x3F);
      adv = 2;
    }
    const bool allowed = (cp >= '0' && cp <= '9') || cp == '*' || cp == '.' || cp == ',' || cp == '[' || cp == ']' ||
                         cp == '(' || cp == ')' || cp == 0x2020 /* † */ || cp == 0x2021 /* ‡ */ ||
                         cp == 0x00A7 /* § */ || cp == 0x00B6 /* ¶ */;
    if (!allowed) return false;
    ++codepoints;
    i += adv;
  }
  return codepoints >= 1 && codepoints <= 4;
}

struct Target {
  uint32_t keyHash;
  uint16_t spineIndex;
  std::string fragment;
};

// Pass A: SAX scan of one spine document collecting footnote-shaped link targets.
class LinkScanner {
  SaxParser parser_;
  const Epub& epub_;
  const int spineIndex_;
  std::vector<Target>& targets_;
  int depth_ = 0;
  int linkDepth_ = -1;
  bool linkIsNoteref_ = false;
  char href_[MAX_HREF_BYTES + 1] = {};
  char text_[MAX_MARKER_BYTES + 1] = {};
  size_t textLen_ = 0;
  bool textOverflow_ = false;

  static bool isInternalHref(const char* href) {
    if (!href || *href == '\0') return false;
    return strstr(href, "://") == nullptr && strncmp(href, "mailto:", 7) != 0 && strncmp(href, "javascript:", 11) != 0;
  }

  static void startElement(void* ctx, const char* name, const char** atts) {
    auto* self = static_cast<LinkScanner*>(ctx);
    if (self->linkDepth_ < 0 && strcmp(name, "a") == 0) {
      const char* href = getAttribute(atts, "href");
      if (isInternalHref(href) && strchr(href, '#') != nullptr && strlen(href) <= MAX_HREF_BYTES) {
        self->linkDepth_ = self->depth_;
        strncpy(self->href_, href, MAX_HREF_BYTES);
        self->href_[MAX_HREF_BYTES] = '\0';
        self->linkIsNoteref_ = hasAttributeToken(getAttribute(atts, "epub:type"), "noteref") ||
                               hasAttributeToken(getAttribute(atts, "role"), "doc-noteref");
        self->textLen_ = 0;
        self->textOverflow_ = false;
      }
    }
    ++self->depth_;
  }

  static void characterData(void* ctx, const char* text, const int length) {
    auto* self = static_cast<LinkScanner*>(ctx);
    if (self->linkDepth_ < 0) return;
    for (int i = 0; i < length; ++i) {
      if (self->textLen_ >= MAX_MARKER_BYTES) {
        self->textOverflow_ = true;  // too long to be a marker; noteref tagging may still qualify
        return;
      }
      self->text_[self->textLen_++] = text[i];
    }
  }

  static void endElement(void* ctx, const char*) {
    auto* self = static_cast<LinkScanner*>(ctx);
    --self->depth_;
    if (self->linkDepth_ != self->depth_) return;
    self->text_[self->textLen_] = '\0';
    const bool qualifies = self->linkIsNoteref_ || (!self->textOverflow_ && isMarkerText(self->text_, self->textLen_));
    if (qualifies && self->targets_.size() < FootnotePreviews::MAX_ENTRIES) {
      const char* hash = strchr(self->href_, '#');
      const char* fragment = hash + 1;  // '#' presence checked at startElement
      if (*fragment != '\0') {
        const int targetSpine =
            self->href_[0] == '#' ? self->spineIndex_ : self->epub_.resolveHrefToSpineIndex(self->href_);
        if (targetSpine >= 0) {
          const uint32_t keyHash = makeKeyHash(targetSpine, fragment);
          const bool seen = std::any_of(self->targets_.begin(), self->targets_.end(),
                                        [&](const Target& t) { return t.keyHash == keyHash; });
          if (!seen) {
            self->targets_.push_back({keyHash, static_cast<uint16_t>(targetSpine), std::string(fragment)});
          }
        }
      }
    }
    self->linkDepth_ = -1;
  }

 public:
  LinkScanner(const Epub& epub, const int spineIndex, std::vector<Target>& targets)
      : epub_(epub), spineIndex_(spineIndex), targets_(targets) {}
  // Scans chapter XHTML (HTML-flavored): enable bare-void-tag repair.
  bool setup() { return parser_.init(this, startElement, endElement, characterData, nullptr, true); }
  bool feed(const uint8_t* data, const size_t size) { return parser_.feed(data, size); }
  void finalize() { parser_.finalize(); }
};

// Pass B: SAX scan of one target spine document capturing note text at wanted anchors.
// Handles both real-world shapes:
//  - container id (<p id="n3">text</p>, <aside epub:type="rearnote" id=...>): subtree text.
//  - empty inline anchor (<a id="filepos123"/>text…</p>, Calibre/MOBI): the subtree
//    yields nothing, so capture continues past the anchor until its parent block closes
//    or the NEXT wanted anchor starts (sequential rearnote lists).
class NoteCapturer {
 public:
  using EmitFn = std::function<void(size_t targetIdx, const char* text, size_t len)>;

 private:
  SaxParser parser_;
  const std::vector<Target>& targets_;
  const std::vector<size_t>& wanted_;  // indexes into targets_, all for this spine
  EmitFn emit_;
  int depth_ = 0;
  int captureDepth_ = -1;  // depth of the id'd element while capturing, -1 = idle
  bool tailMode_ = false;  // id element closed short; capturing following siblings
  size_t activeIdx_ = 0;   // targets_ index being captured
  char text_[FootnotePreviews::MAX_TEXT_BYTES + 1] = {};
  size_t textLen_ = 0;
  bool truncated_ = false;

  int findWanted(const char* id) const {
    if (!id || *id == '\0') return -1;
    for (const size_t idx : wanted_) {
      if (targets_[idx].fragment == id) return static_cast<int>(idx);
    }
    return -1;
  }

  void beginCapture(const size_t targetIdx, const int idDepth) {
    activeIdx_ = targetIdx;
    captureDepth_ = idDepth;
    tailMode_ = false;
    textLen_ = 0;
    truncated_ = false;
  }

  void finishCapture() {
    while (textLen_ > 0 && text_[textLen_ - 1] == ' ') --textLen_;
    if (truncated_ && textLen_ >= 3) {
      text_[textLen_ - 3] = '.';
      text_[textLen_ - 2] = '.';
      text_[textLen_ - 1] = '.';
    }
    text_[textLen_] = '\0';
    if (textLen_ > 0) emit_(activeIdx_, text_, textLen_);
    captureDepth_ = -1;
    tailMode_ = false;
  }

  static void startElement(void* ctx, const char*, const char** atts) {
    auto* self = static_cast<NoteCapturer*>(ctx);
    const int wantedIdx = self->findWanted(getAttribute(atts, "id"));
    if (wantedIdx >= 0) {
      // A new wanted anchor always starts its own capture — in sequential rearnote
      // lists it is also what terminates the previous note's tail capture.
      if (self->captureDepth_ >= 0) self->finishCapture();
      self->beginCapture(static_cast<size_t>(wantedIdx), self->depth_);
    }
    ++self->depth_;
  }

  static void characterData(void* ctx, const char* text, const int length) {
    auto* self = static_cast<NoteCapturer*>(ctx);
    if (self->captureDepth_ < 0) return;
    for (int i = 0; i < length; ++i) {
      const bool space = isSpaceChar(text[i]);
      if (space && (self->textLen_ == 0 || self->text_[self->textLen_ - 1] == ' ')) continue;
      if (self->textLen_ >= FootnotePreviews::MAX_TEXT_BYTES) {
        self->truncated_ = true;
        return;
      }
      self->text_[self->textLen_++] = space ? ' ' : text[i];
    }
  }

  static void endElement(void* ctx, const char*) {
    auto* self = static_cast<NoteCapturer*>(ctx);
    --self->depth_;
    if (self->captureDepth_ < 0) return;
    if (!self->tailMode_ && self->depth_ == self->captureDepth_) {
      // The id'd element itself closed. Enough subtree text = container pattern, done;
      // near-empty = inline anchor pattern, keep capturing the following siblings.
      if (self->textLen_ >= FootnotePreviews::MIN_SUBTREE_BYTES || self->truncated_) {
        self->finishCapture();
      } else {
        self->tailMode_ = true;
      }
    } else if (self->depth_ < self->captureDepth_) {
      // Parent of the anchor closed — the enclosing block is over either way.
      self->finishCapture();
    }
  }

 public:
  NoteCapturer(const std::vector<Target>& targets, const std::vector<size_t>& wanted, EmitFn emit)
      : targets_(targets), wanted_(wanted), emit_(std::move(emit)) {}
  // Scans chapter XHTML (HTML-flavored): enable bare-void-tag repair.
  bool setup() { return parser_.init(this, startElement, endElement, characterData, nullptr, true); }
  bool feed(const uint8_t* data, const size_t size) { return parser_.feed(data, size); }
  void finalize() {
    parser_.finalize();
    if (captureDepth_ >= 0) finishCapture();  // note ran to end-of-document
  }
};

// Streams one spine entry through a SAX consumer. Returns false on ZIP/read errors;
// SAX-level errors are tolerated (consumer keeps whatever it saw — same policy as the
// section parser, which survives loose real-world HTML).
template <typename Consumer>
bool streamSpineEntry(ZipFile& zip, const std::string& href, uint8_t* chunk, Consumer& consumer) {
  ZipFile::EntryReader reader(zip, STREAM_CHUNK_BYTES);
  if (!reader.open(FsHelpers::normalisePath(href).c_str())) {
    return false;
  }
  bool done = false;
  while (!done) {
    size_t produced = 0;
    if (!reader.step(chunk, STREAM_CHUNK_BYTES, &produced, &done)) {
      return false;
    }
    if (produced > 0 && !consumer.feed(chunk, produced)) {
      break;  // malformed markup mid-stream: keep partial results
    }
  }
  consumer.finalize();
  return true;
}

}  // namespace

namespace FootnotePreviews {

bool cacheExists(const std::string& bookCachePath) { return Storage.exists((bookCachePath + CACHE_FILENAME).c_str()); }

bool gather(Epub& epub, const std::function<void(int)>& progressFn) {
  const std::string cachePath = epub.getCachePath() + CACHE_FILENAME;
  const int spineCount = epub.getSpineItemsCount();
  const uint32_t startMs = millis();

  auto chunk = makeUniqueNoThrow<uint8_t[]>(STREAM_CHUNK_BYTES);
  if (!chunk) {
    LOG_ERR("FNP", "OOM: cannot allocate %u-byte stream chunk", static_cast<uint32_t>(STREAM_CHUNK_BYTES));
    return false;
  }

  ZipFile zip(epub.getPath());

  // Pass A: collect footnote-shaped link targets from every spine document.
  int spinesScanned = 0;
  std::vector<Target> targets;
  targets.reserve(64);  // typical books have dozens of notes; grows to MAX_ENTRIES worst case
  for (int i = 0; i < spineCount; ++i) {
    LinkScanner scanner(epub, i, targets);
    if (!scanner.setup()) {
      LOG_ERR("FNP", "Link scanner setup failed (spine=%d)", i);
      return false;
    }
    if (!streamSpineEntry(zip, epub.getSpineItem(i).href, chunk.get(), scanner)) {
      LOG_ERR("FNP", "Failed to stream spine %d for link scan", i);
      // Unreadable entry: skip it; other chapters' notes still gather.
    } else {
      ++spinesScanned;
    }
    if (progressFn) progressFn((i + 1) * 60 / spineCount);
  }

  // Every spine failed to open — almost certainly transient heap exhaustion (the ZIP reader
  // could not even get its 4 KB EOCD buffer), not a book without notes. Writing a cache here
  // would record "0 previews" as a permanent, authoritative answer: cacheExists() would report
  // ready on every later open, so the book's footnotes would stay dead until someone deleted
  // the cache directory. Fail instead, so the caller can retry when there is memory again.
  if (spineCount > 0 && spinesScanned == 0) {
    LOG_ERR("FNP", "No spine could be read (0/%d); not writing a cache", spineCount);
    return false;
  }

  // Pass B: capture the note text at each wanted anchor, one stream per target spine,
  // writing the blob as we go. Header is a placeholder until count/indexOffset are known.
  FsFile out;
  if (!Storage.openFileForWrite("FNP", cachePath, out)) {
    LOG_ERR("FNP", "Cannot open %s for write", cachePath.c_str());
    return false;
  }
  serialization::writePod(out, CACHE_MAGIC);
  serialization::writePod(out, CACHE_VERSION);
  serialization::writePod(out, static_cast<uint16_t>(0));
  serialization::writePod(out, static_cast<uint32_t>(0));

  std::vector<std::pair<uint32_t, uint32_t>> index;  // keyHash -> blobOffset
  index.reserve(targets.size());

  std::vector<uint16_t> spines;
  spines.reserve(targets.size());
  for (const Target& t : targets) {
    if (std::find(spines.begin(), spines.end(), t.spineIndex) == spines.end()) spines.push_back(t.spineIndex);
  }
  std::sort(spines.begin(), spines.end());

  for (size_t s = 0; s < spines.size(); ++s) {
    std::vector<size_t> wanted;
    for (size_t t = 0; t < targets.size(); ++t) {
      if (targets[t].spineIndex == spines[s]) wanted.push_back(t);
    }
    NoteCapturer capturer(targets, wanted, [&](const size_t targetIdx, const char* text, const size_t len) {
      const uint32_t offset = static_cast<uint32_t>(out.position());
      serialization::writePod(out, static_cast<uint16_t>(len));
      out.write(reinterpret_cast<const uint8_t*>(text), len);
      index.emplace_back(targets[targetIdx].keyHash, offset);
    });
    if (!capturer.setup()) {
      LOG_ERR("FNP", "Note capturer setup failed (spine=%u)", spines[s]);
      break;
    }
    if (!streamSpineEntry(zip, epub.getSpineItem(spines[s]).href, chunk.get(), capturer)) {
      LOG_ERR("FNP", "Failed to stream spine %u for note capture", spines[s]);
    }
    if (progressFn) progressFn(60 + static_cast<int>((s + 1) * 39 / spines.size()));
  }

  std::sort(index.begin(), index.end());
  const uint32_t indexOffset = static_cast<uint32_t>(out.position());
  for (const auto& [hash, offset] : index) {
    serialization::writePod(out, hash);
    serialization::writePod(out, offset);
  }
  if (!out.seekSet(0)) {
    out.close();
    Storage.remove(cachePath.c_str());
    LOG_ERR("FNP", "Header rewrite seek failed; cache discarded");
    return false;
  }
  serialization::writePod(out, CACHE_MAGIC);
  serialization::writePod(out, CACHE_VERSION);
  serialization::writePod(out, static_cast<uint16_t>(index.size()));
  serialization::writePod(out, indexOffset);
  out.close();

  LOG_INF("FNP", "Gathered %u footnote previews (%u link targets, %u note files) in %lums",
          static_cast<uint32_t>(index.size()), static_cast<uint32_t>(targets.size()),
          static_cast<uint32_t>(spines.size()), millis() - startMs);
  if (progressFn) progressFn(100);
  return true;
}

bool Lookup::open(const std::string& bookCachePath, const Epub* epub, const int currentSpineIndex) {
  entryCount_ = 0;
  epub_ = epub;
  currentSpineIndex_ = currentSpineIndex;
  if (!Storage.openFileForRead("FNP", bookCachePath + CACHE_FILENAME, file_)) {
    return false;
  }
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t count = 0;
  uint32_t indexOffset = 0;
  serialization::readPod(file_, magic);
  serialization::readPod(file_, version);
  serialization::readPod(file_, count);
  serialization::readPod(file_, indexOffset);
  const uint32_t expectedEnd = indexOffset + static_cast<uint32_t>(count) * sizeof(IndexEntry);
  if (magic != CACHE_MAGIC || version != CACHE_VERSION || count > MAX_ENTRIES || expectedEnd != file_.size()) {
    LOG_ERR("FNP", "Invalid footnotes.bin (magic=%08lx version=%u count=%u); previews skipped",
            static_cast<unsigned long>(magic), version, count);
    file_.close();
    return false;
  }
  if (count == 0) {
    file_.close();
    return false;  // valid-but-empty: nothing to look up, keep the parser fast path off
  }
  index_ = makeUniqueNoThrow<IndexEntry[]>(count);  // 8 B/entry, <= 4 KB at MAX_ENTRIES
  if (!index_) {
    LOG_ERR("FNP", "OOM: cannot allocate %u-entry preview index", count);
    file_.close();
    return false;
  }
  if (!file_.seekSet(indexOffset) || file_.read(reinterpret_cast<uint8_t*>(index_.get()), count * sizeof(IndexEntry)) !=
                                         static_cast<int>(count * sizeof(IndexEntry))) {
    LOG_ERR("FNP", "Failed to read preview index");
    index_.reset();
    file_.close();
    return false;
  }
  entryCount_ = count;
  return true;
}

bool Lookup::find(const char* href, std::string& outText) {
  if (entryCount_ == 0 || !href) return false;
  const char* hash = strchr(href, '#');
  if (!hash || hash[1] == '\0') return false;
  int targetSpine = currentSpineIndex_;
  if (href[0] != '#') {
    if (!epub_) return false;
    targetSpine = epub_->resolveHrefToSpineIndex(href);
    if (targetSpine < 0) return false;
  }
  const uint32_t keyHash = makeKeyHash(targetSpine, hash + 1);

  int lo = 0, hi = entryCount_ - 1;
  uint32_t blobOffset = 0;
  bool found = false;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    if (index_[mid].keyHash == keyHash) {
      blobOffset = index_[mid].blobOffset;
      found = true;
      break;
    }
    if (index_[mid].keyHash < keyHash) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  if (!found) return false;

  uint16_t len = 0;
  if (!file_.seekSet(blobOffset)) return false;
  serialization::readPod(file_, len);
  if (len == 0 || len > MAX_TEXT_BYTES) return false;
  outText.resize(len);
  return file_.read(reinterpret_cast<uint8_t*>(&outText[0]), len) == static_cast<int>(len);
}

}  // namespace FootnotePreviews
