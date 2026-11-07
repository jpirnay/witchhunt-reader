// The outline families the font manager hosts (assets/sd-fonts), as a reader installs them. The eight .otf CJK
// families: each file probed as discovery probes it, its family installed into a glyph store with the X3's font area
// and mapping budget, and a sample of its characters drawn at 12 pt (spec 9). The twenty .ttf Indic families: each
// file loaded, its style and weight read, and the family fitted into the X3's font area; each family's Regular file
// shaped alike through the CPac the install writes and through filters it computes.
#include <IndicScripts.h>
#include <OtFilterWriter.h>
#include <OtShaper.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
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

std::vector<std::filesystem::path> shippedOutlineFiles(const std::string& extension) {
  std::vector<std::filesystem::path> files;
  for (const auto& family : std::filesystem::directory_iterator(SD_FONTS_DIR)) {
    if (!family.is_directory()) continue;
    for (const auto& file : std::filesystem::directory_iterator(family.path())) {
      if (!ttf::isOutlineFontFileName(file.path().filename().generic_string().c_str())) continue;
      if (file.path().extension().generic_string() == extension) files.push_back(file.path());
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
  const std::vector<std::filesystem::path> files = shippedOutlineFiles(".otf");
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
    EXPECT_EQ(ttf::flashBlockBytes(bytes, probe.cpacBytes), probe.flashBytes);
    EXPECT_TRUE(ttf::fitsStore(&probe.flashBytes, &probe.sdBytes, 1, kX3FontAreaBytes, 32));

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

TEST(ShippedTtfFamilies, EachFileLoadsAndEachFamilyFitsTheX3Store) {
  const std::vector<std::filesystem::path> files = shippedOutlineFiles(".ttf");
  EXPECT_EQ(files.size(), 40u);
  std::map<std::string, std::vector<uint32_t>> flashBytes;  // by family directory
  std::map<std::string, std::vector<uint32_t>> sdBytes;
  for (const auto& path : files) {
    const std::string fileName = path.filename().generic_string();
    SCOPED_TRACE(fileName);
    const bool bold = fileName.find("-Bold.ttf") != std::string::npos;
    ASSERT_TRUE(bold || fileName.find("-Regular.ttf") != std::string::npos);
    MemoryFile file;
    file.bytes = readAll(path);
    const ttf::FileProbe probe = ttf::probeFile(file.source());
    EXPECT_EQ(bold ? static_cast<int>(EpdFontFamily::BOLD) : 0, probe.style);
    EXPECT_EQ(bold ? 700 : 400, probe.weight);
    EXPECT_FALSE(probe.variable);
    EXPECT_EQ(ttf::flashBlockBytes(static_cast<uint32_t>(file.bytes.size()), probe.cpacBytes), probe.flashBytes);

    // The file loads alone into the X3's store.
    SimNorFlash flash(kX3FontAreaBytes);
    TtfFontSet set(flash);
    TtfFontSet::Files families[1] = {{file.source()}};
    ASSERT_TRUE(set.load(families, 1, kIdentity));
    ASSERT_TRUE(set.font(0)->setBodySize(12));

    flashBytes[path.parent_path().filename().generic_string()].push_back(probe.flashBytes);
    sdBytes[path.parent_path().filename().generic_string()].push_back(probe.sdBytes);
  }
  EXPECT_EQ(flashBytes.size(), 20u);
  for (const auto& [family, flashSizes] : flashBytes) {
    SCOPED_TRACE(family);
    ASSERT_EQ(flashSizes.size(), 2u);
    // In flash mode: the SD blocks are given as impossible, so only the flash blocks can fit.
    const uint32_t noSdBlock[2] = {UINT32_MAX, UINT32_MAX};
    EXPECT_TRUE(ttf::fitsStore(flashSizes.data(), noSdBlock, 2, kX3FontAreaBytes, 32));
  }
}

// A Hindi or Sanskrit book often carries IAST or ISO 15919 transliteration, so every merged family maps the Latin
// Extended Additional letters (t, s, n, r, h with a dot below) and the combining dot below (decomposed t + U+0323).
TEST(ShippedTtfFamilies, EachFileMapsTheIastLetters) {
  constexpr uint32_t kIast[] = {0x1E6D, 0x1E63, 0x1E47, 0x1E5B, 0x1E25, 0x0323};
  const std::vector<std::filesystem::path> files = shippedOutlineFiles(".ttf");
  EXPECT_EQ(files.size(), 40u);
  for (const auto& path : files) {
    SCOPED_TRACE(path.filename().generic_string());
    const std::vector<uint8_t> bytes = readAll(path);
    FontBytes fontBytes(bytes.data(), static_cast<uint32_t>(bytes.size()));
    ttf::Face face;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(fontBytes, &face));
    for (const uint32_t cp : kIast) EXPECT_NE(0u, ttf::glyphIndex(fontBytes, face, cp)) << "U+" << std::hex << cp;
  }
}

// --- The Indic families' shaping, through the CPac the install writes ---

namespace {

// A sfnt with the CPac the install writes into a style block (ot::FilterTableWriter, as TtfFont's writeCpac), or with
// none, so that the face computes its filters.
struct ShapedFace {
  std::vector<uint8_t> font;
  std::vector<uint8_t> cpac;
  ot::Face face;

  bool open(const std::filesystem::path& path, const bool withCpac) {
    font = readAll(path);
    ot::FaceTables tables;
    if (font.empty() || !ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) return false;
    if (!face.init(tables)) return false;
    if (!withCpac) return face.filters(ot::GSUB) == nullptr && face.filters(ot::GPOS) == nullptr;
    ot::FilterTableWriter writer(face);
    cpac.resize(writer.bytes());
    if (cpac.empty() || writer.write(cpac.data(), static_cast<uint32_t>(cpac.size())) != cpac.size()) return false;
    tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
    return face.init(tables) && face.filters(ot::GSUB) != nullptr && face.filters(ot::GPOS) != nullptr;
  }
};

// A corpus line: UTF-8, with the format characters (category Cf: ZWJ, ZWNJ) written as a backslash, u and four hex
// digits.
std::vector<uint32_t> decodeCorpusLine(const std::string& line) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; i < line.size();) {
    if (i + 6 <= line.size() && line[i] == '\\' && line[i + 1] == 'u') {
      cps.push_back(static_cast<uint32_t>(std::stoul(line.substr(i + 2, 4), nullptr, 16)));
      i += 6;
      continue;
    }
    const unsigned char* at = reinterpret_cast<const unsigned char*>(line.c_str()) + i;
    const unsigned char* next = at;
    cps.push_back(utf8NextCodepoint(&next));
    i += static_cast<size_t>(next - at);
  }
  return cps;
}

std::vector<std::vector<uint32_t>> corpusSample(const std::string& script) {
  std::string lower = script;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::ifstream in(std::string(INDIC_CORPUS_DIR) + "/indic_corpus_" + lower + ".txt");
  std::vector<std::vector<uint32_t>> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == 0x0D) line.pop_back();  // a CRLF checkout
    if (!line.empty()) lines.push_back(decodeCorpusLine(line));
  }
  return lines;
}

std::vector<int32_t> shapeLine(const ot::Face& face, const ot::Plan& plan, const std::vector<uint32_t>& cps,
                               const int32_t scale, const unsigned ppem, ot::Buffer& buffer) {
  ot::Scale s;
  s.set(scale, ppem, face.upem());
  std::vector<int32_t> out;
  if (!ot::shape(face, s, plan, cps.data(), static_cast<unsigned>(cps.size()), buffer)) return {-1};
  for (unsigned i = 0; i < buffer.len(); i++) {
    out.insert(out.end(), {static_cast<int32_t>(buffer.info[i].codepoint), buffer.pos[i].xAdvance,
                           buffer.pos[i].xOffset, buffer.pos[i].yOffset});
  }
  return out;
}

}  // namespace

// The sample is the first 200 lines of generate_corpus.py's output per script (test/ttf_font/data, cut by
// test/ot_shaper/make_corpus_sample.py): committed, so CI needs no generator and no unicodedata version.
TEST(ShippedFamilies, EachIndicFamilyShapesAlikeThroughItsCpac) {
  static uint8_t storage[ot::Buffer::bytesFor(4096)];
  ot::Buffer buffer;
  buffer.setStorage(storage, sizeof(storage));
  struct Size {
    int32_t scale;
    unsigned ppem;
  };
  const Size sizes[] = {{2133, 33}, {1195, 19}};
  size_t families = 0;
  for (size_t script = 0; script < std::size(indic::SCRIPTS); ++script) {
    const std::string name = indic::SCRIPTS[script].name;
    const std::vector<std::vector<uint32_t>> lines = corpusSample(name);
    ASSERT_EQ(200u, lines.size()) << name;
    for (const char* prefix : {"NotoSans", "NotoSerif"}) {
      const std::string family = prefix + name;
      SCOPED_TRACE(family);
      const std::filesystem::path path = std::filesystem::path(SD_FONTS_DIR) / family / (family + "-Regular.ttf");
      ASSERT_TRUE(std::filesystem::exists(path));
      ShapedFace computed;
      ShapedFace written;
      ASSERT_TRUE(computed.open(path, false));
      ASSERT_TRUE(written.open(path, true));
      ot::Plan computedPlan;
      ot::Plan writtenPlan;
      const uint32_t* languages = ot::languageTagsFor("");
      ASSERT_TRUE(computedPlan.build(computed.face, static_cast<ot::Script>(script), languages));
      ASSERT_TRUE(writtenPlan.build(written.face, static_cast<ot::Script>(script), languages));
      for (const Size& size : sizes) {
        uint32_t different = 0;
        uint32_t glyphs = 0;
        for (const auto& line : lines) {
          const std::vector<int32_t> a = shapeLine(computed.face, computedPlan, line, size.scale, size.ppem, buffer);
          const std::vector<int32_t> b = shapeLine(written.face, writtenPlan, line, size.scale, size.ppem, buffer);
          glyphs += static_cast<uint32_t>(a.size() / 4);
          if (a != b && ++different <= 3) {
            ADD_FAILURE() << "ppem " << size.ppem << ", line starting U+" << std::hex << line.front();
          }
        }
        EXPECT_EQ(0u, different) << "ppem " << size.ppem;
        EXPECT_GT(glyphs, 200u) << "ppem " << size.ppem;  // the lines shaped, they were not all FAIL
      }
      ++families;
    }
  }
  EXPECT_EQ(20u, families);
}
