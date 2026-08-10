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

 public:
  // Per-word font-size bounds, percent of the block's font size (microreader-style).
  // Inline CSS font-size composes multiplicatively through nested elements; anything
  // outside this range is clamped so a uint8_t always holds the value.
  static constexpr uint8_t MIN_WORD_SIZE_PCT = 30;
  static constexpr uint8_t MAX_WORD_SIZE_PCT = 250;
  static constexpr uint8_t DEFAULT_WORD_SIZE_PCT = 100;

 private:
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;  // true = word attaches to previous (no space before it)
  // Per-word font size, percent of the block font size (100 = block size). Kept in
  // lockstep with `words` through every insert/erase; the sizes are handed to each
  // TextBlock line so inline font-size spans survive into the page cache.
  std::vector<uint8_t> wordSizes;
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool bionicReadingEnabled;
  bool isContinuation_ = false;       ///< true after an intermediate flush; suppresses re-applying paragraph indent
  size_t bionicTransformedUpTo_ = 0;  ///< words[0..bionicTransformedUpTo_) have already been bionic-transformed

  void applyParagraphIndent(const GfxRenderer& renderer, int fontId);
  void applyBionicReadingTransform();
  // Effective measurement scale of words[i]: the block-level multiplier combined
  // with the word's own inline size percentage.
  float wordScale(const size_t i) const { return blockStyle.fontSizeMultiplier * (wordSizes[i] / 100.0f); }
  // Returns the available line width at a given 0-based line index, accounting
  // for any active float zones in blockStyle.  lineHeight==0 is a fast path
  // (no float zones active) that returns pageWidth unchanged.
  int widthForLine(int lineIndex, int lineHeight, int16_t blockStartY, int pageWidth) const;

  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        int firstLineIndent, int16_t blockStartY, int lineHeight);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& lineEndsWithHyphenatedWord,
                                                  std::vector<int>& splitPrefixWordIndexes,
                                                  std::vector<bool>& splitInsertedHyphen, int firstLineIndent,
                                                  int16_t blockStartY, int lineHeight);
  // Recompute hyphenated breaks for a suffix that starts at startIndex.
  // Used after a single-line retry so later lines keep normal hyphenation.
  std::vector<size_t> computeHyphenatedLineBreaksFromIndex(
      const GfxRenderer& renderer, int fontId, int pageWidth, std::vector<uint16_t>& wordWidths,
      std::vector<bool>& continuesVec, size_t startIndex, std::vector<bool>& lineEndsWithHyphenatedWord,
      std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen, int16_t blockStartY = 0,
      int lineHeight = 0, int startLineIdx = 0);
  // Compute exactly one line break without hyphenating words.
  // Used only for the page-boundary retry line.
  size_t computeSingleLineBreakNoHyphen(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                                        size_t lineStartIndex, int firstLineIndent, int16_t blockStartY = 0,
                                        int lineHeight = 0) const;
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks,
                            bool* outInsertedHyphen = nullptr);
  LineProcessResult extractLine(
      size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
      const std::vector<size_t>& lineBreakIndices,
      const std::function<LineProcessResult(std::shared_ptr<TextBlock>, bool, bool)>& processLine,
      const GfxRenderer& renderer, int fontId, bool lineEndsWithHyphenatedWord, bool suppressHyphenationRetry,
      int firstLineIndent, int16_t blockStartY = 0, int lineHeight = 0);
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

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint8_t sizePct = DEFAULT_WORD_SIZE_PCT);
  // If every word shares one non-100% size (a span wrapping the whole paragraph, e.g.
  // Alice's mouse-tale lines), fold that percent into the block-level fontSizeMultiplier
  // and reset the per-word sizes to 100. This routes whole-paragraph spans through the
  // block font-resolution path (size ladder) so they render with a real smaller font
  // instead of per-word glyph scaling. Returns true when a fold happened. Must be called
  // before the first layout pass of the block; callers skip continuations.
  bool foldUniformWordSizes();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  bool isContinuation() const { return isContinuation_; }

  // CONSUMES ITS SOURCE by default: every word it lays out is erased from `words` (and the
  // parallel per-word vectors) before returning, so the vector shrinks as the paragraph drains.
  // A caller that needs to lay the SAME text out twice must pass preserveSource.
  //
  // This was previously documented as preserving its source, which was wrong and cost content: the
  // table grid path laid a cell out to discover it needed more lines than the grid can carry, then
  // handed the same ParsedText to emitTableAsParagraphs — by which point every word of that cell,
  // and of every cell laid out before it, had already been erased. The whole cell rendered as
  // nothing. Fixture: test_table_grid_edges.epub ch3.
  //
  // `preserveSource` snapshots the words on entry and restores them on return, so the caller gets
  // back exactly what it passed in. Suppressing the erase alone would not be enough: layout also
  // force-splits any word too wide for the line (inserting a hyphen), and applies the paragraph
  // indent and bionic transform in place. It costs one copy of the word vectors for the duration
  // of the call, so it is opt-in and only the table grid path uses it.
  void layoutAndExtractLines(
      const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
      const std::function<LineProcessResult(std::shared_ptr<TextBlock>, bool, bool)>& processLine,
      bool includeLastLine = true,
      int16_t blockStartY = 0,   // currentPageNextY at call site — needed for float zone geometry
      int lineHeight = 0,        // 0 = no float zones (fast path, existing callers unchanged)
      bool preserveSource = false);
};