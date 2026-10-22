// The .otf families the font manager hosts (assets/sd-fonts), as a reader installs them: each file probed as
// discovery probes it, its family installed into a glyph store with the X3's font area and mapping budget, and a
// sample of its characters drawn at 12 pt (spec 9).
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "EpdFontFamily.h"
#include "SimNorFlash.h"
#include "TtfFamily.h"
#include "TtfFont.h"
#include "TtfFontTestUtil.h"

using namespace ttf_test;

namespace {

constexpr uint32_t kX3FontAreaBytes = 3538944 - 65536;  // spiffs below the language slot
constexpr uint8_t kIdentity[glyphstore::kIdentityBytes] = {'s', 'h', 'i', 'p', 'p', 'e', 'd', '1'};
constexpr uint32_t kSerifTcOversize = 0x9F98;  // 621 points, over the 512-point cap (spec 9.2)

std::vector<std::filesystem::path> shippedOutlineFiles() {
  std::vector<std::filesystem::path> files;
  for (const auto& family : std::filesystem::directory_iterator(SD_FONTS_DIR)) {
    if (!family.is_directory()) continue;
    for (const auto& file : std::filesystem::directory_iterator(family.path())) {
      if (ttf::isOutlineFontFileName(file.path().filename().generic_string().c_str())) files.push_back(file.path());
    }
  }
  return files;
}

std::vector<uint8_t> readAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  std::vector<uint8_t> data(static_cast<size_t>(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return data;
}

}  // namespace

TEST(ShippedOtfFamilies, EachInstallsWithinTheX3StoreAndDraws) {
  const std::vector<std::filesystem::path> files = shippedOutlineFiles();
  EXPECT_EQ(files.size(), 8u);
  for (const auto& path : files) {
    SCOPED_TRACE(path.filename().generic_string());
    MemoryFile file;
    file.bytes = readAll(path);
    const ttf::FileSource source = file.source();

    // Discovery: the family's regular file, which fits the X3's store.
    const ttf::FileProbe probe = ttf::probeFile(source);
    EXPECT_EQ(0, probe.style);
    EXPECT_EQ(400, probe.weight);
    EXPECT_FALSE(probe.variable);
    const uint32_t bytes = static_cast<uint32_t>(file.bytes.size());
    EXPECT_TRUE(ttf::fitsStore(&bytes, &probe.sdBytes, 1, kX3FontAreaBytes, 32));

    // Installed alone and drawn: a spread of the characters it maps, and U+FFFD, which stands in for those it does not.
    SimNorFlash flash(kX3FontAreaBytes);
    TtfFontSet set(flash);
    TtfFontSet::Files families[1] = {{source}};
    ASSERT_TRUE(set.load(families, 1, kIdentity));
    TtfFont& font = *set.font(0);
    ASSERT_TRUE(font.setBodySize(12));
    FontBytes fontBytes(file.bytes.data(), bytes);
    ttf::Face face;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(fontBytes, &face));
    uint32_t mapped = 0;
    uint32_t drawn = 0;
    for (uint32_t cp = 0x20; cp < 0x30000; ++cp) {
      if (ttf::glyphIndex(fontBytes, face, cp) == 0) continue;
      const bool sampled = mapped++ % 97 == 0 || cp == 0xFFFD;
      if (!sampled || cp == kSerifTcOversize) continue;
      const EpdGlyph* glyph = font.glyph(0, cp);
      ASSERT_NE(nullptr, glyph) << "U+" << std::hex << cp;
      EXPECT_GT(glyph->advanceX, 0u) << "U+" << std::hex << cp;
      ++drawn;
    }
    EXPECT_GT(mapped, 7000u);
    EXPECT_GT(drawn, 70u);
    EXPECT_NE(0u, ttf::glyphIndex(fontBytes, face, 0xFFFD));

    // The families are regular only: bold draws through the regular face (synthetic).
    // 漢 (U+6F22) where the language writes it, 汉 (U+6C49) in the simplified Chinese families, which do not map 漢.
    const uint32_t han = ttf::glyphIndex(fontBytes, face, 0x6F22) != 0 ? 0x6F22 : 0x6C49;
    EXPECT_NE(nullptr, font.glyph(EpdFontFamily::BOLD, han)) << "U+" << std::hex << han << " bold";
  }
}

TEST(ShippedOtfFamilies, SerifTcDrawsTheOneOversizeGlyphAsMissing) {
  const std::filesystem::path path = std::filesystem::path(SD_FONTS_DIR) / "NotoSerifTC" / "NotoSerifTC-Regular.otf";
  ASSERT_TRUE(std::filesystem::exists(path));
  MemoryFile file;
  file.bytes = readAll(path);
  SimNorFlash flash(kX3FontAreaBytes);
  TtfFontSet set(flash);
  TtfFontSet::Files families[1] = {{file.source()}};
  ASSERT_TRUE(set.load(families, 1, kIdentity));
  TtfFont& font = *set.font(0);
  ASSERT_TRUE(font.setBodySize(12));
  FontBytes fontBytes(file.bytes.data(), static_cast<uint32_t>(file.bytes.size()));
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(fontBytes, &face));
  EXPECT_NE(0u, ttf::glyphIndex(fontBytes, face, kSerifTcOversize));
  EXPECT_EQ(nullptr, font.glyph(0, kSerifTcOversize));
  EXPECT_NE(0u, ttf::glyphIndex(fontBytes, face, 0x9F95));  // U+9F97 is not mapped in the TC set
  EXPECT_NE(nullptr, font.glyph(0, 0x9F95));
}
