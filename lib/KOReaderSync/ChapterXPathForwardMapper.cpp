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

namespace {

// Forward mapper: translate intra-spine progress to a KOReader-compatible XPath.
// Strategy:
// 1) Count total visible text bytes in chapter.
// 2) Stream parse again and stop when target byte offset is reached.
// 3) Emit either /text()[N].M when the cursor is at a direct text child of
//    <body>, or the bare element path otherwise.
//
// Why body-level only (and not deep nested /p[i]/span[j]/text()[k].M):
//   KOReader's crengine normalises the DOM differently than expat — it merges
//   adjacent inline elements, drops empty wrappers, and renumbers text nodes
//   inside <p>/<span>/<em>. A deep XPath we emit (e.g. /p[17]/span[1]/text()[1].26)
//   often fails to match crengine's tree, and KOReader stores a degraded
//   fallback position (start-of-wrapper-div or off-by-N text node) that
//   round-trips back to the wrong page on pull. Body-level text-point XPaths
//   have a much higher round-trip success rate even though they sacrifice
//   character-precision inside paragraphs. The Section paragraph LUT then
//   snaps the pulled position to the correct page anyway, so the precision
//   loss is invisible to users.
//
// This matches the 1.42 behavior. The pre-1.43 forward mapper only emitted
// text-point XPaths when the cursor was a direct text child of <body>; the
// 1.43 change to deep emission is the regression we're undoing here.

struct ForwardState : StackState {
  int spineIndex;
  size_t targetOffset;
  std::string result;
  bool found = false;
  SaxParser* saxParser = nullptr;

  // Body-level text-node bookkeeping: only counts text nodes that are direct
  // children of <body>. Inline-element text contributes to totalTextBytes via
  // the StackState base, but does not advance bodyTextNodeCount because
  // KOReader can't round-trip a deep text-node XPath reliably.
  int bodyTextNodeCount = 0;
  size_t codepointsInBodyTextNode = 0;
  bool inBodyTextNode = false;

  ForwardState(const int spineIndex, const size_t targetOffset) : spineIndex(spineIndex), targetOffset(targetOffset) {}

  void onStartElement(const char* rawName) {
    inBodyTextNode = false;
    pushElement(rawName);
  }

  void onEndElement() {
    inBodyTextNode = false;
    popElement();
  }

  void onCharData(const char* text, const int len) {
    if (shouldSkipText(len) || found) {
      return;
    }

    const bool atBodyLevel = bodyIdx() + 1 == static_cast<int>(stack.size());
    if (atBodyLevel && !inBodyTextNode) {
      inBodyTextNode = true;
      bodyTextNodeCount++;
      codepointsInBodyTextNode = 0;
    }

    if (isWhitespaceOnly(text, len)) {
      if (atBodyLevel) {
        codepointsInBodyTextNode += countUtf8Codepoints(text, len);
      }
      return;
    }

    const size_t visible = countVisibleBytes(text, len);
    if (totalTextBytes + visible >= targetOffset) {
      if (atBodyLevel && bodyTextNodeCount > 0) {
        // KOReader/crengine text-point semantics use codepoint offsets.
        const size_t targetVisibleByteInChunk = targetOffset - totalTextBytes;
        const size_t cpInChunk = codepointAtVisibleByte(text, len, targetVisibleByteInChunk);
        const size_t charOff = codepointsInBodyTextNode + cpInChunk;
        result =
            currentXPath(spineIndex) + "/text()[" + std::to_string(bodyTextNodeCount) + "]." + std::to_string(charOff);
      } else {
        // Cursor is inside a nested element. Emit the element path without a
        // text-point suffix — KOReader will treat this as a position at the
        // start of the named element, which is good enough for paragraph-level
        // accuracy. Don't emit a deep text() index here: see header comment.
        result = currentXPath(spineIndex);
      }
      found = true;
      if (saxParser) {
        saxParser->stop();
      }
      return;
    }

    totalTextBytes += visible;
    if (atBodyLevel) {
      codepointsInBodyTextNode += countUtf8Codepoints(text, len);
    }
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

}  // namespace ChapterXPathIndexerInternal
