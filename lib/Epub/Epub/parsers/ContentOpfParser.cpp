#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

#include "../BookMetadataCache.h"
#include "../htmlEntities.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_PAGEMAP[] = "application/oebps-page-map+xml";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";
constexpr size_t MAX_DESCRIPTION_LENGTH = 1024;

inline bool isTag(const char* name, const char* plainTag, const char* opfTag) {
  return strcmp(name, plainTag) == 0 || strcmp(name, opfTag) == 0;
}

inline bool isPackageTag(const char* name) { return isTag(name, "package", "opf:package"); }
inline bool isMetadataTag(const char* name) { return isTag(name, "metadata", "opf:metadata"); }
inline bool isManifestTag(const char* name) { return isTag(name, "manifest", "opf:manifest"); }
inline bool isSpineTag(const char* name) { return isTag(name, "spine", "opf:spine"); }
inline bool isGuideTag(const char* name) { return isTag(name, "guide", "opf:guide"); }
inline bool isMetaTag(const char* name) { return isTag(name, "meta", "opf:meta"); }
inline bool isItemTag(const char* name) { return isTag(name, "item", "opf:item"); }
inline bool isItemRefTag(const char* name) { return isTag(name, "itemref", "opf:itemref"); }
inline bool isReferenceTag(const char* name) { return isTag(name, "reference", "opf:reference"); }

bool startsWithImageMediaType(const char* mediaType) {
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  for (size_t i = 0; i < prefixLen; ++i) {
    // A shorter string mismatches on its NUL before this reads past the end.
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) {
      return false;
    }
  }
  return true;
}

// Everything the manifest handler needs to know about a media-type, computed once per
// DISTINCT value: items are only ever routed by these classes, never by the raw string.
enum class MediaClass : uint8_t { Other = 0, Image, Ncx, Css, PageMap };

MediaClass classifyMediaType(const char* mediaType) {
  if (mediaType == nullptr || *mediaType == '\0') return MediaClass::Other;
  if (startsWithImageMediaType(mediaType)) return MediaClass::Image;
  if (strcmp(mediaType, MEDIA_TYPE_NCX) == 0) return MediaClass::Ncx;
  if (strcmp(mediaType, MEDIA_TYPE_CSS) == 0) return MediaClass::Css;
  if (strcmp(mediaType, MEDIA_TYPE_PAGEMAP) == 0) return MediaClass::PageMap;
  return MediaClass::Other;
}

// True when `word` appears as a whole space-separated token in `props` (the OPF
// `properties` attribute format). Pointer scan — no std::string construction.
bool hasPropertyWord(const char* props, const char* word) {
  if (props == nullptr || *props == '\0') return false;
  const size_t wordLen = strlen(word);
  const char* p = props;
  while ((p = strstr(p, word)) != nullptr) {
    const bool startsToken = (p == props) || (p[-1] == ' ');
    const char after = p[wordLen];
    if (startsToken && (after == '\0' || after == ' ')) return true;
    p += wordLen;
  }
  return false;
}

// Append the UTF-8 encoding of a Unicode code point.
void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  } else if (cp <= 0xFFFF) {
    out += static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu));
    out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  } else {
    out += static_cast<char>(0xF0u | ((cp >> 18) & 0x07u));
    out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
    out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  }
}

// Resolve a numeric character reference (&#N; decimal or &#xN; hex) into UTF-8.
// `entity` points at the leading '&', `len` includes the trailing ';'. Returns
// false when the reference is malformed or out of range.
bool appendNumericEntity(std::string& out, const char* entity, size_t len) {
  // Shortest valid form is "&#0;" (len 4); entity[1] must be '#'.
  if (len < 4 || entity[1] != '#') {
    return false;
  }

  size_t p = 2;
  int base = 10;
  if (entity[p] == 'x' || entity[p] == 'X') {
    base = 16;
    ++p;
  }
  if (p >= len - 1) {  // no digits before ';'
    return false;
  }

  uint32_t cp = 0;
  for (; p < len - 1; ++p) {
    const char c = entity[p];
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return false;
    }
    cp = cp * static_cast<uint32_t>(base) + static_cast<uint32_t>(digit);
    if (cp > 0x10FFFF) {  // beyond Unicode range — bail rather than emit garbage
      return false;
    }
  }

  appendUtf8(out, cp);
  return true;
}

bool appendHtmlEntity(std::string& out, const std::string& html, size_t& i) {
  const size_t semi = html.find(';', i + 1);
  if (semi == std::string::npos) {
    return false;
  }

  const size_t len = semi - i + 1;
  const char* entity = html.data() + i;

  // Numeric character references (&#8212; / &#x2014;) are not in the named-entity
  // table; decode them directly. yxml already decodes these when they appear raw,
  // but Calibre-style descriptions often double-escape them (&amp;#8212;), so the
  // literal form reaches here after the outer &amp; is resolved.
  if (len > 3 && entity[1] == '#') {
    if (appendNumericEntity(out, entity, len)) {
      i = semi;
      return true;
    }
    return false;
  }

  // Metadata descriptions should collapse these to normal spaces; the chapter
  // renderer keeps NBSP because it has layout semantics there.
  if ((len == 6 && std::memcmp(entity, "&nbsp;", len) == 0) || (len == 6 && std::memcmp(entity, "&ensp;", len) == 0) ||
      (len == 6 && std::memcmp(entity, "&emsp;", len) == 0) ||
      (len == 8 && std::memcmp(entity, "&thinsp;", len) == 0)) {
    out += ' ';
    i = semi;
    return true;
  }

  const char* resolved = lookupHtmlEntity(entity, len);
  if (resolved == nullptr) {
    return false;
  }

  out += resolved;
  i = semi;
  return true;
}

// Strip HTML tags and collapse whitespace from a description string.
// Expat already decodes XML entities (&lt; → <), so we see raw angle brackets.
std::string stripHtml(const std::string& html) {
  std::string result;
  result.reserve(html.size());
  bool inTag = false;
  for (size_t i = 0; i < html.size(); ++i) {
    const char c = html[i];
    if (c == '<') {
      // Only treat as a tag if immediately followed (no space skip) by a tag-like character
      const size_t j = i + 1;
      if (j < html.size() &&
          (isalpha(static_cast<unsigned char>(html[j])) || html[j] == '/' || html[j] == '!' || html[j] == '?')) {
        inTag = true;
        // Ensure words don't merge when a tag is removed
        if (!result.empty() && result.back() != ' ') result += ' ';
      } else {
        result += c;
      }
    } else if (c == '>') {
      if (inTag) {
        inTag = false;
      } else {
        result += c;
      }
    } else if (!inTag) {
      if (c == '&') {
        // Decode HTML entities not covered by XML/Expat.
        if (!appendHtmlEntity(result, html, i)) {
          result += c;
        }
      } else if (c == '\n' || c == '\r' || c == '\t') {
        if (!result.empty() && result.back() != ' ') result += ' ';
      } else {
        result += c;
      }
    }
  }
  // Collapse consecutive spaces and trim trailing whitespace
  std::string out;
  out.reserve(result.size());
  bool lastSpace = false;
  for (char c : result) {
    if (c == ' ') {
      if (!lastSpace && !out.empty()) {
        out += ' ';
        lastSpace = true;
      }
    } else {
      out += c;
      lastSpace = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string trim(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && (in[start] == ' ' || in[start] == '\n' || in[start] == '\r' || in[start] == '\t')) {
    ++start;
  }
  size_t end = in.size();
  while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\n' || in[end - 1] == '\r' || in[end - 1] == '\t')) {
    --end;
  }
  return in.substr(start, end - start);
}
}  // namespace

bool ContentOpfParser::setup() {
  if (!saxParser_.init(this, startElement, endElement, characterData)) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!saxParser_.isActive()) return 0;

  stats.writeCalls++;
  stats.bytesParsed += size;

  remainingSize -= std::min(size, remainingSize);
  if (!saxParser_.feed(buffer, size)) {
    LOG_DBG("COF", "Parse error at line %d: %s", saxParser_.errorLine(), saxParser_.errorString());
    return 0;
  }
  if (remainingSize == 0) {
    if (!saxParser_.finalize()) {
      LOG_DBG("COF", "Parse error (finalize): %s", saxParser_.errorString());
      return 0;
    }
  }
  return size;
}

bool ContentOpfParser::resolveItemRefHrefWithIndex(const std::string& idref, std::string& href) {
  const uint32_t targetHash = fnvHash(idref);
  const uint16_t targetLen = static_cast<uint16_t>(idref.size());

  auto it = std::lower_bound(itemIndex.begin(), itemIndex.end(), ItemIndexEntry{targetHash, targetLen, 0},
                             [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
                               return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
                             });

  // Trust (hash, len): the duplicate scan at spine start guarantees the key is unique in this
  // index, so the first match IS the item — seek straight to its href, skipping the id
  // read-back-and-compare that used to cost a second string read per itemref (5.8 s of a
  // 1732-itemref spine pass was this loop's SD traffic). The buffered reader makes the seek
  // free when the href sits in the current window, which spine-follows-manifest order makes
  // the common case.
  if (it != itemIndex.end() && it->idHash == targetHash && it->idLen == targetLen) {
    itemReader_->seek(it->hrefOffset);
    return itemReader_->readString(href);
  }

  return false;
}

bool ContentOpfParser::resolveItemRefHrefLinearScan(const std::string& idref, std::string& href) {
  itemReader_->seek(0);
  const size_t itemStoreSize = tempItemStore.fileSize();
  std::string itemId;

  while (itemReader_->position() < itemStoreSize) {
    const size_t beforeReadPos = itemReader_->position();
    if (!itemReader_->readString(itemId) || !itemReader_->readString(href)) {
      return false;
    }

    // Guard against malformed temp data or host shims that don't signal EOF via available().
    if (itemReader_->position() <= beforeReadPos) {
      return false;
    }

    if (itemId == idref) {
      return true;
    }
  }

  return false;
}

bool ContentOpfParser::resolveSpineItemRefHref(const std::string& idref, std::string& href) {
  if (!itemReader_.has_value()) {
    return false;  // spine element never opened the item store — nothing to resolve against
  }
  if (useItemIndex) {
    return resolveItemRefHrefWithIndex(idref, href);
  }

  // Slow path: linear scan (for small manifests, keeps original behavior).
  return resolveItemRefHrefLinearScan(idref, href);
}

void ContentOpfParser::handleSpineItemRefElement(const char** atts) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "idref") == 0) {
      const std::string idref = atts[i + 1];
      std::string href;

      if (resolveSpineItemRefHref(idref, href) && cache) {
        cache->createSpineEntry(href);
      }
    }
  }
}

void ContentOpfParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->state == START && isPackageTag(name)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && isMetadataTag(name)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:title") == 0) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:language") == 0) {
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:description") == 0) {
    // Only capture the first dc:description element; subsequent ones are alternate/localized variants
    if (self->description.empty()) {
      self->state = IN_BOOK_DESCRIPTION;
    }
    return;
  }

  if (self->state == IN_PACKAGE && isManifestTag(name)) {
    self->state = IN_MANIFEST;
    if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    self->itemWriter_.emplace(self->tempItemStore);
    return;
  }

  if (self->state == IN_PACKAGE && isSpineTag(name)) {
    self->state = IN_SPINE;
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    self->itemReader_.emplace(self->tempItemStore);

    // Sort item index for binary search if we have enough items
    if (self->itemIndex.size() >= LARGE_SPINE_THRESHOLD) {
      std::sort(self->itemIndex.begin(), self->itemIndex.end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      // Lookups trust (hash, len) without reading the id back, so a (hash, len) duplicate —
      // a genuine 32-bit collision (~1e-4 odds per book) or a spec-invalid duplicated id —
      // could resolve to the wrong href. Adjacent scan after the sort: any duplicate disables
      // the index for this book and every idref falls back to the exact linear scan.
      bool duplicateKeys = false;
      for (size_t i = 1; i < self->itemIndex.size(); ++i) {
        if (self->itemIndex[i].idHash == self->itemIndex[i - 1].idHash &&
            self->itemIndex[i].idLen == self->itemIndex[i - 1].idLen) {
          duplicateKeys = true;
          break;
        }
      }
      if (duplicateKeys) {
        LOG_DBG("COF", "Manifest id hash collision; using exact linear idref lookup");
      } else {
        self->useItemIndex = true;
        LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex.size());
      }
      // (A manifest past MAX_INDEX_ENTRIES latched indexDisabled_ and CLEARED itemIndex above, so its
      // size is now < LARGE_SPINE_THRESHOLD and this whole block is skipped → linear scan. No branch
      // needed here.)
    }
    return;
  }

  if (self->state == IN_PACKAGE && isGuideTag(name)) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && isMetaTag(name)) {
    const char* metaName = nullptr;
    const char* metaContent = nullptr;
    const char* metaProperty = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0) {
        metaName = atts[i + 1];
      } else if (strcmp(atts[i], "content") == 0) {
        metaContent = atts[i + 1];
      } else if (strcmp(atts[i], "property") == 0) {
        metaProperty = atts[i + 1];
      }
    }

    if (metaName && metaContent) {
      if (strcmp(metaName, "cover") == 0) {
        self->coverItemId = metaContent;
      } else if (strcmp(metaName, "calibre:series") == 0 && self->series.empty()) {
        self->series = trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
      } else if (strcmp(metaName, "calibre:series_index") == 0 && self->seriesIndex.empty()) {
        self->seriesIndex =
            trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
      }
    }

    // EPUB 3 collection metadata:
    // <meta property="belongs-to-collection">Series Name</meta>  (character data)
    // <meta property="belongs-to-collection" content="Series Name"/>  (attribute, some generators)
    // <meta property="group-position">1</meta>
    if (metaProperty) {
      if (strcmp(metaProperty, "belongs-to-collection") == 0 && self->series.empty()) {
        if (metaContent) {
          self->series = trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
        } else {
          self->state = IN_BOOK_SERIES;
          return;
        }
      }
      if (strcmp(metaProperty, "group-position") == 0 && self->seriesIndex.empty()) {
        if (metaContent) {
          self->seriesIndex =
              trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
        } else {
          self->state = IN_BOOK_SERIES_INDEX;
          return;
        }
      }
    }

    return;
  }

  if (self->state == IN_MANIFEST && isItemTag(name)) {
    std::string itemId;
    std::string href;
    // media-type/properties are only INSPECTED, never stored: keep them as pointers into the
    // parser's attribute array (valid for this callback) instead of copying to std::string.
    // The dominant media type, application/xhtml+xml, is 21 chars — past SSO — so the old copy
    // heap-allocated once per item (~1500 alloc/free pairs on a large Calibre manifest).
    const char* mediaType = nullptr;
    const char* properties = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Manifests group items of one media type together (all chapters, then all images, ...),
    // so the previous item's classification almost always applies: one strcmp against the memo
    // replaces the per-item prefix/equality chain. A distinct value is classified once and
    // cached in the fixed member buffer (no heap; values longer than the buffer classify
    // per-item, which no real media type triggers).
    MediaClass mediaClass = MediaClass::Other;
    if (mediaType != nullptr) {
      if (strcmp(mediaType, self->lastMediaType_) == 0) {
        mediaClass = static_cast<MediaClass>(self->lastMediaClass_);
      } else {
        mediaClass = classifyMediaType(mediaType);
        if (strlen(mediaType) < sizeof(self->lastMediaType_)) {
          strcpy(self->lastMediaType_, mediaType);
          self->lastMediaClass_ = static_cast<uint8_t>(mediaClass);
        }
      }
    }

    // Write items down to SD card (buffered — thousands of tiny field writes otherwise cost a
    // full FsFile call each). The index entry records the position AFTER the id string:
    // hash-trusted lookups seek straight to the href and never re-read the id.
    self->itemWriter_->writeString(itemId);
    if (self->tempItemStore && !self->indexDisabled_) {
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.hrefOffset = self->itemWriter_->position();
      // Grow the in-RAM index with NOTHROW allocation. It keeps growing as long as the heap can hold
      // it (~12 bytes/item), so even a 1732-spine book keeps the fast binary-search index whenever
      // memory allows. Only if a growth genuinely can't be satisfied (OOM / fragmentation) do we
      // abandon the index and fall back to the exact linear scan over .items.bin — never abort (the
      // device is -fno-exceptions). No arbitrary item-count cap: memory is the only limit.
      if (!self->itemIndex.push_back(entry)) {
        self->indexDisabled_ = true;
        LOG_INF("COF", "manifest index OOM at %zu items; using linear idref lookup for this book",
                self->itemIndex.size());
        self->itemIndex.clear();  // free the partial index; lookups use the linear scan
      }
    }
    self->itemWriter_->writeString(href);

    if (itemId == self->coverItemId) {
      // Some EPUBs set meta name="cover" to an XHTML wrapper item.
      // Only treat it as a cover image when the manifest media-type is image/*.
      if (mediaClass == MediaClass::Image) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%s' with non-image media type: %s", itemId.c_str(),
                mediaType != nullptr ? mediaType : "(none)");
      }
    }

    switch (mediaClass) {
      case MediaClass::Ncx:
        if (self->tocNcxPath.empty()) {
          self->tocNcxPath = href;
        } else {
          LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
        }
        break;
      // EPUB 2.01 page-map.xml — separate top-level file mapping printed page numbers to spine
      // locations (e.g. <page name="1" href="OEBPS/c9_split_000.xhtml"/>). Spine references it
      // via <spine page-map="..."> but only the manifest item carries the canonical href.
      case MediaClass::PageMap:
        if (self->pageMapPath.empty()) {
          self->pageMapPath = href;
          LOG_DBG("COF", "Found EPUB 2.01 page-map: %s", href.c_str());
        }
        break;
      case MediaClass::Css:
        self->cssFiles.push_back(href);
        break;
      case MediaClass::Image:
      case MediaClass::Other:
        break;
    }

    // EPUB 3: Check for nav document (properties contains "nav" as a whole token)
    if (self->tocNavPath.empty() && hasPropertyWord(properties, "nav")) {
      self->tocNavPath = href;
      LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
    }

    // EPUB 3: Check for cover image (properties contains "cover-image" as a whole token)
    if (self->coverItemHref.empty() && hasPropertyWord(properties, "cover-image")) {
      self->coverItemHref = href;
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && isItemRefTag(name)) {
      self->handleSpineItemRefElement(atts);
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && isReferenceTag(name)) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      if (type == "text" || (type == "start" && !self->textReferenceHref.empty())) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void ContentOpfParser::characterData(void* userData, const char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    if (!self->author.empty()) {
      self->author.append(", ");  // Add separator for multiple authors
    }
    self->author.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_DESCRIPTION) {
    if (self->description.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->description.size();
      self->description.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }

  if (self->state == IN_BOOK_SERIES) {
    if (self->series.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->series.size();
      self->series.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }

  if (self->state == IN_BOOK_SERIES_INDEX) {
    if (self->seriesIndex.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->seriesIndex.size();
      self->seriesIndex.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }
}

void ContentOpfParser::endElement(void* userData, const char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->state == IN_SPINE && isSpineTag(name)) {
    self->state = IN_PACKAGE;
    self->itemReader_.reset();
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && isGuideTag(name)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && isManifestTag(name)) {
    self->state = IN_PACKAGE;
    // Flush buffered item records before the file closes; the spine pass reads them back.
    if (self->itemWriter_.has_value()) {
      self->itemWriter_->flush();
      self->itemWriter_.reset();
    }
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(name, "dc:title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(name, "dc:language") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_DESCRIPTION && strcmp(name, "dc:description") == 0) {
    self->description = stripHtml(self->description);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SERIES && isMetaTag(name)) {
    self->series = trim(self->series);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SERIES_INDEX && isMetaTag(name)) {
    self->seriesIndex = trim(self->seriesIndex);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && isMetadataTag(name)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && isPackageTag(name)) {
    self->state = START;
    return;
  }
}
