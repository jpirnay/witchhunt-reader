// The Greek, Hebrew and Arabic the built-in UI faces hold (lib/EpdFont/scripts/convert-builtin-fonts.sh,
// UI_SCRIPT_INTERVALS): modern Greek, the Hebrew letters and points, and the Arabic presentation forms, in every face a
// UI string can land in, so the keyboard's Greek, Hebrew and Arabic layouts (freeink-sdk FreeInkUI,
// KeyboardLayoutId::GreekEl, HebrewIl and ArabicAr) and their names draw from the face itself; nothing of Greek beyond
// the modern set, which was left out for flash; and every form BidiUtils::arabicUiForms writes, with no font on the
// card.
#include <BidiUtils.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "builtinFonts/inter_ui_10_bold.h"
#include "builtinFonts/inter_ui_10_regular.h"
#include "builtinFonts/inter_ui_12_bold.h"
#include "builtinFonts/inter_ui_12_regular.h"
#include "builtinFonts/inter_ui_14_bold.h"
#include "builtinFonts/inter_ui_14_regular.h"
#include "builtinFonts/inter_ui_8_bold.h"
#include "builtinFonts/inter_ui_8_regular.h"
#include "builtinFonts/notosans_6_regular.h"
#include "builtinFonts/notosans_8_regular.h"

namespace {

struct Face {
  const char* name;
  const EpdFontData* data;
};

// ui_font_coverage.py's UI_FACE_HEADERS.
const Face FACES[] = {
    {"notosans_6_regular", &notosans_6_regular},   {"notosans_8_regular", &notosans_8_regular},
    {"inter_ui_8_regular", &inter_ui_8_regular},   {"inter_ui_8_bold", &inter_ui_8_bold},
    {"inter_ui_10_regular", &inter_ui_10_regular}, {"inter_ui_10_bold", &inter_ui_10_bold},
    {"inter_ui_12_regular", &inter_ui_12_regular}, {"inter_ui_12_bold", &inter_ui_12_bold},
    {"inter_ui_14_regular", &inter_ui_14_regular}, {"inter_ui_14_bold", &inter_ui_14_bold},
};

// The face's own table, no fallbacks: the faces carry no provider here.
bool holds(const EpdFontData* face, const uint32_t cp) { return static_cast<bool>(EpdFont(face).findGlyph(cp)); }

std::vector<uint32_t> codepointsOf(const std::string& utf8) {
  std::vector<uint32_t> cps;
  const auto* p = reinterpret_cast<const uint8_t*>(utf8.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) {
    if (cp != ' ') cps.push_back(cp);
  }
  return cps;
}

void expectEveryFaceHolds(const std::string& utf8) {
  for (const Face& face : FACES) {
    for (const uint32_t cp : codepointsOf(utf8))
      EXPECT_TRUE(holds(face.data, cp)) << face.name << " U+" << std::hex << cp;
  }
}

// convert-builtin-fonts.sh's UI_ARABIC_INTERVALS: every form the mapper writes (Forms-B and the Forms-A of minibidi.c's
// xshapeforms), the harakat and superscript alef, Arabic punctuation and tatweel, the Arabic-Indic and Persian digits.
struct Range {
  uint32_t first;
  uint32_t last;
};
constexpr Range ARABIC[] = {
    {0x060C, 0x060C}, {0x061B, 0x061B}, {0x061F, 0x061F}, {0x0640, 0x0640}, {0x064B, 0x0652},
    {0x0660, 0x066C}, {0x0670, 0x0670}, {0x06D4, 0x06D4}, {0x06F0, 0x06F9}, {0xFB50, 0xFBB1},
    {0xFBD3, 0xFBDC}, {0xFBDE, 0xFBE7}, {0xFBFC, 0xFBFF}, {0xFE80, 0xFEFC},
};

bool inArabicSet(const uint32_t cp) {
  for (const Range& range : ARABIC) {
    if (cp >= range.first && cp <= range.last) return true;
  }
  return false;
}

std::string repeated(const uint32_t cp, const int times) {
  std::string text;
  for (int i = 0; i < times; ++i) utf8AppendCodepoint(cp, text);
  return text;
}

}  // namespace

TEST(UiFaceScripts, TheGreekKeyboardAndItsNameDrawFromEveryFace) {
  // GreekEl's compact rows, each key with its long-press alternate, lower and upper layers.
  expectEveryFaceHolds("; ς εέ ρ τ υύ θ ιί οό π αά σ δ φ γ ηή ξ κ λ ϊΐ ϋΰ ζ χ ψ ωώ β ν μ");
  expectEveryFaceHolds(": ς ΕΈ Ρ Τ ΥΎ Θ ΙΊ ΟΌ Π ΑΆ Σ Δ Φ Γ ΗΉ Ξ Κ Λ Ϊ Ϋ Ζ Χ Ψ ΩΏ Β Ν Μ");
  expectEveryFaceHolds("Ελληνικά");
}

TEST(UiFaceScripts, ModernGreekWithItsPunctuationIsHeldWhole) {
  for (const Face& face : FACES) {
    for (uint32_t cp = 0x0384; cp <= 0x03CE; ++cp) {
      if (cp == 0x038B || cp == 0x038D || cp == 0x03A2) continue;  // unassigned
      EXPECT_TRUE(holds(face.data, cp)) << face.name << " U+" << std::hex << cp;
    }
    EXPECT_TRUE(holds(face.data, 0x037E)) << face.name;  // Greek question mark
  }
}

TEST(UiFaceScripts, GreekBeyondTheModernSetIsLeftOut) {
  for (const Face& face : FACES) {
    for (const uint32_t cp :
         {0x0370u, 0x0373u, 0x037Bu, 0x03CFu, 0x03D0u, 0x03D5u, 0x03DAu, 0x03E2u, 0x03F2u, 0x03FFu, 0x1F00u}) {
      EXPECT_FALSE(holds(face.data, cp)) << face.name << " U+" << std::hex << cp;
    }
  }
}

TEST(UiFaceScripts, TheHebrewKeyboardAndItsNameDrawFromEveryFace) {
  // HebrewIl's rows, and the points a vowelled title carries.
  expectEveryFaceHolds("ק ר א ט ו ן ם פ ף ש ד ג כ ע י ח ל ך ז ס ב ה נ מ צ ת ץ עברית");
  expectEveryFaceHolds("ְִַָּ־ׁׂ׳״");
}

TEST(UiFaceScripts, TheArabicSetIsHeldWholeAndNothingElseOfTheArabicBlocks) {
  for (const Face& face : FACES) {
    for (const Range block : {Range{0x0600, 0x06FF}, Range{0xFB50, 0xFDFF}, Range{0xFE70, 0xFEFF}}) {
      for (uint32_t cp = block.first; cp <= block.last; ++cp) {
        EXPECT_EQ(holds(face.data, cp), inArabicSet(cp)) << face.name << " U+" << std::hex << cp;
      }
    }
  }
}

TEST(UiFaceScripts, EveryFormTheMapperWritesDrawsFromEveryFace) {
  // Every Arabic letter alone, and as the initial, medial and final of a word of three: what arabicUiForms() writes is
  // a form every face holds, or a letter with no form, which stays in the Arabic block and in no face.
  size_t forms = 0;
  for (uint32_t letter = 0x0620; letter <= 0x06FF; ++letter) {
    for (const int times : {1, 3}) {
      for (const uint32_t cp : codepointsOf(BidiUtils::arabicUiForms(repeated(letter, times)))) {
        if (cp >= 0x0600 && cp <= 0x06FF) continue;
        ++forms;
        for (const Face& face : FACES) {
          EXPECT_TRUE(holds(face.data, cp)) << face.name << " U+" << std::hex << letter << " -> U+" << cp;
        }
      }
    }
  }
  EXPECT_GT(forms, 250u);  // the walk reaches the forms: 298 today
  // The Lam-Alef ligatures, alone and after a letter that joins them.
  for (const uint32_t alef : {0x0622u, 0x0623u, 0x0625u, 0x0627u}) {
    std::string lamAlef;
    utf8AppendCodepoint(0x0644, lamAlef);
    utf8AppendCodepoint(alef, lamAlef);
    const std::string afterBeh = repeated(0x0628, 1) + lamAlef;
    EXPECT_EQ(codepointsOf(BidiUtils::arabicUiForms(lamAlef)).size(), 1u);
    expectEveryFaceHolds(BidiUtils::arabicUiForms(lamAlef));
    expectEveryFaceHolds(BidiUtils::arabicUiForms(afterBeh));
  }
}

TEST(UiFaceScripts, ArabicPersianAndUrduTitlesDrawFormedFromEveryFace) {
  // العربية, کتاب فارسی, اردو ادب, and a vowelled كِتَابٌ: formed, every character is the face's own.
  for (const char* title : {"\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9",
                            "\xDA\xA9\xD8\xAA\xD8\xA7\xD8\xA8 \xD9\x81\xD8\xA7\xD8\xB1\xD8\xB3\xDB\x8C",
                            "\xD8\xA7\xD8\xB1\xD8\xAF\xD9\x88 \xD8\xA7\xD8\xAF\xD8\xA8",
                            "\xD9\x83\xD9\x90\xD8\xAA\xD9\x8E\xD8\xA7\xD8\xA8\xD9\x8C"}) {
    const std::string formed = BidiUtils::arabicUiForms(title);
    EXPECT_NE(formed, title);
    expectEveryFaceHolds(formed);
  }
  // Digits and punctuation stay as they are, and are held too: ٢٠٢٦، ۱۴۰۵؟
  expectEveryFaceHolds("\xD9\xA2\xD9\xA0\xD9\xA2\xD9\xA6\xD8\x8C \xDB\xB1\xDB\xB4\xDB\xB0\xDB\xB5\xD8\x9F");
}
