#include "ChapterXPathForwardMapper.h"

#include <Logging.h>
#include <SaxParser/SaxParser.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>

#include "ChapterXPathIndexerInternal.h"
#include "ChapterXPathIndexerState.h"

namespace ChapterXPathIndexerInternal {

// Forward mapper: the XPath of the text at a visible-byte offset.
//
// Where the cursor lands in text that is a direct child of a BLOCK element (isBlockTag: p, li,
// headings, div, td, ...; not table/thead/tbody/tfoot/tr, which keep no text in crengine, see
// allowsTextChildren) the result is .../block[K]/text()[N].M: N the text node within the
// block, M the codepoint within the node. That is the shape KOReader emits itself, and the shape
// our reverse mapper resolves at its text-node-exact tier. Inline elements (em, span, a, ...) are
// named through: .../p[K]/span[1]/text()[N].M (kTextPointsInsideInlineElements). The element path
// remains only where allowsTextChildren is false (stray text in table rows). The result is set only
// in onCharData, so the push never names an image: an image-led page names the first text after
// it, and a page starting at the chapter's total gets the end of the last text node (second pass
// in findXPathForVisibleOffsetInternal).
//
// N and M count crengine's DOM, not our SAX stream (koreader/crengine, crengine/src/lvtinydom.cpp
// and lvxml.cpp, read 2026-10-06). StackState::onTextChunk/closeTextRun keep one counter for both
// mappers:
//   R1 ldomElementWriter::onText: the first whitespace-only text run of a non-pre block element
//      (no child yet) is dropped at parse time; <p>\n  <em>x</em> rest</p> has one text node.
//   R2 PreProcessXmlString (non-pre): CR, LF and TAB become spaces and a run of spaces is stored
//      as ONE space (the first, even at node start; nothing is trimmed). M indexes this stored
//      text in codepoints: <p>Hello\n      world</p> stores "Hello world", w at M=6.
//   R3 ldomNode::removeStandaloneWhitespaceTextChildrenInMixedContent: in an element with block
//      children AND inline content, a whitespace-only text node is deleted unless it sits between
//      two inline-ish siblings (an inline element or a text node).
//   R4 createXPointerV1/V2: a text()[N] that does not exist is a null XPointer, and
//      LVDocView::getBookmarkPage(null) is page 0: one node too many sends KOReader to the book's
//      first page.
//   R5 <pre> (TXTFLG_PRE, inherited by descendants): R1 and R2 do not apply; text is stored raw.
//   R6 (accepted gap) a comment splits a text node in crengine; our SAX parser reports no comment.
//   R7 (accepted gap) crengine decides block-ness and pre from computed CSS (isBlockNode,
//      white_space >= pre-line); we decide from tag names (isBlockTag, <pre>). span{display:block},
//      div{display:inline} or white-space: pre-wrap over pretty-printed whitespace can shift N or M.
// Whitespace for R1-R3 is crengine's IsEmptySpace set: space, CR, LF, TAB. NBSP is a character.
//
// The fraction path (no page offset) keeps the form it had before: the element path, so the
// paragraph rather than a character in it, since its target is a byte fraction of the chapter and
// not a page's start. Only text directly in <body>, which has no element of its own to name, gets
// a text point there (1.42's rule).
namespace {

// Text points for text directly inside an inline element (span, em, a, ...), named through the
// inline elements: /p[K]/span[1]/text()[N].M. On: release 1.43 emitted these and KOReader landed
// on wrong pages, which was the text-node numbering and codepoint counting that 5b corrected to
// follow crengine's DOM. A live KOReader then resolved such a point (2026-10-06, probe
// /body/DocFragment[10]/body/p[18]/i[1]/text()[1].39 landed on the page holding its target).
// The constant stays as the one-line way back to element paths inside inline elements.
constexpr bool kTextPointsInsideInlineElements = true;

struct ForwardState : StackState {
  int spineIndex;
  size_t targetOffset;
  // The fraction path's target can equal the chapter's total (intra 1.0) and must name the chunk
  // that ENDS there; a page's start is a byte of text and must name the chunk that CONTAINS it.
  bool inclusive = true;
  // Which text is named to the character: in any block element (a page's start), or only directly
  // in <body> (the fraction path; see the header comment).
  bool textPointsInBlocks = false;
  std::string result;
  bool found = false;
  SaxParser* saxParser = nullptr;

  ForwardState(const int spineIndex, const size_t targetOffset) : spineIndex(spineIndex), targetOffset(targetOffset) {}

  void onStartElement(const char* rawName) { pushElement(rawName); }
  void onEndElement() { popElement(); }

  void onCharData(const char* text, const int len) {
    if (shouldSkipText(len) || found || stack.empty()) {
      return;
    }
    StackNode& parent = stack.back();
    const TextRun run = onTextChunk(text, len);  // every chunk, before any early return
    const size_t visible = countVisibleBytes(text, len);
    if (!run.counts || visible == 0) {
      return;
    }
    // Fraction path: the target can equal the chapter total and names the chunk that ENDS there.
    // Offset path: a page's start is a byte of text and names the chunk that CONTAINS it.
    const bool reached = inclusive ? totalTextBytes + visible >= targetOffset : totalTextBytes + visible > targetOffset;
    if (reached) {
      const bool blockParent = textPointsInBlocks ? isBlockTag(parent.tag) : parent.tag == "body";
      const bool inlineParent = textPointsInBlocks && kTextPointsInsideInlineElements && !isBlockTag(parent.tag);
      if ((blockParent || inlineParent) && allowsTextChildren(parent.tag)) {
        const size_t targetVisibleByteInChunk = targetOffset - totalTextBytes;
        const size_t cpInChunk =
            collapsedCodepointAtVisibleByte(text, len, targetVisibleByteInChunk, preDepth == 0, run.spaceBefore);
        result = currentXPath(spineIndex) + "/text()[" + std::to_string(run.nodeIndex) + "]." +
                 std::to_string(run.codepointsBefore + cpInChunk);
      } else {
        result = currentXPath(spineIndex);
      }
      found = true;
      if (saxParser) saxParser->stop();
      return;
    }
    totalTextBytes += visible;
  }
};

std::string makeSpineCacheKey(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }
  const auto spineItem = epub->getSpineItem(spineIndex);
  return epub->getCachePath() + "|" + std::to_string(spineIndex) + "|" + spineItem.href;
}

std::optional<size_t> getTotalTextBytesCached(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  static std::unordered_map<std::string, size_t> sTotalBytesBySpine;
  static std::string sCachedBookPath;

  const std::string currentBookPath = epub ? epub->getCachePath() : std::string();
  if (currentBookPath != sCachedBookPath) {
    sTotalBytesBySpine.clear();
    sCachedBookPath = currentBookPath;
  }

  const std::string key = makeSpineCacheKey(epub, spineIndex);
  if (!key.empty()) {
    const auto it = sTotalBytesBySpine.find(key);
    if (it != sTotalBytesBySpine.end()) {
      return it->second;
    }
  }

  // A failed read is not cached: it may be transient (heap, SD), and the next call should retry.
  const auto totalTextBytes = countTotalTextBytes(epub, spineIndex);
  if (totalTextBytes && !key.empty()) {
    sTotalBytesBySpine[key] = *totalTextBytes;
  }
  return totalTextBytes;
}

}  // namespace

std::string findXPathForProgressInternal(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                         const float intraSpineProgress) {
  const auto counted = getTotalTextBytesCached(epub, spineIndex);
  if (!counted) {
    return "";
  }
  const size_t totalTextBytes = *counted;
  if (totalTextBytes == 0) {
    const std::string base = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
    LOG_DBG("KOX", "Forward: spine=%d no text, returning base xpath", spineIndex);
    return base;
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetOffset = static_cast<size_t>(clamped * static_cast<float>(totalTextBytes));

  ForwardState state(spineIndex, targetOffset);
  state.inclusive = true;
  SaxParser saxParser;
  if (!saxParser.init(&state, parserStartCb<ForwardState>, parserEndCb<ForwardState>, parserCharCb<ForwardState>,
                      parserDefaultCb<ForwardState>, /*htmlVoidTagRepair=*/true, SaxParser::Profile::Lean)) {
    return "";
  }

  state.saxParser = &saxParser;
  streamSpine(epub, spineIndex, saxParser);

  if (state.result.empty()) {
    state.result = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  LOG_DBG("KOX", "Forward: spine=%d progress=%.3f target=%zu/%zu -> %s", spineIndex, intraSpineProgress, targetOffset,
          totalTextBytes, state.result.c_str());
  return state.result;
}

namespace {

// One streamed pass of the offset path. `inclusive` names the chunk that ENDS at the target
// instead of the one that contains it. `atChapterEnd` reports a pass that found nothing because
// the target is the chapter's own total: the parse completed and counted exactly that many bytes.
std::string visibleOffsetPass(const std::shared_ptr<Epub>& epub, const int spineIndex, const uint32_t visibleOffset,
                              const bool inclusive, bool& atChapterEnd) {
  atChapterEnd = false;
  ForwardState state(spineIndex, visibleOffset);
  state.inclusive = inclusive;
  state.textPointsInBlocks = true;
  SaxParser saxParser;
  if (!saxParser.init(&state, parserStartCb<ForwardState>, parserEndCb<ForwardState>, parserCharCb<ForwardState>,
                      parserDefaultCb<ForwardState>, /*htmlVoidTagRepair=*/true, SaxParser::Profile::Lean)) {
    return "";
  }
  state.saxParser = &saxParser;
  const bool completed = streamSpine(epub, spineIndex, saxParser);
  atChapterEnd = completed && state.result.empty() && state.totalTextBytes == visibleOffset;
  return state.result;
}

}  // namespace

std::string findXPathForVisibleOffsetInternal(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                              const uint32_t visibleOffset) {
  bool atChapterEnd = false;
  std::string result = visibleOffsetPass(epub, spineIndex, visibleOffset, /*inclusive=*/false, atChapterEnd);
  // A page that starts at the chapter's total (a last page holding only an image or spacing) has no
  // text at or after its start, and the fraction fallback would land early. Name the end of the
  // last text node instead: the inclusive pass picks the chunk that ends there, and one past its
  // last visible byte is its collapsed count. The passes run one after the other, never together.
  if (result.empty() && atChapterEnd && visibleOffset > 0) {
    bool unused = false;
    result = visibleOffsetPass(epub, spineIndex, visibleOffset, /*inclusive=*/true, unused);
  }
  LOG_DBG("KOX", "Forward: spine=%d offset=%u -> %s", spineIndex, visibleOffset,
          result.empty() ? "(not found)" : result.c_str());
  return result;
}

}  // namespace ChapterXPathIndexerInternal
