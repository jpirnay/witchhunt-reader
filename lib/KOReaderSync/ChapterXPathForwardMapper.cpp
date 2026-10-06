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
// headings, div, td, ...) the result is .../block[K]/text()[N].M: N the text node within the
// block, M the codepoint within the node. That is the shape KOReader emits itself, and the shape
// our reverse mapper resolves at its text-node-exact tier. Inside an inline element (em, span, a,
// ...) the element path is emitted: crengine merges and renumbers inline runs, and the deep
// text-point forms of 1.43 did not survive it (see git history of this file).
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
// Whitespace for R1-R3 is crengine's IsEmptySpace set: space, CR, LF, TAB. NBSP is a character.
//
// The fraction path (no page offset) keeps the form it had before: the element path, so the
// paragraph rather than a character in it, since its target is a byte fraction of the chapter and
// not a page's start. Only text directly in <body>, which has no element of its own to name, gets
// a text point there (1.42's rule).
namespace {

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
      if (textPointsInBlocks ? isBlockTag(parent.tag) : parent.tag == "body") {
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
                      parserDefaultCb<ForwardState>)) {
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

std::string findXPathForVisibleOffsetInternal(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                              const uint32_t visibleOffset) {
  ForwardState state(spineIndex, visibleOffset);
  state.inclusive = false;
  state.textPointsInBlocks = true;
  SaxParser saxParser;
  if (!saxParser.init(&state, parserStartCb<ForwardState>, parserEndCb<ForwardState>, parserCharCb<ForwardState>,
                      parserDefaultCb<ForwardState>)) {
    return "";
  }
  state.saxParser = &saxParser;
  streamSpine(epub, spineIndex, saxParser);
  LOG_DBG("KOX", "Forward: spine=%d offset=%u -> %s", spineIndex, visibleOffset,
          state.result.empty() ? "(not found)" : state.result.c_str());
  return state.result;
}

}  // namespace ChapterXPathIndexerInternal
