#pragma once

// One wrapped line of the keyboard's entry field with right-to-left text in it, as the field draws it. The text stays
// in logical order -- the order it was typed in, which the cursor, Delete and the wrap work on -- and the field draws
// each line through BidiUtils::visualUiLine at the paragraph's direction. This is the map between the two: where each
// character of the line stands once it is drawn, so the caret, the block cursor and a tap land on the right one.
//
// The order is BidiUtils::orderUiLine's, the one visualUiLine draws; the x of each character is measured on the line
// in that order, as GfxRenderer::getTextAdvanceX measures what drawFormedText draws.

#include <BidiUtils.h>

#include <cstddef>
#include <cstdint>

class EntryFieldLine {
 public:
  // The advance of `utf8` drawn as it is, left to right.
  using Measure = int (*)(const void* ctx, const char* utf8);

  // Orders line[0, len) at `paragraphLevel` (0 or 1) and measures it. False when it cannot be ordered: more than
  // BidiUtils::kMaxLineCodepoints code points, or no bidi workspace. visualUiLine then draws the line as it is, and the
  // caller places the caret as it does on a left-to-right line.
  bool build(const char* line, size_t len, int paragraphLevel, Measure measure, const void* ctx);

  int width() const { return edge[count]; }

  // The caret standing before the character at byte `pos` of the line (`pos` == its length: after the last one): its x,
  // and whether it follows a right-to-left character -- text typed there then grows to its left. A caret goes with the
  // character before it, the one just typed; at the start of a line, with the first one.
  int caretX(size_t pos, bool* rtl = nullptr) const;

  // The character at byte `pos`: the x it starts at and its advance.
  void charSpan(size_t pos, int& x, int& advance) const;

  // The byte position a tap at `x` puts the caret at: before or after the character under it, by the half it hit.
  size_t positionAt(int x) const;

 private:
  static constexpr size_t MAX = BidiUtils::kMaxLineCodepoints;

  // The index of the character that starts at byte `pos`; `count` at the end of the line.
  size_t indexOf(size_t pos) const;
  // The character a caret next to `i` goes by: a mark is placed with the letter it sits on.
  size_t baseOf(size_t i) const;

  size_t count = 0;
  uint16_t byteAt[MAX + 1] = {};  // character -> its first byte in the line; [count] = the line's length
  uint8_t slotOf[MAX] = {};       // character -> where it is drawn, counted from the left
  uint8_t charAt[MAX] = {};       // the reverse
  bool rtl[MAX] = {};             // the character runs right to left
  bool mark[MAX] = {};            // a transparent mark, drawn over the character before it
  int16_t edge[MAX + 1] = {};     // drawn position -> the x it starts at; [count] = the line's width

  // build()'s scratch: orderUiLine's input and output, and the line in drawn order as it is measured.
  uint32_t cps[MAX] = {};
  uint16_t visual[MAX] = {};
  uint32_t drawn[MAX] = {};
  char form[MAX * 4 + 1] = {};
};
