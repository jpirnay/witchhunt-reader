#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_CID_H
#include <gtest/gtest.h>

#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {

FT_Library library() {
  static FT_Library lib = [] {
    FT_Library l = nullptr;
    FT_Init_FreeType(&l);
    return l;
  }();
  return lib;
}

}  // namespace

// The CFF test fonts are the ground truth for the CFF rasterizer: the oracle's FreeType must load each one.
TEST(CffFixtures, FreeTypeLoadsEveryCffTestFont) {
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> font = readCffTestFont(name);
    ASSERT_FALSE(font.empty()) << name;
    FT_Face face = nullptr;
    ASSERT_EQ(0, FT_New_Memory_Face(library(), font.data(), static_cast<FT_Long>(font.size()), 0, &face)) << name;
    EXPECT_GT(face->num_glyphs, 1) << name;
    const bool cid = name.rfind("Noto", 0) == 0;
    FT_Bool isCid = 0;
    FT_Get_CID_Is_Internally_CID_Keyed(face, &isCid);
    EXPECT_EQ(cid, isCid != 0) << name;
    for (FT_Long g = 0; g < face->num_glyphs; ++g) {
      EXPECT_EQ(0, FT_Load_Glyph(face, static_cast<FT_UInt>(g), FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP))
          << name << " glyph " << g;
    }
    FT_Done_Face(face);
  }
}
