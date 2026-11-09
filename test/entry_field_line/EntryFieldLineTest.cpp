// EntryFieldLine: where the characters of a line of the keyboard's entry field stand once it is drawn in visual order,
// for the caret, the block cursor and a tap. The text is in logical order, as it was typed.
//
// Every character measures 10 px here, a mark drawn over its letter none, so an x reads as a drawn position.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>

#include "BidiUtils.h"
#include "EntryFieldLine.h"
#include "Utf8.h"

namespace {

std::string encode(const std::initializer_list<uint32_t> codepoints) {
  std::string utf8;
  for (const uint32_t cp : codepoints) utf8AppendCodepoint(cp, utf8);
  return utf8;
}

// שלום
const std::string SHALOM = encode({0x05E9, 0x05DC, 0x05D5, 0x05DD});

// The longest string measured: the whole line, in the order it was measured in.
std::string lastMeasured;

int measure(const void*, const char* utf8) {
  if (std::strlen(utf8) > lastMeasured.size()) lastMeasured = utf8;
  int width = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (const uint32_t cp = utf8NextCodepoint(&p)) {
    if (!BidiUtils::isTransparentMark(cp)) width += 10;
  }
  return width;
}

std::unique_ptr<EntryFieldLine> built(const std::string& line, const int level) {
  auto layout = std::make_unique<EntryFieldLine>();
  lastMeasured.clear();
  EXPECT_TRUE(layout->build(line.data(), line.size(), level, measure, nullptr));
  return layout;
}

}  // namespace

// A Hebrew word: its first letter on the right, the caret after the last one on its left.
TEST(EntryFieldLine, AHebrewWordRunsRightToLeft) {
  const auto line = built(SHALOM, 1);
  EXPECT_EQ(line->width(), 40);
  bool rtl = false;
  EXPECT_EQ(line->caretX(SHALOM.size(), &rtl), 0);  // after ם: where the next letter goes
  EXPECT_TRUE(rtl);
  EXPECT_EQ(line->caretX(0), 40);  // before ש
  EXPECT_EQ(line->caretX(2), 30);  // after ש

  int x = 0, advance = 0;
  line->charSpan(0, x, advance);  // ש
  EXPECT_EQ(x, 30);
  EXPECT_EQ(advance, 10);
  line->charSpan(6, x, advance);  // ם
  EXPECT_EQ(x, 0);

  // A tap on the right half of ש is before it, on its left half after it.
  EXPECT_EQ(line->positionAt(37), 0u);
  EXPECT_EQ(line->positionAt(32), 2u);
  // Off either end: the end of the word on the left, its start on the right.
  EXPECT_EQ(line->positionAt(-5), SHALOM.size());
  EXPECT_EQ(line->positionAt(100), 0u);
}

// "שלום abc" in a right-to-left field: the Latin word stands left of the Hebrew one and reads left to right.
TEST(EntryFieldLine, LatinInARightToLeftField) {
  const std::string text = SHALOM + " abc";  // bytes: ש0 ל2 ו4 ם6 space8 a9 b10 c11
  const auto line = built(text, 1);
  EXPECT_EQ(lastMeasured, "abc " + encode({0x05DD, 0x05D5, 0x05DC, 0x05E9}));
  bool rtl = true;
  EXPECT_EQ(line->caretX(text.size(), &rtl), 30);  // after c, which is drawn at 20
  EXPECT_FALSE(rtl);
  EXPECT_EQ(line->caretX(8), 40);                // after ם, left of the Hebrew word
  EXPECT_EQ(line->caretX(9), 30);                // after the space, which belongs to the Hebrew run
  EXPECT_EQ(line->positionAt(3), 9u);            // left half of a: before it
  EXPECT_EQ(line->positionAt(27), text.size());  // right half of c: after it
}

// "abc שלום" in a left-to-right field: typing on after ם puts the next letter left of it, which is where the caret is.
TEST(EntryFieldLine, HebrewInALeftToRightField) {
  const std::string text = "abc " + SHALOM;  // bytes: a0 b1 c2 space3 ש4 ל6 ו8 ם10
  auto line = built(text, 0);
  EXPECT_EQ(lastMeasured, "abc " + encode({0x05DD, 0x05D5, 0x05DC, 0x05E9}));
  bool rtl = false;
  EXPECT_EQ(line->caretX(text.size(), &rtl), 40);
  EXPECT_TRUE(rtl);
  EXPECT_EQ(line->caretX(4), 40);  // after the space, before ש

  // Typed: נ lands at 40, and the caret after it stays there.
  const std::string typed = text + encode({0x05E0});
  line = built(typed, 0);
  int x = 0, advance = 0;
  line->charSpan(text.size(), x, advance);
  EXPECT_EQ(x, 40);
  EXPECT_EQ(line->caretX(typed.size()), 40);
}

// Digits read left to right in a right-to-left field: after "123" the caret stands right of the 3.
TEST(EntryFieldLine, DigitsInARightToLeftField) {
  const std::string text = SHALOM + " 123";
  const auto line = built(text, 1);
  EXPECT_EQ(lastMeasured, "123 " + encode({0x05DD, 0x05D5, 0x05DC, 0x05E9}));
  bool rtl = true;
  EXPECT_EQ(line->caretX(text.size(), &rtl), 30);
  EXPECT_FALSE(rtl);
  // After the space and before the 1, the caret goes with the space: at its left edge, where a Hebrew letter typed
  // there is drawn. (A digit typed there lands left of the 1: one position, two places, as in any bidi editor.)
  EXPECT_EQ(line->caretX(9), 30);
}

// Plain Latin at either direction: positions as a left-to-right line has them.
TEST(EntryFieldLine, ALatinLineStaysInOrder) {
  const auto line = built("abc", 0);
  EXPECT_EQ(line->caretX(0), 0);
  EXPECT_EQ(line->caretX(2), 20);
  EXPECT_EQ(line->caretX(3), 30);
  EXPECT_EQ(line->positionAt(14), 1u);
  EXPECT_EQ(line->positionAt(16), 2u);
}

// A mark is drawn over its letter: a caret after it stands where it would after the letter.
TEST(EntryFieldLine, AMarkGoesWithItsLetter) {
  const std::string text = encode({0x05D1, 0x05B0, 0x05D0});  // בְא: bet, sheva, alef
  const auto line = built(text, 1);
  EXPECT_EQ(line->width(), 20);
  EXPECT_EQ(line->caretX(4), 10);  // after the sheva, between bet and alef
  EXPECT_EQ(line->caretX(2), 10);  // between bet and its sheva: with the letter
  EXPECT_EQ(line->positionAt(15), 0u);
}

// What is measured is what visualUiLine draws: the same order, mirrored brackets, marks after their letter.
TEST(EntryFieldLine, MeasuresTheLineVisualUiLineDraws) {
  for (const std::string& text : {SHALOM, "abc " + SHALOM + " def", "(" + SHALOM + ") 12", std::string("abc def."),
                                  encode({0x05D1, 0x05B0, 0x05BC, 0x05E8, 0x20, 'x', 0x200D, 'y'}),
                                  encode({0x0627, 0x0644, 0x0639, 0x0631, 0x0628, 0x064A, 0x0629, 0x20, '4', '2'})}) {
    for (const int level : {0, 1}) {
      built(text, level);
      std::string drawn = BidiUtils::visualUiLine(text, level);
      // visualUiLine leaves a zero-width joiner out, as the measurement does.
      EXPECT_EQ(lastMeasured, drawn) << text << " at " << level;
    }
  }
}

// سلام as the field shows it where the font has the forms: Seen, Lam-Alef, Meem, three joined glyphs. The caret is
// carried from the text to the formed line (arabicUiFormsOffset) and a tap back (arabicUiFormsSource).
const std::string SALAM = encode({0x0633, 0x0644, 0x0627, 0x0645});

TEST(EntryFieldLine, AnArabicWordIsMeasuredJoined) {
  const std::string formed = BidiUtils::arabicUiForms(SALAM);
  const auto line = built(formed, 1);
  EXPECT_EQ(line->width(), 30);  // three glyphs, not four letters
  EXPECT_EQ(lastMeasured, BidiUtils::visualUiLine(formed, 1));
  const auto caretAt = [&](const size_t textPos) {
    return line->caretX(BidiUtils::arabicUiFormsOffset(SALAM, textPos));
  };
  EXPECT_EQ(caretAt(0), 30);            // before Seen, on the right
  EXPECT_EQ(caretAt(2), 20);            // after Seen: between it and the Lam-Alef
  EXPECT_EQ(caretAt(6), 10);            // after the Alef: between the Lam-Alef and Meem
  EXPECT_EQ(caretAt(SALAM.size()), 0);  // after Meem, on the left
  const auto tapAt = [&](const int x) { return BidiUtils::arabicUiFormsSource(SALAM, line->positionAt(x)); };
  EXPECT_EQ(tapAt(27), 0u);  // right half of Seen: before it
  EXPECT_EQ(tapAt(22), 2u);  // left half of Seen: after it
  EXPECT_EQ(tapAt(12), 6u);  // left half of the Lam-Alef: after both its letters
  EXPECT_EQ(tapAt(3), SALAM.size());
}

TEST(EntryFieldLine, ArabicAndLatinTogether) {
  // "سلام abc" in a right-to-left field: abc on the left, reading left to right, the joined word right of it.
  const std::string text = SALAM + " abc";
  const std::string formed = BidiUtils::arabicUiForms(text);
  auto line = built(formed, 1);
  EXPECT_EQ(lastMeasured, BidiUtils::visualUiLine(formed, 1));
  EXPECT_EQ(line->width(), 70);
  EXPECT_EQ(line->caretX(BidiUtils::arabicUiFormsOffset(text, text.size())), 30);  // after c
  EXPECT_EQ(line->caretX(BidiUtils::arabicUiFormsOffset(text, 8)), 40);            // after Meem
  EXPECT_EQ(BidiUtils::arabicUiFormsSource(text, line->positionAt(42)), 8u);       // left half of Meem: after it
  EXPECT_EQ(BidiUtils::arabicUiFormsSource(text, line->positionAt(47)), 6u);       // right half: before it

  // "abc سلام" in a left-to-right one: the caret after Meem stands where the next letter joins on, left of it.
  const std::string ltr = "abc " + SALAM;
  const std::string ltrFormed = BidiUtils::arabicUiForms(ltr);
  line = built(ltrFormed, 0);
  EXPECT_EQ(lastMeasured, BidiUtils::visualUiLine(ltrFormed, 0));
  bool rtl = false;
  EXPECT_EQ(line->caretX(BidiUtils::arabicUiFormsOffset(ltr, ltr.size()), &rtl), 40);
  EXPECT_TRUE(rtl);
  EXPECT_EQ(line->caretX(BidiUtils::arabicUiFormsOffset(ltr, 6)), 60);  // after Seen
}

// orderLine's limit is the line's: past it, visualUiLine draws the line as it is, and the caller lays it out so.
TEST(EntryFieldLine, ALinePastTheBidiLimitIsNotOrdered) {
  std::string text;
  for (size_t i = 0; i <= BidiUtils::kMaxLineCodepoints; ++i) text += encode({0x05D0});
  auto line = std::make_unique<EntryFieldLine>();
  EXPECT_FALSE(line->build(text.data(), text.size(), 1, measure, nullptr));
  EXPECT_EQ(BidiUtils::visualUiLine(text, 1), text);
}

// A field holds no bidi workspace once its line is laid out.
TEST(EntryFieldLine, LeavesNoWorkspace) {
  BidiUtils::releaseWorkspace();
  built(SHALOM + " abc", 1);
  EXPECT_FALSE(BidiUtils::heapWorkspaceHeld());
}
