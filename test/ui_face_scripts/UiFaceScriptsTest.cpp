// The Greek and Hebrew the built-in UI faces hold (lib/EpdFont/scripts/convert-builtin-fonts.sh, UI_SCRIPT_INTERVALS):
// modern Greek and the Hebrew letters and points, in every face a UI string can land in, so the keyboard's Greek and
// Hebrew layouts (freeink-sdk FreeInkUI, KeyboardLayoutId::GreekEl and HebrewIl) and their names draw from the face
// itself; and nothing of Greek beyond the modern set, which was left out for flash.
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

std::vector<uint32_t> codepointsOf(const char* utf8) {
  std::vector<uint32_t> cps;
  const auto* p = reinterpret_cast<const uint8_t*>(utf8);
  while (const uint32_t cp = utf8NextCodepoint(&p)) {
    if (cp != ' ') cps.push_back(cp);
  }
  return cps;
}

void expectEveryFaceHolds(const char* utf8) {
  for (const Face& face : FACES) {
    for (const uint32_t cp : codepointsOf(utf8))
      EXPECT_TRUE(holds(face.data, cp)) << face.name << " U+" << std::hex << cp;
  }
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
