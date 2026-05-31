#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
 public:
  enum class LineProcessResult {
    Accepted,
    RetryWithoutHyphenation,
  };

 private:
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;  // true = word attaches to previous (no space before it)
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool bionicReadingEnabled;
  bool isContinuation_ = false;       ///< true after an intermediate flush; suppresses re-applying paragraph indent
  size_t bionicTransformedUpTo_ = 0;  ///< words[0..bionicTransformedUpTo_) have already been bionic-transformed

  void applyParagraphIndent();
  void applyBionicReadingTransform();
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        int firstLineIndent);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& lineEndsWithHyphenatedWord,
                                                  std::vector<int>& splitPrefixWordIndexes,
                                                  std::vector<bool>& splitInsertedHyphen, int firstLineIndent);
  // Recompute hyphenated breaks for a suffix that starts at startIndex.
  // Used after a single-line retry so later lines keep normal hyphenation.
  std::vector<size_t> computeHyphenatedLineBreaksFromIndex(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                           std::vector<uint16_t>& wordWidths,
                                                           std::vector<bool>& continuesVec, size_t startIndex,
                                                           std::vector<bool>& lineEndsWithHyphenatedWord,
                                                           std::vector<int>& splitPrefixWordIndexes,
                                                           std::vector<bool>& splitInsertedHyphen);
  // Compute exactly one line break without hyphenating words.
  // Used only for the page-boundary retry line.
  size_t computeSingleLineBreakNoHyphen(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                                        size_t lineStartIndex, int firstLineIndent) const;
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks,
                            bool* outInsertedHyphen = nullptr);
  LineProcessResult extractLine(
      size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
      const std::vector<size_t>& lineBreakIndices,
      const std::function<LineProcessResult(std::shared_ptr<TextBlock>, bool, bool)>& processLine,
      const GfxRenderer& renderer, int fontId, bool lineEndsWithHyphenatedWord, bool suppressHyphenationRetry,
      int firstLineIndent);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer,
                                            int fontId);  // uses blockStyle.fontSizeMultiplier internally

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const bool bionicReadingEnabled = false)
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        bionicReadingEnabled(bionicReadingEnabled) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  bool isContinuation() const { return isContinuation_; }
  void layoutAndExtractLines(
      const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
      const std::function<LineProcessResult(std::shared_ptr<TextBlock>, bool, bool)>& processLine,
      bool includeLastLine = true);
};