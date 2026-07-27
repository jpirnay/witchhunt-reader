#pragma once
#include <BufferedFileIO.h>
#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <vector>

#include "Epub.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_BOOK_DESCRIPTION,
    IN_BOOK_SERIES,
    IN_BOOK_SERIES_INDEX,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  SaxParser saxParser_;
  ParserState state = START;
  BookMetadataCache* cache;
  FsFile tempItemStore;
  // Buffered views over tempItemStore, alive one phase at a time: the writer during the
  // manifest pass (thousands of tiny id/href field writes), the reader during the spine pass
  // (one seek + string read per itemref, usually landing inside the current 4 KB window since
  // spine order tracks manifest order). Each internally degrades to pass-through on OOM.
  std::optional<serialization::BufferedFileWriter> itemWriter_;
  std::optional<serialization::BufferedFileReader> itemReader_;
  std::string coverItemId;

  // Index for fast idref→href lookup (used only for large EPUBs).
  // Lookups trust (idHash, idLen) alone and read the href directly — the id string is never
  // read back for verification. Hash-trusted matching adapted from the FreeInk SDK's
  // BookCatalog (github.com/Free-Ink/freeink-sdk, "Add SD-backed catalog for large EPUB
  // containers" by Justin Mitchell), whose rationale applies verbatim: the strings a verify
  // would need are exactly the RAM/IO this index exists to avoid. Correctness is guaranteed
  // by a duplicate scan after the sort: if any two manifest ids share (hash, len), the index
  // is disabled for the whole book and lookups fall back to the exact linear scan.
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t hrefOffset;  // offset of the href string in .items.bin (record's id skipped)
  };
  std::deque<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;
  // Latched when the manifest exceeds MAX_INDEX_ENTRIES: the in-RAM fast index is abandoned for this
  // book (dropped) and idref lookups use the exact linear scan over .items.bin. The device build is
  // -fno-exceptions, so letting the deque grow until a chunk alloc fails would ABORT the firmware
  // (observed on a 1732-spine book); the cap prevents that. The index is a pure speed optimisation,
  // never correctness. Chosen well above normal books (~hundreds of items) and far below heap trouble.
  bool indexDisabled_ = false;
  static constexpr size_t MAX_INDEX_ENTRIES = 1200;

  // Memo of the last manifest item's media-type and its classification (MediaClass enum in the
  // .cpp, stored as its uint8_t value here). Manifest items overwhelmingly repeat one media type
  // (all chapters share application/xhtml+xml, images share image/jpeg), so the common case is a
  // single strcmp hit instead of re-classifying — and the raw attribute is never copied into a
  // per-item std::string. Fixed buffer, zero heap; 48 covers every registered EPUB media type.
  char lastMediaType_[48] = {};
  uint8_t lastMediaClass_ = 0;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const char* name, const char** atts);
  static void characterData(void* userData, const char* s, int len);
  static void endElement(void* userData, const char* name);

  bool resolveItemRefHrefWithIndex(const std::string& idref, std::string& href);
  bool resolveItemRefHrefLinearScan(const std::string& idref, std::string& href);
  bool resolveSpineItemRefHref(const std::string& idref, std::string& href);
  void handleSpineItemRefElement(const char** atts);

 public:
  struct Stats {
    uint32_t writeCalls = 0;
    uint32_t bytesParsed = 0;
    uint32_t parseBufferMs = 0;
    uint32_t manifestOpenMs = 0;
    uint32_t spineOpenMs = 0;
    uint32_t guideOpenMs = 0;
    uint32_t itemRefCount = 0;
    uint32_t itemRefLookupMs = 0;
    uint32_t createSpineEntryMs = 0;
  } stats;

  std::string title;
  std::string author;
  std::string language;
  std::string description;
  std::string series;
  std::string seriesIndex;
  std::string tocNcxPath;
  std::string tocNavPath;   // EPUB 3 nav document path
  std::string pageMapPath;  // EPUB 2.01 page-map.xml document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
