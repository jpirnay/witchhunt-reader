#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <vector>

#include "hyphenation/HyphenationCommon.h"
#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Closing punctuation that should not have extra space inserted before it during justification.
// Includes common closing brackets/quotes and sentence-ending marks. En/em dashes
// are also treated as inline separators here to avoid justification stretch
// immediately before them.
bool isClosingPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case '!':
    case '?':
    case ':':
    case ';':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x203A:  // ›
    case 0x2019:  // '  right single quotation mark
    case 0x201D:  // "  right double quotation mark
    case 0x2026:  // … ellipsis
    case 0x2013:  // – en dash
    case 0x2014:  // — em dash
      return true;
    default:
      return false;
  }
}

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false, const float scale = 1.0f) {
  int raw = 0;
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    raw = renderer.getSpaceWidth(fontId, style);
  } else {
    const bool hasSoftHyphen = containsSoftHyphen(word);
    if (!hasSoftHyphen && !appendHyphen) {
      raw = renderer.getTextAdvanceX(fontId, word.c_str(), style);
    } else {
      std::string sanitized = word;
      if (hasSoftHyphen) stripSoftHyphensInPlace(sanitized);
      if (appendHyphen) sanitized.push_back('-');
      raw = renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
    }
  }
  if (scale == 1.0f) return static_cast<uint16_t>(raw);
  return static_cast<uint16_t>(raw * scale + 0.5f);
}

std::string buildLinePreview(const std::vector<std::string>& words, const std::vector<bool>& continuesVec,
                             const size_t start, const size_t endExclusive, const size_t maxLen = 120) {
  // Build a readable line preview while preserving continuation semantics
  // (no synthetic spaces before attached tokens).
  std::string preview;
  for (size_t idx = start; idx < endExclusive; ++idx) {
    if (idx > start && idx < continuesVec.size() && !continuesVec[idx]) {
      preview.push_back(' ');
    }
    preview += words[idx];
    if (preview.size() >= maxLen) {
      preview.resize(maxLen);
      preview += "...";
      break;
    }
  }
  return preview;
}

constexpr int kBionicReadingMinCodepoints = 4;
constexpr int kBionicReadingMinBoldPrefix = 1;
constexpr int kBionicReadingBoldPrefixNumerator = 1;
constexpr int kBionicReadingBoldPrefixDenominator = 2;

struct TokenSpan {
  size_t start;
  size_t end;
  bool isWord;
};

static int computeBionicBoldPrefixCount(const int codepointCount) {
  return std::max(kBionicReadingMinBoldPrefix,
                  (codepointCount * kBionicReadingBoldPrefixNumerator + kBionicReadingBoldPrefixDenominator - 1) /
                      kBionicReadingBoldPrefixDenominator);
}

static bool isBionicWordCodepoint(const uint32_t cp) {
  if (cp == 0) {
    return false;
  }
  if (utf8IsCombiningMark(cp)) {
    return true;
  }
  return isAlphabetic(cp) || isAsciiDigit(cp) || isApostrophe(cp);
}

// Split a word token into contiguous spans of "word-like" characters and non-word characters.
// This avoids applying bionic bolding to punctuation, digits-only runs, or other separators.
// Only spans marked as word-like are eligible for the bionic prefix transform.
static std::vector<TokenSpan> tokenizeBionicWord(const std::string& word) {
  std::vector<TokenSpan> spans;
  spans.reserve(2);

  const unsigned char* base = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* ptr = base;
  const unsigned char* segmentStart = ptr;
  bool currentIsWord = false;
  bool haveCurrent = false;

  while (true) {
    const unsigned char* cpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) {
      break;
    }

    bool cpIsWord = isBionicWordCodepoint(cp);
    if (!haveCurrent) {
      currentIsWord = cpIsWord;
      haveCurrent = true;
    } else if (!utf8IsCombiningMark(cp) && cpIsWord != currentIsWord) {
      spans.push_back({static_cast<size_t>(segmentStart - base), static_cast<size_t>(cpStart - base), currentIsWord});
      segmentStart = cpStart;
      currentIsWord = cpIsWord;
    }
  }

  if (haveCurrent) {
    spans.push_back({static_cast<size_t>(segmentStart - base), word.size(), currentIsWord});
  }

  return spans;
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  word = utf8NfcNorm(std::move(word));
  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

// Consumes data to minimize memory usage
int ParsedText::widthForLine(const int lineIndex, const int lineHeight, const int16_t blockStartY,
                             const int pageWidth) const {
  if (lineHeight == 0 || blockStyle.floatZoneCount == 0) return pageWidth;
  const int lineTop = blockStartY + lineIndex * lineHeight;
  int indent = 0;
  for (int i = 0; i < blockStyle.floatZoneCount; ++i) {
    const auto& z = blockStyle.floatZones[i];
    if (lineTop < z.bottom && lineTop + lineHeight > z.top) {
      indent += z.width;
    }
  }
  return std::max(1, pageWidth - indent);
}

void ParsedText::layoutAndExtractLines(
    const GfxRenderer& renderer, const int bodyFontId, const uint16_t viewportWidth,
    const std::function<LineProcessResult(std::shared_ptr<TextBlock>, bool, bool)>& processLine,
    const bool includeLastLine, const int16_t blockStartY, const int lineHeight) {
  if (words.empty()) {
    return;
  }
  // Heading blocks lay out (and below render) with a taller real font instead of scaling the
  // body font. Resolve the effective fontId once; all measurement helpers below take it as
  // their fontId argument, so widths/kerning/indent are computed in the heading font and stay
  // consistent with TextBlock::render(). For the scale-fallback path headingFontId==0 and this
  // is just the body font (fontSizeMultiplier still applies inside the helpers).
  const int fontId = blockStyle.headingFontId != 0 ? blockStyle.headingFontId : bodyFontId;

  // Apply fixed transforms before any per-line layout work.
  // Paragraph indent only applies to the first layout pass; skip on continuations.
  if (!isContinuation_) {
    applyParagraphIndent(renderer, fontId);
  }
  // Bionic transform is incremental: applyBionicReadingTransform() is a no-op
  // for already-transformed words (bionicTransformedUpTo_ == words.size()) and
  // only processes raw words appended since the last flush, so it is always safe
  // to call regardless of isContinuation_.
  if (bionicReadingEnabled) {
    applyBionicReadingTransform();
  }

  // Ensure font glyph metrics are loaded before measuring word widths.
  // For built-in fonts this is a no-op (the map lookup finds nothing and returns
  // immediately). For SD card fonts it reads glyph metadata (advanceX, no bitmaps)
  // for all codepoints in this paragraph so calculateWordWidths() needs no SD I/O.
  {
    size_t totalSize = 1;  // reserve room for a possible hyphen fallback
    for (size_t i = 0; i < words.size(); i++) {
      if (i > 0 && !wordContinues[i]) totalSize += 1;
      totalSize += words[i].size();
    }
    std::string allText;
    allText.reserve(totalSize);
    for (size_t i = 0; i < words.size(); i++) {
      if (i > 0 && !wordContinues[i]) allText += ' ';
      allText += words[i];
    }
    allText += '-';
    renderer.ensureFontReady(fontId, allText.c_str());
  }

  const int pageWidth = viewportWidth;

  // Compute firstLineIndent once here so all layout helpers use the same value.
  // On a continuation flush the remaining words are mid-paragraph, so no indent.
  // firstLineExtraIndent adds extra indent on the first line (on top of CSS text-indent).
  const int cssTextIndent =
      !isContinuation_ && blockStyle.textIndentDefined &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? std::min(std::max<int>(static_cast<int>(blockStyle.textIndent), -(pageWidth - 1)), pageWidth - 1)
          : 0;
  const int firstLineIndent = cssTextIndent + (isContinuation_ ? 0 : static_cast<int>(blockStyle.firstLineExtraIndent));

  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  std::vector<bool> lineEndsWithHyphenatedWord;
  std::vector<int> splitPrefixWordIndexes;
  std::vector<bool> splitInsertedHyphen;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues,
                                                   lineEndsWithHyphenatedWord, splitPrefixWordIndexes,
                                                   splitInsertedHyphen, firstLineIndent, blockStartY, lineHeight);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, firstLineIndent,
                                         blockStartY, lineHeight);
    lineEndsWithHyphenatedWord.assign(lineBreakIndices.size(), false);
    splitPrefixWordIndexes.assign(lineBreakIndices.size(), -1);
    splitInsertedHyphen.assign(lineBreakIndices.size(), false);
  }
  size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    const bool lineEndedWithHyphenation = i < lineEndsWithHyphenatedWord.size() ? lineEndsWithHyphenatedWord[i] : false;
    const auto result = extractLine(i, pageWidth, wordWidths, wordContinues, lineBreakIndices, processLine, renderer,
                                    fontId, lineEndedWithHyphenation, false, firstLineIndent, blockStartY, lineHeight);

    if (result == LineProcessResult::RetryWithoutHyphenation && lineEndedWithHyphenation) {
      const size_t lineStart = i > 0 ? lineBreakIndices[i - 1] : 0;
      const size_t lineEnd = i < lineBreakIndices.size() ? lineBreakIndices[i] : lineStart;
      const std::string firstAttemptPreview = buildLinePreview(words, wordContinues, lineStart, lineEnd);
      LOG_DBG("PTX", "Line %u requested rerender without hyphenation, first attempt: %s", static_cast<unsigned>(i),
              firstAttemptPreview.c_str());
      // Undo precomputed splits from this line onward so the retry starts from
      // clean, unsplit tokens and cannot inherit future hyphenation artifacts.
      std::set<int, std::greater<int>> splitIndexesToUndo;
      for (size_t lineIdx = i; lineIdx < splitPrefixWordIndexes.size(); ++lineIdx) {
        const int splitIndex = splitPrefixWordIndexes[lineIdx];
        if (splitIndex >= 0) {
          splitIndexesToUndo.insert(splitIndex);
        }
      }
      for (const int splitIndex : splitIndexesToUndo) {
        if (splitIndex < 0 || static_cast<size_t>(splitIndex + 1) >= words.size()) {
          continue;
        }
        bool removeInsertedHyphen = false;
        for (size_t lineIdx = i; lineIdx < splitPrefixWordIndexes.size(); ++lineIdx) {
          if (splitPrefixWordIndexes[lineIdx] == splitIndex && lineIdx < splitInsertedHyphen.size()) {
            removeInsertedHyphen = splitInsertedHyphen[lineIdx];
            break;
          }
        }

        std::string merged = words[splitIndex];
        if (removeInsertedHyphen && !merged.empty() && merged.back() == '-') {
          merged.pop_back();
        }
        merged += words[splitIndex + 1];
        words[splitIndex] = std::move(merged);
        words.erase(words.begin() + splitIndex + 1);
        wordStyles.erase(wordStyles.begin() + splitIndex + 1);
        wordContinues.erase(wordContinues.begin() + splitIndex + 1);
      }

      // Recompute widths after restoring unsplit words.
      wordWidths = calculateWordWidths(renderer, fontId);

      // Keep previous lines fixed; recompute only this specific line without hyphenation.
      // Suppression is intentionally line-local.
      const size_t retryBreak = computeSingleLineBreakNoHyphen(renderer, fontId, pageWidth, wordWidths, wordContinues,
                                                               lineStart, firstLineIndent, blockStartY, lineHeight);

      lineBreakIndices.resize(i + 1);
      lineEndsWithHyphenatedWord.resize(i + 1);
      splitPrefixWordIndexes.resize(i + 1);
      splitInsertedHyphen.resize(i + 1);

      lineBreakIndices[i] = retryBreak;
      lineEndsWithHyphenatedWord[i] = false;
      splitPrefixWordIndexes[i] = -1;
      splitInsertedHyphen[i] = false;
      lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

      if (i < lineBreakIndices.size()) {
        const size_t retryLineStart = i > 0 ? lineBreakIndices[i - 1] : 0;
        const size_t retryLineEnd = i < lineBreakIndices.size() ? lineBreakIndices[i] : retryLineStart;
        const std::string retryPreview = buildLinePreview(words, wordContinues, retryLineStart, retryLineEnd);
        LOG_DBG("PTX", "Rerendering line %u with hyphenation suppressed, retry attempt: %s", static_cast<unsigned>(i),
                retryPreview.c_str());
        extractLine(i, pageWidth, wordWidths, wordContinues, lineBreakIndices, processLine, renderer, fontId, false,
                    true, firstLineIndent, blockStartY, lineHeight);

        // Resume regular hyphenation from the first word after the retried line.
        const size_t resumeIndex = lineBreakIndices[i];
        std::vector<bool> suffixLineEndsWithHyphenatedWord;
        std::vector<int> suffixSplitPrefixWordIndexes;
        std::vector<bool> suffixSplitInsertedHyphen;
        const auto hyphenatedSuffixBreaks = computeHyphenatedLineBreaksFromIndex(
            renderer, fontId, pageWidth, wordWidths, wordContinues, resumeIndex, suffixLineEndsWithHyphenatedWord,
            suffixSplitPrefixWordIndexes, suffixSplitInsertedHyphen, blockStartY, lineHeight, static_cast<int>(i) + 1);

        lineBreakIndices.insert(lineBreakIndices.end(), hyphenatedSuffixBreaks.begin(), hyphenatedSuffixBreaks.end());
        lineEndsWithHyphenatedWord.insert(lineEndsWithHyphenatedWord.end(), suffixLineEndsWithHyphenatedWord.begin(),
                                          suffixLineEndsWithHyphenatedWord.end());
        splitPrefixWordIndexes.insert(splitPrefixWordIndexes.end(), suffixSplitPrefixWordIndexes.begin(),
                                      suffixSplitPrefixWordIndexes.end());
        splitInsertedHyphen.insert(splitInsertedHyphen.end(), suffixSplitInsertedHyphen.begin(),
                                   suffixSplitInsertedHyphen.end());

        lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;
        LOG_DBG("PTX", "Resumed regular hyphenation after rerendered line %u", static_cast<unsigned>(i));
      }
    }
  }

  // Remove consumed words so size() reflects only remaining words, then
  // release excess capacity.  Without shrink_to_fit the vector retains a
  // large allocation from before the flush; the next paragraph fills it
  // back up and eventually needs an even larger contiguous realloc.
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    words.shrink_to_fit();
    wordStyles.shrink_to_fit();
    wordContinues.shrink_to_fit();
    // All remaining words were already transformed before the flush; reset the
    // watermark so that words appended by addWord() are processed next time.
    bionicTransformedUpTo_ = words.size();
  }
  isContinuation_ = !includeLastLine;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());
  const float scale = blockStyle.fontSizeMultiplier;

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i], false, scale));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  const int firstLineIndent, const int16_t blockStartY,
                                                  const int lineHeight) {
  if (words.empty()) {
    return {};
  }

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = widthForLine(0, lineHeight, blockStartY, pageWidth) - (i == 0 ? firstLineIndent : 0);
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // Pre-compute inter-word gaps once so the O(n²) DP inner loop avoids repeated
  // codepoint scanning and renderer calls for every (i,j) pair.
  // interWordGaps[j] = the spacing between words[j-1] and words[j] (0 for j==0).
  std::vector<int> interWordGaps(totalWordCount, 0);
  for (size_t j = 1; j < totalWordCount; ++j) {
    if (!continuesVec[j]) {
      interWordGaps[j] =
          renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    } else {
      interWordGaps[j] =
          renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    }
  }

  // Greedy pre-pass: map each word-start index to its line number so the DP can
  // call widthForLine(lineIdx, ...) per-word-start in O(1).
  std::vector<int> lineIndexForWord(totalWordCount, 0);
  if (lineHeight > 0 && blockStyle.floatZoneCount > 0) {
    int lineIdx = 0;
    size_t cur = 0;
    while (cur < totalWordCount) {
      const int lineW =
          widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (lineIdx == 0 ? firstLineIndent : 0);
      int w = 0;
      size_t start = cur;
      while (cur < totalWordCount) {
        const int gap = (cur > start) ? interWordGaps[cur] : 0;
        if (w + gap + wordWidths[cur] > lineW && cur > start) break;
        lineIndexForWord[cur] = lineIdx;
        w += gap + wordWidths[cur];
        ++cur;
      }
      // Guard: if no progress (single oversized word), advance anyway
      if (cur == start) {
        lineIndexForWord[cur] = lineIdx;
        ++cur;
      }
      ++lineIdx;
    }
  }

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    const int lineIdx = lineIndexForWord[i];
    const int effectivePageWidth =
        widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (lineIdx == 0 ? firstLineIndent : 0);

    for (size_t j = i; j < totalWordCount; ++j) {
      const int gap = (j > static_cast<size_t>(i)) ? interWordGaps[j] : 0;
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line — no penalty regardless of looseness
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Knuth-Plass style demerits:
        //   badness  = (gap/lineWidth)³ × 10000, clamped to [0, 10000]
        //   demerits = (1 + badness)²
        // Cubic badness strongly penalises very loose lines while being
        // lenient on moderately loose ones, producing visually balanced paragraphs.
        const long long b_num = static_cast<long long>(remainingSpace) * remainingSpace * remainingSpace;
        const long long b_den = static_cast<long long>(effectivePageWidth) * effectivePageWidth * effectivePageWidth;
        const int badness = (b_den > 0) ? static_cast<int>(std::min(b_num * 10000LL / b_den, 10000LL)) : 10000;
        const long long demerits = static_cast<long long>(1 + badness) * (1 + badness);
        const long long cost_ll = demerits + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

size_t ParsedText::computeSingleLineBreakNoHyphen(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const std::vector<uint16_t>& wordWidths,
                                                  const std::vector<bool>& continuesVec, const size_t lineStartIndex,
                                                  const int firstLineIndent, const int16_t blockStartY,
                                                  const int lineHeight) const {
  // One-line non-hyphenating breaker used by the page-boundary retry path.
  if (lineStartIndex >= wordWidths.size()) {
    return lineStartIndex;
  }

  // lineStartIndex == 0 means this is the first line of the block.
  const int effectivePageWidth = widthForLine(lineStartIndex == 0 ? 0 : 1, lineHeight, blockStartY, pageWidth) -
                                 (lineStartIndex == 0 ? firstLineIndent : 0);

  size_t currentIndex = lineStartIndex;
  int lineWidth = 0;

  while (currentIndex < wordWidths.size()) {
    const bool isFirstWord = currentIndex == lineStartIndex;
    int spacing = 0;
    if (!isFirstWord) {
      if (!continuesVec[currentIndex]) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(words[currentIndex - 1]),
                                           firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      } else {
        spacing = renderer.getKerning(fontId, lastCodepoint(words[currentIndex - 1]),
                                      firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      }
    }

    const int candidateWidth = spacing + wordWidths[currentIndex];
    if (lineWidth + candidateWidth <= effectivePageWidth) {
      lineWidth += candidateWidth;
      ++currentIndex;
      continue;
    }

    if (currentIndex == lineStartIndex) {
      ++currentIndex;
    }
    break;
  }

  while (currentIndex > lineStartIndex + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
    --currentIndex;
  }

  return currentIndex;
}

void ParsedText::applyParagraphIndent(const GfxRenderer& renderer, const int fontId) {
  if (words.empty()) {
    return;
  }

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) — handled by extractLine() via firstLineIndent.
  } else if (!extraParagraphSpacing && blockStyle.floatZoneCount == 0 &&
             (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)) {
    // No CSS text-indent defined — apply a font-size-relative pixel indent so paragraph
    // boundaries are visually clear. Using getFontAscenderSize() gives one em in pixels,
    // matching the typographic convention for a paragraph indent. This avoids injecting
    // U+2003 (em-space) as a character, which caused glyph-miss overhead on every page
    // when the active font doesn't contain that codepoint.
    // Skip when a float zone is active: the image already provides visual separation.
    const int oneEm = static_cast<int>(renderer.getFontAscenderSize(fontId) * blockStyle.fontSizeMultiplier + 0.5f);
    blockStyle.textIndent = oneEm;
    blockStyle.textIndentDefined = true;
  }
}

void ParsedText::applyBionicReadingTransform() {
  // Only transform words that haven't been processed yet.  On a fresh block
  // bionicTransformedUpTo_ == 0 so all words are processed.  After an
  // intermediate flush, only the new raw words appended since the last flush
  // (indices bionicTransformedUpTo_..words.size()-1) need transformation.
  if (words.empty() || bionicTransformedUpTo_ >= words.size()) {
    return;
  }

  const size_t suffixStart = bionicTransformedUpTo_;
  std::vector<std::string> transformedSuffix;
  std::vector<EpdFontFamily::Style> transformedSuffixStyles;
  std::vector<bool> transformedSuffixContinues;
  transformedSuffix.reserve((words.size() - suffixStart) * 2);
  transformedSuffixStyles.reserve(transformedSuffix.capacity());
  transformedSuffixContinues.reserve(transformedSuffix.capacity());

  for (size_t i = suffixStart; i < words.size(); ++i) {
    std::string source = std::move(words[i]);
    const auto originalStyle = wordStyles[i];
    const bool originalAttachToPrevious = wordContinues[i];

    const auto spans = tokenizeBionicWord(source);
    if (spans.empty()) {
      continue;
    }

    bool attachToPrevious = originalAttachToPrevious;
    for (size_t spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
      const TokenSpan span = spans[spanIndex];
      const size_t spanLength = span.end - span.start;
      std::string token = source.substr(span.start, spanLength);

      if (span.isWord) {
        const unsigned char* ptr = reinterpret_cast<const unsigned char*>(token.c_str());
        int codepointCount = 0;
        while (utf8NextCodepoint(&ptr)) {
          codepointCount++;
        }

        if (codepointCount >= kBionicReadingMinCodepoints) {
          const int boldPrefixCount = computeBionicBoldPrefixCount(codepointCount);
          ptr = reinterpret_cast<const unsigned char*>(token.c_str());
          const unsigned char* prefixEnd = ptr;
          for (int j = 0; j < boldPrefixCount && *prefixEnd; ++j) {
            utf8NextCodepoint(&prefixEnd);
          }
          const size_t prefixByteCount =
              static_cast<size_t>(prefixEnd - reinterpret_cast<const unsigned char*>(token.c_str()));
          if (prefixByteCount < token.size()) {
            std::string suffix(reinterpret_cast<const char*>(prefixEnd), token.size() - prefixByteCount);
            token.resize(prefixByteCount);
            const auto boldStyle = static_cast<EpdFontFamily::Style>(originalStyle | EpdFontFamily::BOLD);
            transformedSuffix.push_back(std::move(token));
            transformedSuffixStyles.push_back(boldStyle);
            transformedSuffixContinues.push_back(attachToPrevious);

            transformedSuffix.push_back(std::move(suffix));
            transformedSuffixStyles.push_back(originalStyle);
            transformedSuffixContinues.push_back(true);
            attachToPrevious = true;
            continue;
          }
        }
      }

      transformedSuffix.push_back(std::move(token));
      transformedSuffixStyles.push_back(originalStyle);
      transformedSuffixContinues.push_back(attachToPrevious);
      attachToPrevious = true;
    }
  }

  // Replace the (now move-emptied) suffix with the transformed version.
  words.resize(suffixStart);
  wordStyles.resize(suffixStart);
  wordContinues.resize(suffixStart);
  words.insert(words.end(), std::make_move_iterator(transformedSuffix.begin()),
               std::make_move_iterator(transformedSuffix.end()));
  wordStyles.insert(wordStyles.end(), transformedSuffixStyles.begin(), transformedSuffixStyles.end());
  wordContinues.insert(wordContinues.end(), transformedSuffixContinues.begin(), transformedSuffixContinues.end());
  bionicTransformedUpTo_ = words.size();
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    std::vector<bool>& continuesVec, std::vector<bool>& lineEndsWithHyphenatedWord,
    std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen, const int firstLineIndent,
    const int16_t blockStartY, const int lineHeight) {
  // Pre-compute inter-word gaps to avoid repeated codepoint scanning and renderer
  // calls in the inner loop. When hyphenateWordAtIndex inserts a new word, we insert
  // a placeholder gap (0) at that position to keep the vector in sync; the remainder
  // is always the first word on the next line so its spacing is never used.
  std::vector<int> interWordGaps(wordWidths.size(), 0);
  for (size_t j = 1; j < wordWidths.size(); ++j) {
    if (!continuesVec[j]) {
      interWordGaps[j] =
          renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    } else {
      interWordGaps[j] =
          renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    }
  }

  std::vector<size_t> lineBreakIndices;
  lineEndsWithHyphenatedWord.clear();
  splitPrefixWordIndexes.clear();
  splitInsertedHyphen.clear();
  size_t currentIndex = 0;
  int lineIdx = 0;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    bool lineEndedWithHyphenation = false;
    int splitPrefixIndex = -1;
    bool splitNeedsInsertedHyphen = false;

    const int effectivePageWidth =
        widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (lineIdx == 0 ? firstLineIndent : 0);

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord ? 0 : interWordGaps[currentIndex];
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      bool insertedHyphen = false;
      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &insertedHyphen)) {
        // Keep interWordGaps in sync: insert placeholder for the new remainder word.
        // The remainder is always the first word on the next line so this slot is never read.
        interWordGaps.insert(interWordGaps.begin() + currentIndex + 1, 0);
        lineEndedWithHyphenation = true;
        splitPrefixIndex = static_cast<int>(currentIndex);
        splitNeedsInsertedHyphen = insertedHyphen;
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    if (lineEndedWithHyphenation &&
        (splitPrefixIndex < static_cast<int>(lineStart) || splitPrefixIndex >= static_cast<int>(currentIndex))) {
      lineEndedWithHyphenation = false;
      splitPrefixIndex = -1;
      splitNeedsInsertedHyphen = false;
    }

    lineBreakIndices.push_back(currentIndex);
    lineEndsWithHyphenatedWord.push_back(lineEndedWithHyphenation);
    splitPrefixWordIndexes.push_back(splitPrefixIndex);
    splitInsertedHyphen.push_back(splitNeedsInsertedHyphen);
    ++lineIdx;
  }

  return lineBreakIndices;
}

std::vector<size_t> ParsedText::computeHyphenatedLineBreaksFromIndex(
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    std::vector<bool>& continuesVec, const size_t startIndex, std::vector<bool>& lineEndsWithHyphenatedWord,
    std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen, const int16_t blockStartY,
    const int lineHeight, const int startLineIdx) {
  // Same greedy hyphenating breaker as the full pass, but scoped to a suffix.
  if (startIndex >= wordWidths.size()) {
    lineEndsWithHyphenatedWord.clear();
    splitPrefixWordIndexes.clear();
    splitInsertedHyphen.clear();
    return {};
  }

  std::vector<int> interWordGaps(wordWidths.size(), 0);
  for (size_t j = 1; j < wordWidths.size(); ++j) {
    if (!continuesVec[j]) {
      interWordGaps[j] =
          renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    } else {
      interWordGaps[j] =
          renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    }
  }

  std::vector<size_t> lineBreakIndices;
  lineEndsWithHyphenatedWord.clear();
  splitPrefixWordIndexes.clear();
  splitInsertedHyphen.clear();

  size_t currentIndex = startIndex;
  int lineIdx = startLineIdx;
  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    bool lineEndedWithHyphenation = false;
    int splitPrefixIndex = -1;
    bool splitNeedsInsertedHyphen = false;
    const int effectivePageWidth = widthForLine(lineIdx, lineHeight, blockStartY, pageWidth);

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord ? 0 : interWordGaps[currentIndex];
      const int candidateWidth = spacing + wordWidths[currentIndex];

      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;

      bool insertedHyphen = false;
      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &insertedHyphen)) {
        interWordGaps.insert(interWordGaps.begin() + currentIndex + 1, 0);
        lineEndedWithHyphenation = true;
        splitPrefixIndex = static_cast<int>(currentIndex);
        splitNeedsInsertedHyphen = insertedHyphen;
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    if (lineEndedWithHyphenation &&
        (splitPrefixIndex < static_cast<int>(lineStart) || splitPrefixIndex >= static_cast<int>(currentIndex))) {
      lineEndedWithHyphenation = false;
      splitPrefixIndex = -1;
      splitNeedsInsertedHyphen = false;
    }

    lineBreakIndices.push_back(currentIndex);
    lineEndsWithHyphenatedWord.push_back(lineEndedWithHyphenation);
    splitPrefixWordIndexes.push_back(splitPrefixIndex);
    splitInsertedHyphen.push_back(splitNeedsInsertedHyphen);
    ++lineIdx;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks, bool* outInsertedHyphen) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  if (outInsertedHyphen) {
    *outInsertedHyphen = chosenNeedsHyphen;
  }
  return true;
}

ParsedText::LineProcessResult ParsedText::extractLine(
    const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
    const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
    const std::function<LineProcessResult(std::shared_ptr<TextBlock>, bool, bool)>& processLine,
    const GfxRenderer& renderer, const int fontId, const bool lineEndsWithHyphenatedWord,
    const bool suppressHyphenationRetry, const int firstLineIndent, const int16_t blockStartY, const int lineHeight) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Apply indent only to line 0 of the layout pass; firstLineIndent is already
  // 0 for continuation flushes (computed once in layoutAndExtractLines).
  const int lineIndent = (breakIndex == 0) ? firstLineIndent : 0;

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation.
    // Gaps before closing punctuation (. , ) » etc.) are excluded from justification
    // distribution so they stay at natural space width.
    const uint32_t firstCp = firstCodepoint(words[lastBreakAt + wordIdx]);
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      const bool beforeClosing = isClosingPunctuation(firstCp);
      if (!beforeClosing) actualGapCount++;
      totalNaturalGaps +=
          static_cast<int>(renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]), firstCp,
                                                    wordStyles[lastBreakAt + wordIdx - 1]) *
                               blockStyle.fontSizeMultiplier +
                           0.5f);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
      // Non-breaking space tokens (" " with continues=true) are visible, stretchable spaces —
      // count them as justifiable gaps so justifyExtra is distributed to them too.
      if (words[lastBreakAt + wordIdx] == " ") {
        actualGapCount++;
      }
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps += static_cast<int>(renderer.getKerning(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]),
                                                               firstCp, wordStyles[lastBreakAt + wordIdx - 1]) *
                                               blockStyle.fontSizeMultiplier +
                                           0.5f);
    }
  }

  // Calculate spacing (account for float zone narrowing + first-line indent)
  const int effectivePageWidth =
      widthForLine(static_cast<int>(breakIndex), lineHeight, blockStartY, pageWidth) - lineIndent;
  // A line is only truly last when it consumes all paragraph words.
  // During single-line retry we may temporarily pass a truncated break vector,
  // so relying only on breakIndex would incorrectly disable justification.
  const bool isLastLine = lineBreak == words.size();

  // For justified text, compute per-gap extra to distribute remaining space evenly.
  // Cap stretch to effectivePageWidth/8 per gap to prevent rivers in sparse lines.
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1)
                               ? std::min(spareSpace / static_cast<int>(actualGapCount), effectivePageWidth / 8)
                               : 0;

  // Calculate initial x position (first line starts at indent for left/justified text;
  // may be negative for hanging indents, e.g. margin-left:3em; text-indent:-1em).
  auto xpos = static_cast<int16_t>(lineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
  }

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineXPos.push_back(xpos);

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    if (nextIsContinuation) {
      int advance = wordWidths[lastBreakAt + wordIdx];
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      advance += static_cast<int>(renderer.getKerning(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                                                      firstCodepoint(words[lastBreakAt + wordIdx + 1]),
                                                      wordStyles[lastBreakAt + wordIdx]) *
                                      blockStyle.fontSizeMultiplier +
                                  0.5f);
      // Non-breaking space tokens are stretchable — expand them during justification like normal spaces.
      if (words[lastBreakAt + wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
          blockStyle.alignment == CssTextAlign::Justify && !isLastLine) {
        advance += justifyExtra;
      }
      xpos += advance;
    } else {
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        const uint32_t nextFirstCp = firstCodepoint(words[lastBreakAt + wordIdx + 1]);
        gap = static_cast<int>(renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                                                        nextFirstCp, wordStyles[lastBreakAt + wordIdx]) *
                                   blockStyle.fontSizeMultiplier +
                               0.5f);
        // Don't stretch the gap before closing punctuation — it looks wrong with
        // extra space before ".", ")", "»" etc.
        const bool nextIsClosing = isClosingPunctuation(nextFirstCp);
        if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && !nextIsClosing) {
          gap += justifyExtra;
        }
      }
      xpos += wordWidths[lastBreakAt + wordIdx] + gap;
    }
  }

  // Copy line words; keep source intact so retry paths can safely inspect/merge tokens.
  std::vector<std::string> lineWords(words.begin() + lastBreakAt, words.begin() + lineBreak);
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  return processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle),
      lineEndsWithHyphenatedWord, suppressHyphenationRetry);
}
