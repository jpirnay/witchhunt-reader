#pragma once

#include <cstdint>

// The reader's Paragraph Indent, in the order the setting stores it (CrossPointSettings::PARAGRAPH_INDENT asserts it
// matches). FromBook, Book's Style, is the default: the book's text-indent, and one em where the book sets none and
// Extra Paragraph Spacing is off (not on <pre>). The others are a first-line indent the reader chose for body
// paragraphs, in ems of the paragraph's font.
enum class ParagraphIndent : uint8_t { None = 0, HalfEm, OneEm, OneAndHalfEm, TwoEm, FromBook };

// The fixed steps, None to TwoEm: every option but Book's Style.
constexpr uint8_t kParagraphIndentFixedSteps = static_cast<uint8_t>(ParagraphIndent::FromBook);

namespace paragraph_indent_detail {
constexpr uint8_t kTenthsOfEm[kParagraphIndentFixedSteps] = {0, 5, 10, 15, 20};
// The labels of the steps with a size, HalfEm to TwoEm. "em" is the typographic unit, not a word to translate.
constexpr const char* const kLabels[kParagraphIndentFixedSteps - 1] = {"0.5 em", "1 em", "1.5 em", "2 em"};
}  // namespace paragraph_indent_detail

// The label of a step with a size, from HalfEm on ("0.5 em"); nullptr past them. A plain function, so a settings row
// can take it as its literal options (SettingInfo::withLiteralOptions).
inline const char* paragraphIndentSizeLabel(const uint8_t index) {
  return index < kParagraphIndentFixedSteps - 1 ? paragraph_indent_detail::kLabels[index] : nullptr;
}

// What decides a paragraph's first-line indent, read off its block when it is laid out.
struct IndentInputs {
  // The book's CSS sets a text-indent, 0 included, and its size in pixels.
  bool bookDefined = false;
  int16_t bookPx = 0;
  // The indent is a span's margin-left, the shape of a poem's line, not the paragraph's own (BlockStyle).
  bool spanIndent = false;
  // The block goes on with its paragraph after a <br>.
  bool brContinuation = false;
  // Justified, or aligned to the start of the line: the left, the right in a right-to-left paragraph.
  bool startAligned = false;
  // A body paragraph: not a heading, a list item, a table cell or <pre>, nor a block inside one.
  bool bodyParagraph = false;
  // <pre> or a block inside one: laid out line by line as the book wrote it, so no indent but the book's own.
  bool preformatted = false;
  // A float or a drop cap sits beside the first lines.
  bool besideFloat = false;
  bool extraParagraphSpacing = false;
};

struct ResolvedIndent {
  int16_t px = 0;
  bool defined = false;  // false: no indent at all (BlockStyle::textIndentDefined)
};

// The first-line indent a block is laid out with, for the reader's `setting` and the block's font em of `emPx`.
//
// Book's Style is the book's indent, else one em on a justified or start-aligned block that does not go on after a
// <br>, as long as Extra Paragraph Spacing is off and no float sits beside it: the rule from before the setting, list
// items included, but not on <pre>, where it shifted a code block's or a verse's first line against the rest. A fixed
// step replaces the book's indent, positive or zero, on a body paragraph that is justified or start-aligned, whether
// Extra Paragraph Spacing is on or not: the reader chose it. Everything else keeps Book's Style: a hanging (negative)
// indent, a poem line's span indent, a <br> continuation, a heading, list item, table cell or <pre>, a block beside a
// float or a drop cap, and a centred or end-aligned one.
constexpr ResolvedIndent resolveParagraphIndent(const uint8_t setting, const IndentInputs& in, const int emPx) {
  ResolvedIndent bookStyle;
  if (in.bookDefined) {
    bookStyle = {in.bookPx, true};
  } else if (!in.brContinuation && !in.extraParagraphSpacing && !in.besideFloat && in.startAligned &&
             !in.preformatted) {
    bookStyle = {static_cast<int16_t>(emPx), true};
  }
  if (setting >= kParagraphIndentFixedSteps) return bookStyle;
  const bool readersToSet = in.bodyParagraph && in.startAligned && !in.brContinuation && !in.besideFloat &&
                            !in.spanIndent && !(in.bookDefined && in.bookPx < 0);
  if (!readersToSet) return bookStyle;
  const int tenths = paragraph_indent_detail::kTenthsOfEm[setting];
  return {static_cast<int16_t>((tenths * emPx + 5) / 10), true};
}
