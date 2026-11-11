// The reader's Paragraph Indent: which first-line indent a block is laid out with (resolveParagraphIndent), for every
// setting against the book's indent, the block's alignment, a hanging indent, a <br> continuation, Extra Paragraph
// Spacing, a float beside it, and blocks that are not running text. One em is 18 px, as the layout tests' stub font.
#include <gtest/gtest.h>

#include <iterator>
#include <string>

#include "Epub/ParagraphIndent.h"

namespace {

constexpr int kEm = 18;
constexpr uint8_t kFromBook = static_cast<uint8_t>(ParagraphIndent::FromBook);
constexpr uint8_t kFixed[] = {
    static_cast<uint8_t>(ParagraphIndent::None), static_cast<uint8_t>(ParagraphIndent::HalfEm),
    static_cast<uint8_t>(ParagraphIndent::OneEm), static_cast<uint8_t>(ParagraphIndent::OneAndHalfEm),
    static_cast<uint8_t>(ParagraphIndent::TwoEm)};
// The fixed steps at one em of 18 px: 0, 0.5, 1, 1.5 and 2 em.
constexpr int kFixedPx[] = {0, 9, 18, 27, 36};

// The book's text-indent as the block carries it: none, 0, one and a half em, and hanging (-1 em, under a margin).
struct Book {
  const char* name;
  bool defined;
  int16_t px;
};
constexpr Book kBooks[] = {{"none", false, 0}, {"zero", true, 0}, {"positive", true, 27}, {"hanging", true, -18}};

// A justified body paragraph, extra spacing off: the block every rule leaves to the setting.
IndentInputs bodyParagraph(const Book& book) {
  IndentInputs in;
  in.bookDefined = book.defined;
  in.bookPx = book.px;
  in.startAligned = true;
  in.bodyParagraph = true;
  return in;
}

// What the layout did before the setting existed (ParsedText::applyParagraphIndent): the book's indent when it sets
// one, else one em on a start-aligned block that is not a <br> continuation, with extra spacing off and no float. Only
// <pre> has changed: it no longer takes that em.
ResolvedIndent booksStyle(const IndentInputs& in) {
  if (in.bookDefined) return {in.bookPx, true};
  if (!in.brContinuation && !in.extraParagraphSpacing && !in.besideFloat && in.startAligned && !in.preformatted) {
    return {static_cast<int16_t>(kEm), true};
  }
  return {0, false};
}

void expectIndent(const ResolvedIndent got, const ResolvedIndent want, const std::string& what) {
  EXPECT_EQ(got.defined, want.defined) << what;
  if (want.defined) EXPECT_EQ(got.px, want.px) << what;
}

// Every combination of the block's state, under Book's Style: today's rule, whatever kind of block it is.
TEST(ParagraphIndent, BooksStyleIsTheRuleBeforeTheSetting) {
  for (const Book& book : kBooks) {
    for (int mask = 0; mask < 128; ++mask) {
      IndentInputs in = bodyParagraph(book);
      in.startAligned = (mask & 1) != 0;
      in.brContinuation = (mask & 2) != 0;
      in.extraParagraphSpacing = (mask & 4) != 0;
      in.besideFloat = (mask & 8) != 0;
      in.bodyParagraph = (mask & 16) != 0;
      in.spanIndent = (mask & 32) != 0 && book.defined;
      in.preformatted = (mask & 64) != 0;
      expectIndent(resolveParagraphIndent(kFromBook, in, kEm), booksStyle(in),
                   std::string(book.name) + " mask " + std::to_string(mask));
    }
  }
}

// A fixed step replaces the book's positive or zero indent, and stands in where the book sets none, on a justified or
// start-aligned body paragraph, with Extra Paragraph Spacing on or off.
TEST(ParagraphIndent, AFixedStepReplacesAPositiveOrZeroIndent) {
  for (const Book& book : {kBooks[0], kBooks[1], kBooks[2]}) {
    for (const bool extraSpacing : {false, true}) {
      for (size_t i = 0; i < std::size(kFixed); ++i) {
        IndentInputs in = bodyParagraph(book);
        in.extraParagraphSpacing = extraSpacing;
        expectIndent(resolveParagraphIndent(kFixed[i], in, kEm), {static_cast<int16_t>(kFixedPx[i]), true},
                     std::string(book.name) + " step " + std::to_string(i) + (extraSpacing ? " spaced" : ""));
      }
    }
  }
}

TEST(ParagraphIndent, NoneRemovesAPositiveIndent) {
  const ResolvedIndent got = resolveParagraphIndent(kFixed[0], bodyParagraph(kBooks[2]), kEm);
  EXPECT_TRUE(got.defined);
  EXPECT_EQ(got.px, 0);
}

TEST(ParagraphIndent, AHangingIndentIsKept) {
  for (const uint8_t step : kFixed) {
    expectIndent(resolveParagraphIndent(step, bodyParagraph(kBooks[3]), kEm), {-18, true},
                 "step " + std::to_string(step));
  }
}

// What a fixed step leaves alone keeps Book's Style exactly: the same result as with the setting at Book's Style.
struct Exemption {
  const char* name;
  void (*apply)(IndentInputs&);
};
constexpr Exemption kExemptions[] = {
    {"centred or end-aligned", [](IndentInputs& in) { in.startAligned = false; }},
    {"<br> continuation", [](IndentInputs& in) { in.brContinuation = true; }},
    {"heading or list item", [](IndentInputs& in) { in.bodyParagraph = false; }},
    {"<pre>",
     [](IndentInputs& in) {
       in.bodyParagraph = false;
       in.preformatted = true;
     }},
    {"beside a float or drop cap", [](IndentInputs& in) { in.besideFloat = true; }},
};

TEST(ParagraphIndent, WhatAFixedStepLeavesAloneKeepsBooksStyle) {
  for (const Exemption& exemption : kExemptions) {
    for (const Book& book : kBooks) {
      for (const bool extraSpacing : {false, true}) {
        IndentInputs in = bodyParagraph(book);
        in.extraParagraphSpacing = extraSpacing;
        exemption.apply(in);
        const ResolvedIndent want = resolveParagraphIndent(kFromBook, in, kEm);
        for (const uint8_t step : kFixed) {
          expectIndent(resolveParagraphIndent(step, in, kEm), want,
                       std::string(exemption.name) + ", " + book.name + ", step " + std::to_string(step));
        }
      }
    }
  }
}

// A <pre>'s lines are the book's: no indent under any setting, Book's Style included, unless its CSS sets one, which
// stands, hanging or not.
TEST(ParagraphIndent, PreformattedTextTakesOnlyTheBooksIndent) {
  for (const Book& book : kBooks) {
    for (const bool extraSpacing : {false, true}) {
      IndentInputs in = bodyParagraph(book);
      in.bodyParagraph = false;
      in.preformatted = true;
      in.extraParagraphSpacing = extraSpacing;
      const ResolvedIndent want = book.defined ? ResolvedIndent{book.px, true} : ResolvedIndent{0, false};
      for (const uint8_t step : {kFixed[0], kFixed[1], kFixed[2], kFixed[3], kFixed[4], kFromBook}) {
        expectIndent(resolveParagraphIndent(step, in, kEm), want,
                     std::string(book.name) + ", step " + std::to_string(step) + (extraSpacing ? " spaced" : ""));
      }
    }
  }
}

// A list item keeps Book's Style's one em where its book sets none, under every setting.
TEST(ParagraphIndent, AListItemKeepsBooksStylesEm) {
  IndentInputs in = bodyParagraph(kBooks[0]);
  in.bodyParagraph = false;
  for (const uint8_t step : {kFixed[0], kFixed[1], kFixed[2], kFixed[3], kFixed[4], kFromBook}) {
    expectIndent(resolveParagraphIndent(step, in, kEm), {static_cast<int16_t>(kEm), true},
                 "step " + std::to_string(step));
  }
}

// A poem's line drawn indented by a span's margin keeps its shape.
TEST(ParagraphIndent, ASpanIndentIsKept) {
  IndentInputs in = bodyParagraph({"span", true, 40});
  in.spanIndent = true;
  for (const uint8_t step : kFixed) {
    expectIndent(resolveParagraphIndent(step, in, kEm), {40, true}, "step " + std::to_string(step));
  }
}

// Tenths of an em, rounded half up: at 29 px, half an em is 14.5 and one and a half 43.5.
TEST(ParagraphIndent, SizesFollowTheEm) {
  const int want[] = {0, 15, 29, 44, 58};
  for (size_t i = 0; i < std::size(kFixed); ++i) {
    EXPECT_EQ(resolveParagraphIndent(kFixed[i], bodyParagraph(kBooks[0]), 29).px, want[i]) << i;
  }
  // One em is exactly the one em Book's Style gives a paragraph with no indent of its own.
  EXPECT_EQ(resolveParagraphIndent(static_cast<uint8_t>(ParagraphIndent::OneEm), bodyParagraph(kBooks[0]), 29).px,
            resolveParagraphIndent(kFromBook, bodyParagraph(kBooks[0]), 29).px);
}

TEST(ParagraphIndent, AnUnknownSettingIsBooksStyle) {
  for (const Book& book : kBooks) {
    const IndentInputs in = bodyParagraph(book);
    expectIndent(resolveParagraphIndent(9, in, kEm), booksStyle(in), book.name);
  }
}

TEST(ParagraphIndent, LabelsAreTheSizesInEms) {
  EXPECT_STREQ(paragraphIndentSizeLabel(0), "0.5 em");
  EXPECT_STREQ(paragraphIndentSizeLabel(1), "1 em");
  EXPECT_STREQ(paragraphIndentSizeLabel(2), "1.5 em");
  EXPECT_STREQ(paragraphIndentSizeLabel(3), "2 em");
  EXPECT_EQ(paragraphIndentSizeLabel(4), nullptr);
}

}  // namespace
