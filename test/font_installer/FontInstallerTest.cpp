// FontInstaller over an in-memory card (test/file_replace/HalStorage.h): whether a family on the card has an update, a
// pack's files compared by their CRC as well as their size and a font's by their size alone, and the CRC of a file as
// computeFileCrc32() reads it, 1 KB at a time.

#include <FontInstaller.h>
#include <HalStorage.h>
#include <esp_rom_crc.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "HyphenationPacks.h"

// What FontInstaller.cpp links against and these tests never reach: the setting it clears when the reader's family is
// deleted, the registry it refreshes, and HyphenationPacks, which it tells of a pack installed or removed.
CrossPointSettings CrossPointSettings::instance;
bool CrossPointSettings::saveToFile() const { return true; }
bool SdCardFontRegistry::discover() { return false; }
const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string&) const { return nullptr; }
namespace HyphenationPacks {
bool codeOfPackFile(const char*, char (&)[3]) { return false; }
void packInstalled(const char*) {}
void packRemoved(const char*) {}
bool packOnCard(const char*) { return false; }
}  // namespace HyphenationPacks

namespace {

enum class Kind { Font, Hyphenation, Language };

struct FileSpec {
  std::string name;  // as the manifest gives it: "<family>/<file>"
  uint32_t size = 0;
  uint32_t crc32 = 0;
  bool hasCrc32 = false;
};

void feed(FontCatalog::Builder& builder, const std::string& family, const Kind kind,
          const std::vector<FileSpec>& files) {
  builder.beginFamily();
  builder.hyphenation(kind == Kind::Hyphenation);
  builder.languagePack(kind == Kind::Language);
  for (const auto& file : files)
    builder.addFile(file.name.data(), file.name.size(), file.size, file.crc32, file.hasCrc32);
  builder.keepFamily(family.data(), family.size());
}

// A catalog of the one family, built in the manifest reader's two passes.
void build(FontCatalog& catalog, const std::string& family, const Kind kind, const std::vector<FileSpec>& files) {
  FontCatalog::Builder counter;
  feed(counter, family, kind, files);
  ASSERT_TRUE(catalog.allocate(counter.size()));
  FontCatalog::Builder filler(catalog);
  feed(filler, family, kind, files);
  ASSERT_FALSE(filler.overflowed());
  ASSERT_EQ(catalog.count(), 1u);
}

uint32_t crcOf(const std::string& bytes) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size()));
}

// The CRC as computeFileCrc32() read it before, 128 bytes at a time, chained.
uint32_t crcIn128ByteReads(const std::string& bytes) {
  uint32_t crc = 0;
  for (size_t at = 0; at < bytes.size(); at += 128) {
    const size_t n = std::min<size_t>(128, bytes.size() - at);
    crc = esp_rom_crc32_le(crc, reinterpret_cast<const uint8_t*>(bytes.data() + at), static_cast<uint32_t>(n));
  }
  return crc;
}

// `n` bytes that differ from offset to offset and from seed to seed.
std::string bytesOf(const size_t n, uint32_t seed) {
  std::string bytes(n, '\0');
  for (auto& c : bytes) {
    seed = seed * 1664525u + 1013904223u;
    c = static_cast<char>(seed >> 24);
  }
  return bytes;
}

std::string readHostFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class FontInstallerTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

// A pack rebuilt at the size it had: only its CRC tells.
TEST_F(FontInstallerTest, APacksFileAtItsSizeWithAnotherCrcIsAnUpdate) {
  const std::string onCard = bytesOf(4000, 1);
  Storage.put("/.crosspoint/hyphenation/hyph-de.cphyph", onCard);
  FontCatalog catalog;
  build(catalog, "Hyphenation-German", Kind::Hyphenation,
        {{"Hyphenation-German/hyph-de.cphyph", 4000, crcOf(onCard) ^ 1u, true}});
  EXPECT_TRUE(FontInstaller::hasUpdate(catalog, 0));
}

TEST_F(FontInstallerTest, APacksFileAsTheManifestListsItIsNoUpdate) {
  const std::string onCard = bytesOf(4000, 1);
  Storage.put("/.crosspoint/hyphenation/hyph-de.cphyph", onCard);
  FontCatalog catalog;
  build(catalog, "Hyphenation-German", Kind::Hyphenation,
        {{"Hyphenation-German/hyph-de.cphyph", 4000, crcOf(onCard), true}});
  EXPECT_FALSE(FontInstaller::hasUpdate(catalog, 0));
}

TEST_F(FontInstallerTest, ALanguagePacksFileIsComparedByItsCrcToo) {
  const std::string onCard = bytesOf(11530, 2);
  Storage.put("/.crosspoint/lang/de.cplang", onCard);
  FontCatalog changed;
  build(changed, "Language-German", Kind::Language, {{"Language-German/de.cplang", 11530, crcOf(onCard) + 1u, true}});
  EXPECT_TRUE(FontInstaller::hasUpdate(changed, 0));
  FontCatalog same;
  build(same, "Language-German", Kind::Language, {{"Language-German/de.cplang", 11530, crcOf(onCard), true}});
  EXPECT_FALSE(FontInstaller::hasUpdate(same, 0));
}

// A manifest without CRCs (version 1) leaves the size to decide, as before.
TEST_F(FontInstallerTest, APacksFileWithoutAListedCrcGoesByItsSize) {
  Storage.put("/.crosspoint/lang/de.cplang", bytesOf(500, 3));
  FontCatalog catalog;
  build(catalog, "Language-German", Kind::Language, {{"Language-German/de.cplang", 500, 0, false}});
  HalFile::reads = 0;
  EXPECT_FALSE(FontInstaller::hasUpdate(catalog, 0));
  EXPECT_EQ(HalFile::reads, 0u);
}

// As a language pack converted from a 2.40 slot is: stored uncompressed, larger than the manifest's.
TEST_F(FontInstallerTest, APacksFileAtAnotherSizeIsAnUpdate) {
  const std::string onCard = bytesOf(30000, 4);
  Storage.put("/.crosspoint/lang/de.cplang", onCard);
  FontCatalog catalog;
  build(catalog, "Language-German", Kind::Language, {{"Language-German/de.cplang", 11530, crcOf(onCard), true}});
  EXPECT_TRUE(FontInstaller::hasUpdate(catalog, 0));
}

// A font's files can run to megabytes: at the manifest's size they are not read, whatever their CRC.
TEST_F(FontInstallerTest, AFontsFileAtItsSizeIsNoUpdateWhateverItsCrc) {
  const std::string onCard = bytesOf(4000, 5);
  Storage.put("/.crosspoint/fonts/Alpha/Alpha_10.cpfont", onCard);
  FontCatalog catalog;
  build(catalog, "Alpha", Kind::Font, {{"Alpha/Alpha_10.cpfont", 4000, crcOf(onCard) ^ 1u, true}});
  HalFile::reads = 0;
  EXPECT_FALSE(FontInstaller::hasUpdate(catalog, 0));
  EXPECT_EQ(HalFile::reads, 0u);
}

TEST_F(FontInstallerTest, AFontsFileAtAnotherSizeIsAnUpdate) {
  const std::string onCard = bytesOf(4000, 6);
  Storage.put("/.crosspoint/fonts/Alpha/Alpha_10.cpfont", onCard);
  FontCatalog catalog;
  build(catalog, "Alpha", Kind::Font, {{"Alpha/Alpha_10.cpfont", 4001, crcOf(onCard), true}});
  EXPECT_TRUE(FontInstaller::hasUpdate(catalog, 0));
}

// The pieces the CRC is read in change nothing of what it is: the result the 128-byte reads gave, at every size around
// a piece's edges, in one read call per KB begun.
TEST_F(FontInstallerTest, TheCrcReadIn1KbPiecesIsTheOneReadIn128BytePieces) {
  for (const size_t n : {0u, 1u, 127u, 128u, 129u, 1023u, 1024u, 1025u, 4096u, 70001u}) {
    const std::string bytes = bytesOf(n, static_cast<uint32_t>(n) + 7u);
    Storage.put("/file.bin", bytes);
    HalFile::reads = 0;
    uint32_t crc = 0;
    ASSERT_TRUE(FontInstaller::computeFileCrc32("/file.bin", crc)) << n;
    EXPECT_EQ(crc, crcIn128ByteReads(bytes)) << n;
    EXPECT_EQ(crc, crcOf(bytes)) << n;
    EXPECT_EQ(HalFile::reads, (n + 1023) / 1024) << n;
  }
}

TEST_F(FontInstallerTest, TheShippedPacksHaveTheCrcThe128ByteReadsGave) {
  for (const char* file :
       {"Language-German/de.cplang", "Hyphenation-German/hyph-de.cphyph", "Hyphenation-Hindi/hyph-hi.cphyph"}) {
    const std::string bytes = readHostFile(std::string(SD_FONTS_DIR) + "/" + file);
    ASSERT_FALSE(bytes.empty()) << file;
    Storage.put("/pack", bytes);
    uint32_t crc = 0;
    ASSERT_TRUE(FontInstaller::computeFileCrc32("/pack", crc)) << file;
    EXPECT_EQ(crc, crcIn128ByteReads(bytes)) << file;
  }
}

TEST_F(FontInstallerTest, AMissingFileHasNoCrc) {
  uint32_t crc = 0;
  EXPECT_FALSE(FontInstaller::computeFileCrc32("/missing.bin", crc));
}

}  // namespace
