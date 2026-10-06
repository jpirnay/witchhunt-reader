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
// Where the cursor lands in text that is a direct child of a BLOCK element (p, li, headings,
// div, td, ...) the result is .../block[K]/text()[N].M: N the text node within the block, M the
// codepoint within the node. That is the shape KOReader emits itself, and the shape our reverse
// mapper resolves at its text-node-exact tier. Inside an inline element (em, span, a, ...) the
// element path is emitted: crengine merges and renumbers inline runs, and the deep text-point
// forms of 1.43 did not survive it (see git history of this file).
//
// The fraction path (no page offset) keeps the form it had before: the element path, so the
// paragraph rather than a character in it, since its target is a byte fraction of the chapter and
// not a page's start. Only text directly in <body>, which has no element of its own to name, gets
// a text point there (1.42's rule).
namespace {

bool isBlockTag(const std::string& tag) {
  static constexpr const char* kBlocks[] = {"p",          "li",  "h1",      "h2",      "h3",         "h4", "h5",
                                            "h6",         "div", "td",      "th",      "blockquote", "dd", "dt",
                                            "figcaption", "pre", "section", "article", "body"};
  for (const char* b : kBlocks) {
    if (tag == b) return true;
  }
  return false;
}

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
    if (!parent.inTextNode) {
      parent.inTextNode = true;
      parent.textNodeCount++;
      parent.codepointsInTextNode = 0;
    }
    if (isWhitespaceOnly(text, len)) {
      parent.codepointsInTextNode += countUtf8Codepoints(text, len);
      return;
    }
    const size_t visible = countVisibleBytes(text, len);
    const bool reached = inclusive ? totalTextBytes + visible >= targetOffset : totalTextBytes + visible > targetOffset;
    if (reached) {
      // The target is inside this chunk (totalTextBytes <= target < totalTextBytes + visible).
      if (textPointsInBlocks ? isBlockTag(parent.tag) : parent.tag == "body") {
        const size_t targetVisibleByteInChunk = targetOffset - totalTextBytes;
        const size_t cpInChunk = codepointAtVisibleByte(text, len, targetVisibleByteInChunk);
        result = currentXPath(spineIndex) + "/text()[" + std::to_string(parent.textNodeCount) + "]." +
                 std::to_string(parent.codepointsInTextNode + cpInChunk);
      } else {
        result = currentXPath(spineIndex);
      }
      found = true;
      if (saxParser) saxParser->stop();
      return;
    }
    totalTextBytes += visible;
    parent.codepointsInTextNode += countUtf8Codepoints(text, len);
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
