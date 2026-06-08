#include <cstring>
#include <new>

#include "SaxParser/SaxParser.h"
#include "yxml.h"

// ---------------------------------------------------------------------------
// Capacity constants — all fixed, no heap allocation after init().
//
// Derived from measured maximums across 57 real epub files (2326 XML files):
//
//   kStackSize    yxml internal name stack (element path + attr/PI name).
//                 Holds NUL-separated element path. kMaxDepth(64) ×
//                 kElemNameLen(32) = 2048 B worst case; 2048 is exact fit.
//   kElemNameLen  longest seen: "enc:EncryptionMethod" = 20,
//                 "preserveAspectRatio" = 19 → 32 (1.6× headroom)
//   kAttrValueLen meaningful values (epub path / OPDS URL) fit in 256, but
//                 inline style="..." on content HTML and data: URIs in src can
//                 exceed it. The corpus max for a *consumed* value was a 517-char
//                 Calibre JSON blob we never read, so 384 is chosen as a safety
//                 margin for real style/href values without budgeting the blob.
//                 A value past the cap is truncated and flagged (kTruncAttrValue).
//   kMaxAttrs     corpus max was 8 (alice-illustrated content.opf), but that
//                 corpus under-samples attribute-heavy content HTML: a single
//                 <td>/<a>/<span> can carry class+style+id+colspan+rowspan+
//                 role+aria-*+epub:type, and ChapterHtmlSlimParser *consumes*
//                 id/class/style/colspan/epub:type — dropping the 9th+ attr
//                 silently drifts anchors, table layout and styling. 12 gives
//                 1.5× headroom over the observed max for the realistic case.
//                 Excess attrs are dropped and flagged (kTruncMaxAttrs).
//   kMaxDepth     max seen: 48 (deeply nested HTML in Manticore ACTD_split).
//                 ChapterHtmlSlimParser feeds content HTML through SaxParser,
//                 so this bound applies. 64 gives 1.3× headroom.
//   kCharBufLen   text nodes up to 8730 chars seen (content.opf long
//                 description), but charCb flushes mid-node when full so
//                 callers already handle fragmented delivery. 256 batches
//                 most structural nodes in one call.
//
// RAM note: SaxParserImpl is heap-allocated once per parse and freed at reset().
// The attr table dominates: attrs[kMaxAttrs] = kMaxAttrs*(kElemNameLen +
// kAttrValueLen + 8). At 12*(32+384+8) ≈ 5.0 KB (was 8*(32+256+8) ≈ 2.3 KB),
// a ~2.7 KB transient bump. For chapter parsing this lands while the secondary
// framebuffer is released (~52 KB headroom), so it does not tighten the hot path.
// ---------------------------------------------------------------------------
static constexpr size_t kStackSize    = 2048;
static constexpr size_t kElemNameLen  =   32;
static constexpr size_t kAttrValueLen =  384;
static constexpr size_t kMaxAttrs     =   12;
static constexpr size_t kMaxDepth     =   64;
static constexpr size_t kCharBufLen   =  256;

// ---------------------------------------------------------------------------
// Internal state — entirely stack/struct allocated, zero heap after new.
// ---------------------------------------------------------------------------
struct AttrPair {
  char   name [kElemNameLen];
  char   value[kAttrValueLen];
  size_t valueLen = 0;  // cursor: avoids strlen() on each ATTRVAL character
};

struct SaxParserImpl {
  yxml_t        x;
  unsigned char yxmlStack[kStackSize];

  SaxStartCb   startCb   = nullptr;
  SaxEndCb     endCb     = nullptr;
  SaxCharCb    charCb    = nullptr;
  SaxDefaultCb defaultCb = nullptr;
  void*        userData  = nullptr;

  // Character-data accumulation; flushed before every start/end event and at
  // the end of each text node (ELEMEND).
  char   charBuf[kCharBufLen];
  size_t charLen = 0;

  // Opening-tag accumulation: collected between ELEMSTART and the first
  // non-ATTR token, then fired as a single startCb call.
  char     pendingElem[kElemNameLen];  // element name waiting to be fired
  AttrPair attrs[kMaxAttrs];
  size_t   attrCount    = 0;
  bool     inOpeningTag = false;

  // Element-name stack for end-tag callbacks (yxml's x.elem points to the
  // parent at ELEMEND time, not the element just closed).
  char   elemStack[kMaxDepth][kElemNameLen];
  size_t elemDepth = 0;

  // Running byte offset (updated once per yxml_parse call).
  uint32_t byteOffset = 0;

  // Bitmask of SaxParser::TruncationFlag values — records which fixed-capacity
  // limits were hit (and silently truncated) over the lifetime of the parse.
  uint32_t truncFlags = 0;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void flushChar(SaxParserImpl* impl) {
  if (impl->charCb && impl->charLen > 0) {
    impl->charCb(impl->userData, impl->charBuf,
                 static_cast<int>(impl->charLen));
  }
  impl->charLen = 0;
}

// Append a NUL-terminated string to charBuf, flushing when full.
static void appendChar(SaxParserImpl* impl, const char* s) {
  for (; *s; ++s) {
    if (impl->charLen == kCharBufLen) flushChar(impl);
    impl->charBuf[impl->charLen++] = *s;
  }
}

static void fireStart(SaxParserImpl* impl) {
  // Push element name so ELEMEND knows what to report.
  if (impl->elemDepth < kMaxDepth) {
    strncpy(impl->elemStack[impl->elemDepth], impl->pendingElem, kElemNameLen - 1);
    impl->elemStack[impl->elemDepth][kElemNameLen - 1] = '\0';
    ++impl->elemDepth;
  } else {
    impl->truncFlags |= SaxParser::kTruncMaxDepth;
  }

  if (impl->startCb) {
    // Build flat key/value pointer array on the stack.
    const char* atts[kMaxAttrs * 2 + 1];
    size_t n = impl->attrCount;
    for (size_t i = 0; i < n; ++i) {
      atts[i * 2]     = impl->attrs[i].name;
      atts[i * 2 + 1] = impl->attrs[i].value;
    }
    atts[n * 2] = nullptr;
    impl->startCb(impl->userData, impl->pendingElem, atts);
  }

  impl->attrCount   = 0;
  impl->inOpeningTag = false;
}

// ---------------------------------------------------------------------------
// SaxParser implementation
// ---------------------------------------------------------------------------

void SaxParser::reset() {
  if (!impl_) return;
  delete static_cast<SaxParserImpl*>(impl_);
  impl_ = nullptr;
}

SaxParser::~SaxParser() { reset(); }

bool SaxParser::init(void* userData, SaxStartCb startCb, SaxEndCb endCb,
                     SaxCharCb charCb, SaxDefaultCb defaultCb) {
  reset();
  stopped_     = false;
  errorLine_   = 0;
  errorString_ = nullptr;

  // nothrow + null-check: firmware builds with -fno-exceptions, so a bare new
  // would abort() on OOM instead of letting init() honour its "returns false on
  // allocation failure" contract. SaxParserImpl is ~10 KB (attr table + stacks),
  // large enough to fail under heap fragmentation during a section build.
  auto* impl = new (std::nothrow) SaxParserImpl;
  if (!impl) {
    errorString_ = "SaxParser: out of memory allocating parser state";
    return false;
  }
  impl->startCb   = startCb;
  impl->endCb     = endCb;
  impl->charCb    = charCb;
  impl->defaultCb = defaultCb;
  impl->userData  = userData;

  yxml_init(&impl->x, impl->yxmlStack, kStackSize);
  impl_ = impl;
  return true;
}

bool SaxParser::feed(const uint8_t* buf, size_t len) {
  if (!impl_) return false;
  if (!buf && len > 0) return false;
  auto* impl = static_cast<SaxParserImpl*>(impl_);

  for (size_t i = 0; i < len; ++i) {
    if (stopped_) break;

    yxml_ret_t r = yxml_parse(&impl->x, static_cast<int>(buf[i]));

    // Fast-path the two most frequent tokens without touching byteOffset or
    // entering the switch. In content-heavy XHTML, YXML_CONTENT fires on
    // almost every byte of text; in structural XML, YXML_OK dominates.
    if (r == YXML_OK) continue;
    if (r == YXML_CONTENT) {
      if (impl->inOpeningTag) {
        impl->byteOffset = static_cast<uint32_t>(impl->x.total);
        fireStart(impl);
      }
      if (impl->charCb) {
        // Inline single-byte append — x.data is a single char in >99% of cases.
        if (impl->charLen < kCharBufLen) {
          impl->charBuf[impl->charLen++] = impl->x.data[0];
          if (impl->x.data[1] == '\0') continue;
          appendChar(impl, impl->x.data + 1);
        } else {
          flushChar(impl);
          appendChar(impl, impl->x.data);
        }
      }
      continue;
    }

    // Update byteOffset only when a structural token triggers a callback.
    impl->byteOffset = static_cast<uint32_t>(impl->x.total);

    if (r < 0) {
      errorLine_   = static_cast<int>(impl->x.line);
      errorString_ = "yxml parse error";
      return false;
    }

    switch (r) {
      case YXML_ELEMSTART:
        if (impl->inOpeningTag) fireStart(impl);
        flushChar(impl);
        if (strlen(impl->x.elem) > kElemNameLen - 1) impl->truncFlags |= SaxParser::kTruncElemName;
        strncpy(impl->pendingElem, impl->x.elem, kElemNameLen - 1);
        impl->pendingElem[kElemNameLen - 1] = '\0';
        impl->attrCount    = 0;
        impl->inOpeningTag = true;
        break;

      case YXML_ATTRSTART:
        if (impl->attrCount < kMaxAttrs) {
          AttrPair& a = impl->attrs[impl->attrCount];
          if (strlen(impl->x.attr) > kElemNameLen - 1) impl->truncFlags |= SaxParser::kTruncAttrName;
          strncpy(a.name, impl->x.attr, kElemNameLen - 1);
          a.name[kElemNameLen - 1] = '\0';
          a.value[0] = '\0';
          a.valueLen = 0;
        } else {
          impl->truncFlags |= SaxParser::kTruncMaxAttrs;
        }
        break;

      case YXML_ATTRVAL:
        if (impl->attrCount < kMaxAttrs) {
          AttrPair& a = impl->attrs[impl->attrCount];
          // Append x.data (usually 1 byte, occasionally 2-3 for multi-byte
          // char refs) using the stored cursor — no strlen() needed.
          const char* p = impl->x.data;
          for (; *p && a.valueLen < kAttrValueLen - 1; ++p) {
            a.value[a.valueLen++] = *p;
          }
          if (*p) impl->truncFlags |= SaxParser::kTruncAttrValue;  // value didn't fit
          a.value[a.valueLen] = '\0';
        }
        break;

      case YXML_ATTREND:
        if (impl->attrCount < kMaxAttrs) ++impl->attrCount;
        break;

      case YXML_ELEMEND:
        if (impl->inOpeningTag) fireStart(impl);
        flushChar(impl);
        if (impl->endCb && impl->elemDepth > 0) {
          impl->endCb(impl->userData, impl->elemStack[impl->elemDepth - 1]);
        }
        if (impl->elemDepth > 0) --impl->elemDepth;
        break;

      case YXML_PISTART:
      case YXML_PICONTENT:
      case YXML_PIEND:
        break;

      default:
        break;
    }
  }

  return true;
}

bool SaxParser::finalize() {
  if (!impl_) return false;
  auto* impl = static_cast<SaxParserImpl*>(impl_);

  if (stopped_) return true;

  flushChar(impl);

  yxml_ret_t r = yxml_eof(&impl->x);
  if (r != YXML_OK) {
    errorLine_   = static_cast<int>(impl->x.line);
    errorString_ = "yxml unexpected EOF";
    return false;
  }
  return true;
}

void SaxParser::stop() {
  if (!impl_) return;
  stopped_ = true;
}

uint32_t SaxParser::byteOffset() const {
  if (!impl_) return 0;
  return static_cast<SaxParserImpl*>(impl_)->byteOffset;
}

uint32_t SaxParser::truncationFlags() const {
  if (!impl_) return 0;
  return static_cast<SaxParserImpl*>(impl_)->truncFlags;
}
