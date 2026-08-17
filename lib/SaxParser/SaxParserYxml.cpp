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
static constexpr size_t kStackSize = 2048;
static constexpr size_t kElemNameLen = 32;
static constexpr size_t kAttrValueLen = 384;
static constexpr size_t kMaxAttrs = 12;
static constexpr size_t kMaxDepth = 64;
static constexpr size_t kCharBufLen = 256;

// ---------------------------------------------------------------------------
// Internal state — entirely stack/struct allocated, zero heap after new.
// ---------------------------------------------------------------------------
struct AttrPair {
  char name[kElemNameLen];
  char value[kAttrValueLen];
  size_t valueLen = 0;  // cursor: avoids strlen() on each ATTRVAL character
};

struct SaxParserImpl {
  yxml_t x;
  unsigned char yxmlStack[kStackSize];

  SaxStartCb startCb = nullptr;
  SaxEndCb endCb = nullptr;
  SaxCharCb charCb = nullptr;
  SaxDefaultCb defaultCb = nullptr;
  void* userData = nullptr;

  // Character-data accumulation; flushed before every start/end event and at
  // the end of each text node (ELEMEND).
  char charBuf[kCharBufLen];
  size_t charLen = 0;

  // Opening-tag accumulation: collected between ELEMSTART and the first
  // non-ATTR token, then fired as a single startCb call.
  char pendingElem[kElemNameLen];  // element name waiting to be fired
  AttrPair attrs[kMaxAttrs];
  size_t attrCount = 0;
  bool inOpeningTag = false;

  // Element-name stack for end-tag callbacks (yxml's x.elem points to the
  // parent at ELEMEND time, not the element just closed).
  char elemStack[kMaxDepth][kElemNameLen];
  size_t elemDepth = 0;

  // Running byte offset (updated once per yxml_parse call).
  uint32_t byteOffset = 0;

  // Bitmask of SaxParser::TruncationFlag values — records which fixed-capacity
  // limits were hit (and silently truncated) over the lifetime of the parse.
  uint32_t truncFlags = 0;

  // Entity pre-processor: buffers '&' + name chars until ';' is seen, then
  // either passes the sequence through to yxml (XML built-ins / numeric refs)
  // or routes it to defaultCb (HTML named entities like &nbsp;).
  // Longest standard HTML entity name is ~8 chars; 24 bytes covers all cases.
  char entityBuf[24];
  int entityLen = 0;  // 0 = not accumulating

  // Void-element pre-processor: tracks raw start-tag bytes independently of
  // yxml's own token stream so an HTML-style unclosed void tag (<br>, <hr>, ...)
  // can be turned into a self-closing one (<br/>) before yxml ever sees it.
  enum class TagScan : uint8_t {
    kNone,         // not inside a tag
    kPendingKind,  // just saw '<', deciding what kind of tag this is
    kName,         // accumulating the start-tag name
    kBody,         // past the name, watching for the terminating '>'
  };
  TagScan tagScan = TagScan::kNone;
  char tagNameBuf[kElemNameLen];
  size_t tagNameLen = 0;
  bool tagIsVoid = false;
  char tagQuote = 0;          // 0, '\'' or '"' — attribute value quote currently open
  bool tagPrevSlash = false;  // last unquoted body byte was '/' (already self-closing)
  // Opt-in (see SaxParser::init): repair is for HTML-flavored documents only.
  // Strict-XML documents (EPUB3 OPF) pair <meta>...</meta> and must not be touched.
  bool voidRepairEnabled = false;

  // Explicit-end-tag suppression, armed whenever a self-close is synthesized.
  //
  // Self-closing <meta ...> into <meta ... /> is only half the job: an XHTML
  // document may also carry the matching </meta>, which would then close an
  // element that is no longer open and abort the parse. So after synthesizing,
  // watch for that end tag and drop it if it comes.
  //
  // Buffered rather than decided byte-by-byte because the candidate can be
  // split across feed() chunks, and because a non-match must be replayed to
  // yxml intact — same shape as the entity pre-processor below.
  char voidCloseName[kElemNameLen] = {0};  // element just self-closed; "" = not armed
  char voidCloseBuf[kElemNameLen + 4];     // candidate "</name>" bytes seen so far
  size_t voidCloseLen = 0;                 // 0 = armed but not yet buffering
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// HTML5 void elements — never carry content or a matching end tag. Compared
// case-insensitively since sloppy converters aren't consistent about case.
static bool isHtmlVoidElement(const char* name) {
  static const char* const kVoidElements[] = {"area",  "base", "br",   "col",   "embed",  "hr",    "img",
                                              "input", "link", "meta", "param", "source", "track", "wbr"};
  for (const char* v : kVoidElements) {
    const char* a = name;
    const char* b = v;
    while (*a && *b) {
      char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
      if (ca != *b) break;
      ++a;
      ++b;
    }
    if (*a == '\0' && *b == '\0') return true;
  }
  return false;
}

static void flushChar(SaxParserImpl* impl) {
  if (impl->charCb && impl->charLen > 0) {
    impl->charCb(impl->userData, impl->charBuf, static_cast<int>(impl->charLen));
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
      atts[i * 2] = impl->attrs[i].name;
      atts[i * 2 + 1] = impl->attrs[i].value;
    }
    atts[n * 2] = nullptr;
    impl->startCb(impl->userData, impl->pendingElem, atts);
  }

  impl->attrCount = 0;
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

bool SaxParser::init(void* userData, SaxStartCb startCb, SaxEndCb endCb, SaxCharCb charCb, SaxDefaultCb defaultCb,
                     bool htmlVoidTagRepair) {
  reset();
  stopped_ = false;
  errorLine_ = 0;
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
  impl->startCb = startCb;
  impl->endCb = endCb;
  impl->charCb = charCb;
  impl->defaultCb = defaultCb;
  impl->userData = userData;
  impl->voidRepairEnabled = htmlVoidTagRepair;

  yxml_init(&impl->x, impl->yxmlStack, kStackSize);
  impl_ = impl;
  return true;
}

bool SaxParser::feed(const uint8_t* buf, size_t len) {
  if (!impl_) return false;
  if (!buf && len > 0) return false;
  auto* impl = static_cast<SaxParserImpl*>(impl_);

  // Dispatch one yxml token. Used from both the normal byte path and the
  // entity-flush path. Returns false on hard parse error.
  auto dispatchToken = [this, impl](yxml_ret_t r) -> bool {
    if (r == YXML_OK) return true;
    if (r == YXML_CONTENT) {
      if (impl->inOpeningTag) {
        impl->byteOffset = static_cast<uint32_t>(impl->x.total);
        fireStart(impl);
      }
      if (impl->charCb) {
        if (impl->charLen < kCharBufLen) {
          impl->charBuf[impl->charLen++] = impl->x.data[0];
          if (impl->x.data[1] != '\0') appendChar(impl, impl->x.data + 1);
        } else {
          flushChar(impl);
          appendChar(impl, impl->x.data);
        }
      }
      return true;
    }
    impl->byteOffset = static_cast<uint32_t>(impl->x.total);
    if (r < 0) {
      errorLine_ = static_cast<int>(impl->x.line);
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
        impl->attrCount = 0;
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
          const char* p = impl->x.data;
          for (; *p && a.valueLen < kAttrValueLen - 1; ++p) {
            a.value[a.valueLen++] = *p;
          }
          if (*p) impl->truncFlags |= SaxParser::kTruncAttrValue;
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
    return true;
  };

  // Feed one byte to yxml and dispatch the result.
  auto feedByte = [&](unsigned char c) -> bool { return dispatchToken(yxml_parse(&impl->x, static_cast<int>(c))); };

  // ---------------------------------------------------------------------------
  // HTML void-element pre-processor
  //
  // Real-world EPUB/OPDS/XHTML content frequently contains HTML-style void
  // elements (<br>, <hr>, <img src="..">, ...) without the XML-required
  // self-closing slash. yxml is a strict well-formed-XML engine: an unclosed
  // <br> parses as an open element awaiting a matching </br>, and the
  // document eventually fails with a syntax/EOF error rather than laying out
  // as intended (this is the same class of failure expat had).
  //
  // This watches raw start-tag bytes independently of yxml's own token
  // stream: name, then body up to the terminating unquoted '>'. If the name
  // is a known HTML void element and the tag isn't already self-closed, a
  // synthetic '/' is fed to yxml immediately before the real '>', turning
  // "<br>" into "<br/>" from yxml's point of view. End tags, comments, CDATA,
  // PIs and DOCTYPE are left completely alone (identified by the byte right
  // after '<' and never tracked here), so this can't disturb them.
  //
  // A document that pairs a void element with a real end tag (<meta ...></meta>)
  // is handled too: synthesizing the self-close arms voidCloseName, and the
  // matching end tag is dropped in feed() before yxml sees it.
  //
  // That pairing was once written off here as "essentially never emitted by real
  // tools". It is not rare at all — XHTML 1.1 requires a void element to either
  // self-close or be paired, so converters targeting it emit the paired form, and
  // a FictionBook->XHTML book whose every content file began
  // <meta ...></meta><link ...></link> failed to open on every single spine.
  // ---------------------------------------------------------------------------
  auto tagScanByte = [&](uint8_t c) -> bool {
    using TagScan = SaxParserImpl::TagScan;
    if (!impl->voidRepairEnabled) return true;  // strict XML: never rewrite the byte stream
    if (impl->tagScan == TagScan::kNone) {
      if (c == '<') impl->tagScan = TagScan::kPendingKind;
      return true;
    }
    if (impl->tagScan == TagScan::kPendingKind) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        impl->tagNameLen = 1;
        impl->tagNameBuf[0] = static_cast<char>(c);
        impl->tagScan = TagScan::kName;
      } else {
        impl->tagScan = TagScan::kNone;  // end tag / PI / comment / doctype: not our concern
      }
      return true;
    }
    if (impl->tagScan == TagScan::kName) {
      const bool isNameChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                              c == '_' || c == ':';
      if (isNameChar) {
        if (impl->tagNameLen < kElemNameLen - 1) impl->tagNameBuf[impl->tagNameLen++] = static_cast<char>(c);
        return true;
      }
      impl->tagNameBuf[impl->tagNameLen] = '\0';
      impl->tagIsVoid = isHtmlVoidElement(impl->tagNameBuf);
      impl->tagQuote = 0;
      impl->tagPrevSlash = false;
      impl->tagScan = TagScan::kBody;
      // fall through: this byte (whitespace, '/' or '>') is itself body content
    }
    // TagScan::kBody
    if (impl->tagQuote) {
      if (c == impl->tagQuote) impl->tagQuote = 0;
      impl->tagPrevSlash = false;
      return true;
    }
    if (c == '"' || c == '\'') {
      impl->tagQuote = static_cast<char>(c);
      impl->tagPrevSlash = false;
      return true;
    }
    if (c == '>') {
      impl->tagScan = TagScan::kNone;
      if (impl->tagIsVoid && !impl->tagPrevSlash) {
        impl->truncFlags |= SaxParser::kVoidTagRepaired;
        // Arm end-tag suppression: having just made this element self-closing, a
        // matching </name> in the source would now close nothing. See the
        // voidCloseName block in feed().
        strncpy(impl->voidCloseName, impl->tagNameBuf, kElemNameLen - 1);
        impl->voidCloseName[kElemNameLen - 1] = '\0';
        impl->voidCloseLen = 0;
        return feedByte('/');  // synthesize self-close before the caller feeds the real '>'
      }
      return true;
    }
    impl->tagPrevSlash = (c == '/');
    return true;
  };

  for (size_t i = 0; i < len; ++i) {
    if (stopped_) break;

    const uint8_t c = buf[i];

    // ---------------------------------------------------------------------------
    // Explicit end tag after a synthesized self-close
    //
    // tagScanByte() rewrites <meta ...> into <meta ... />. If the document also
    // supplies </meta> -- which XHTML 1.1 output does routinely, since there a void
    // element must either self-close or be paired -- that end tag now closes an
    // element that is no longer open, and yxml aborts the whole document. Observed
    // on a FictionBook->XHTML conversion whose every content file opens with
    // <meta ...></meta><link ...></link>, making the book unopenable.
    //
    // So drop the end tag that belongs to a self-close we synthesized. Only that
    // one: the name must match, and anything else disarms. A partial candidate is
    // replayed byte-for-byte, so a non-match costs nothing.
    // ---------------------------------------------------------------------------
    if (impl->voidCloseName[0] != '\0') {
      if (impl->voidCloseLen == 0) {
        // Armed, nothing buffered yet. Whitespace between the two tags is
        // insignificant and passes through; anything else that is not the start of
        // a tag means the end tag is not coming.
        if (c == '<') {
          impl->voidCloseBuf[impl->voidCloseLen++] = '<';
          continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
          impl->voidCloseName[0] = '\0';
        }
      } else {
        const size_t pos = impl->voidCloseLen;
        const size_t nameLen = strlen(impl->voidCloseName);
        char expect;
        if (pos == 1) {
          expect = '/';
        } else if (pos - 2 < nameLen) {
          expect = impl->voidCloseName[pos - 2];
        } else {
          expect = '>';
        }
        const char cl = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
        const char el = (expect >= 'A' && expect <= 'Z') ? static_cast<char>(expect + 32) : expect;
        if (cl == el) {
          if (expect == '>') {
            // Complete match — drop the buffered "</name>" entirely.
            impl->voidCloseLen = 0;
            impl->voidCloseName[0] = '\0';
            continue;
          }
          if (impl->voidCloseLen < sizeof(impl->voidCloseBuf)) {
            impl->voidCloseBuf[impl->voidCloseLen++] = static_cast<char>(c);
          }
          continue;
        }
        // Not our end tag. Replay what was withheld through the normal path (the
        // scanner must see these bytes too — the candidate may itself have been the
        // start of a tag), then fall through and process c as usual.
        for (size_t j = 0; j < impl->voidCloseLen && !stopped_; ++j) {
          const uint8_t b = static_cast<uint8_t>(impl->voidCloseBuf[j]);
          if (!tagScanByte(b)) return false;
          if (!feedByte(b)) return false;
        }
        impl->voidCloseLen = 0;
        impl->voidCloseName[0] = '\0';
        if (stopped_) break;
      }
    }

    if (!tagScanByte(c)) return false;
    if (stopped_) break;

    // ---------------------------------------------------------------------------
    // HTML entity pre-processor
    //
    // XHTML files frequently contain HTML named entities (e.g. &nbsp;) that are
    // defined in the XHTML DTD but are not XML built-ins. yxml does not load
    // external DTDs, so it returns YXML_EREF for these and aborts the parse.
    //
    // We intercept &name; sequences before yxml sees them:
    //   - XML built-ins (&amp; &lt; &gt; &quot; &apos;) and numeric refs
    //     (&#N; &#xN;) pass straight through to yxml unchanged.
    //   - All other named entities are routed to defaultCb (same contract as
    //     expat's DefaultHandlerExpand), which lets ChapterHtmlSlimParser resolve
    //     them via lookupHtmlEntity(). yxml never sees these bytes.
    //   - If no defaultCb is registered the bytes pass to yxml → YXML_EREF →
    //     error, preserving the pre-existing behaviour for non-HTML parsers.
    // ---------------------------------------------------------------------------
    if (impl->entityLen > 0) {
      const bool isNameChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#' ||
                              c == '_' || c == ':';
      if (c == ';') {
        if (impl->entityLen < (int)sizeof(impl->entityBuf) - 1) {
          impl->entityBuf[impl->entityLen++] = ';';
          impl->entityBuf[impl->entityLen] = '\0';
        }
        // Numeric refs and the five XML built-ins pass through to yxml.
        const bool isXmlBuiltin = (impl->entityLen > 1 && impl->entityBuf[1] == '#') ||
                                  strcmp(impl->entityBuf, "&amp;") == 0 || strcmp(impl->entityBuf, "&lt;") == 0 ||
                                  strcmp(impl->entityBuf, "&gt;") == 0 || strcmp(impl->entityBuf, "&quot;") == 0 ||
                                  strcmp(impl->entityBuf, "&apos;") == 0;
        if (isXmlBuiltin) {
          for (int j = 0; j < impl->entityLen && !stopped_; ++j) {
            if (!feedByte((unsigned char)impl->entityBuf[j])) return false;
          }
        } else if (impl->defaultCb) {
          // Route to defaultCb — mirrors expat's DefaultHandlerExpand behaviour.
          // Fire any pending element start and flush char data first so that the
          // entity expansion arrives in the correct document order.
          if (impl->inOpeningTag) {
            impl->byteOffset = static_cast<uint32_t>(impl->x.total);
            fireStart(impl);
          }
          flushChar(impl);
          impl->defaultCb(impl->userData, impl->entityBuf, impl->entityLen);
        } else {
          // No resolver: pass to yxml, which will return YXML_EREF → error.
          for (int j = 0; j < impl->entityLen && !stopped_; ++j) {
            if (!feedByte((unsigned char)impl->entityBuf[j])) return false;
          }
        }
        impl->entityLen = 0;
        continue;
      } else if (isNameChar && impl->entityLen < (int)sizeof(impl->entityBuf) - 2) {
        impl->entityBuf[impl->entityLen++] = c;
        continue;
      } else {
        // Not a valid entity sequence (invalid char or name too long): flush
        // the buffered bytes to yxml and fall through to process c normally.
        for (int j = 0; j < impl->entityLen && !stopped_; ++j) {
          if (!feedByte((unsigned char)impl->entityBuf[j])) return false;
        }
        impl->entityLen = 0;
        // fall through
      }
    } else if (c == '&') {
      impl->entityBuf[0] = '&';
      impl->entityLen = 1;
      continue;
    }

    // Fast-path the two most frequent tokens without entering the switch.
    // In content-heavy XHTML, YXML_CONTENT fires on almost every byte of text;
    // in structural XML, YXML_OK dominates.
    yxml_ret_t r = yxml_parse(&impl->x, static_cast<int>(c));
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
    if (!dispatchToken(r)) return false;
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
    errorLine_ = static_cast<int>(impl->x.line);
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
