// The outline fonts under assets/sd-fonts are the bytes assets/sd-fonts/fonts-v3.json describes. CI runs no Python
// tests, so a font re-cut without scripts/generate-font-manifest.py --manifest (the manifest then names a size and
// CRC32 the file no longer has, and every device that downloads it refuses it) is caught here.
#include <FontDownloadRules.h>
#include <FontManifestParser.h>
#include <SerialTransferProtocol.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct ManifestFile {
  std::string name;
  uint32_t size;
  uint32_t crc32;
  bool hasCrc32;
};

void onFile(void* ctx, const FontManifestFile& file) {
  static_cast<std::vector<ManifestFile>*>(ctx)->push_back({file.name, file.size, file.crc32, file.hasCrc32});
}

std::vector<ManifestFile> manifestFiles() {
  std::ifstream in(FONT_MANIFEST_V3_PATH, std::ios::binary);
  const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<ManifestFile> files;
  FontManifestCallbacks callbacks{};
  callbacks.ctx = &files;
  callbacks.onFile = onFile;
  FontManifestParser parser(callbacks);
  parser.feed(json.data(), json.size());
  EXPECT_TRUE(parser.complete());
  EXPECT_FALSE(parser.hasError());
  return files;
}

bool isOutline(const std::string& name) {
  const auto kind = FontDownloadRules::kindOf(name.c_str());
  return kind == FontDownloadRules::FileKind::TrueType || kind == FontDownloadRules::FileKind::OpenTypeCff;
}

}  // namespace

TEST(ShippedFontFilesTest, EveryOutlineFileIsTheSizeAndCrcTheManifestNames) {
  int checked = 0;
  for (const ManifestFile& entry : manifestFiles()) {
    if (!isOutline(entry.name)) continue;
    const std::filesystem::path path = std::filesystem::path(SD_FONTS_DIR) / entry.name;
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << entry.name << " is in the manifest but not under assets/sd-fonts";
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes.size(), entry.size) << entry.name;
    ASSERT_TRUE(entry.hasCrc32) << entry.name;
    EXPECT_EQ(serialtransfer::crc32Update(0, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), entry.crc32)
        << entry.name << " changed without scripts/generate-font-manifest.py --manifest";
    ++checked;
  }
  EXPECT_EQ(checked, 56);  // 8 .otf (CJK) and 48 .ttf (Indic, Arabic and Hebrew)
}
