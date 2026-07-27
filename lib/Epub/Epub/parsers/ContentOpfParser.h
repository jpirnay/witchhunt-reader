#pragma once
#include <BufferedFileIO.h>
#include <Memory.h>  // makeUniqueNoThrow — nothrow growth (device is -fno-exceptions)
#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
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
  // In-RAM manifest fast index (sorted by (idHash,idLen) for binary-search idref lookup). Grown with
  // NOTHROW allocation (device is -fno-exceptions, so a throwing container abort()s on a fragmented
  // heap — observed on a 1732-spine book). It grows as long as memory allows; the FIRST growth that
  // can't be satisfied latches indexDisabled_ and idref lookups fall back to the exact linear scan
  // over .items.bin. So a big book keeps the fast index whenever the heap can hold it (~12 bytes/item),
  // and only genuinely-out-of-memory books degrade — no arbitrary item-count cap. The index is a pure
  // speed optimisation, never correctness. push_back returns false on OOM (never throws).
  struct ItemIndexVec {
    std::unique_ptr<ItemIndexEntry[]> data_;
    size_t size_ = 0;
    size_t cap_ = 0;
    bool push_back(const ItemIndexEntry& e) {
      if (size_ == cap_) {
        const size_t newCap = cap_ == 0 ? 64 : cap_ * 2;
        auto next = makeUniqueNoThrow<ItemIndexEntry[]>(newCap);
        if (!next) return false;  // OOM — caller disables the index, falls back to linear scan
        if (size_ > 0) std::memcpy(next.get(), data_.get(), size_ * sizeof(ItemIndexEntry));
        data_ = std::move(next);
        cap_ = newCap;
      }
      data_[size_++] = e;
      return true;
    }
    void clear() { data_.reset(); size_ = 0; cap_ = 0; }
    size_t size() const { return size_; }
    ItemIndexEntry* begin() { return data_.get(); }
    ItemIndexEntry* end() { return data_.get() + size_; }
    const ItemIndexEntry& operator[](size_t i) const { return data_[i]; }
  };
  ItemIndexVec itemIndex;
  bool useItemIndex = false;
  bool indexDisabled_ = false;  // latched when an index growth hit OOM → linear-scan fallback

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
