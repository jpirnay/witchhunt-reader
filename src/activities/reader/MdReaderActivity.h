#pragma once

#include <MdParser.h>
#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "LineReaderActivity.h"

struct MdHeading {
  size_t offset = 0;
  int level = 1;
  std::string title;
  int pageIndex = -1;
};

class MdReaderActivity final : public LineReaderActivity {
  // A single rendered line on screen (after word-wrapping)
  struct RenderedLine {
    std::vector<MdParser::Span> spans;
    int indent = 0;     // left indent in pixels
    bool isHR = false;  // draw as horizontal rule
    bool isCodeBlock = false;
  };

  std::vector<uint8_t> pageCodeBlockState;  // 1 if page starts inside a code block
  std::vector<RenderedLine> currentPageLines;
  std::vector<uint8_t> pageBuffer;

  std::vector<MdHeading> headings;
  int currentHeadingIndex = -1;

  // Indent constants (in pixels)
  static constexpr int LIST_INDENT = 20;
  static constexpr int BLOCKQUOTE_INDENT = 16;
  static constexpr int CODE_INDENT = 8;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, bool startInCodeBlock, std::vector<RenderedLine>& outLines, size_t& nextOffset,
                        bool& endInCodeBlock);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void scanHeadings();
  void assignHeadingPageNumbers();
  int getHeadingIndexForOffset(size_t offset) const;
  void jumpToHeading(bool next);

  // Word-wrap a parsed markdown line into one or more RenderedLines.
  // Returns true if all content was emitted, false if truncated by maxLines.
  bool wordWrapParsedLine(const MdParser::ParsedLine& parsed, int indent, std::vector<RenderedLine>& outLines,
                          int maxLines, bool isCodeBlock = false);

  // Measure total pixel width of a span list
  int measureSpans(const std::vector<MdParser::Span>& spans) const;

 protected:
  void onReaderExit() override;
  // Opens the ToC overlay when the document has headings.
  bool onConfirmShortPress() override;
  // Keeps currentHeadingIndex in sync with the page shown.
  void onPageChanged() override;

 public:
  explicit MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : LineReaderActivity("MdReader", "MDR", renderer, mappedInput, std::move(txt)) {}
  void render(RenderLock&&) override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;
};
