// The TTF glyph store and the hyphenation store over a simulated flash area, each through the FlashAreaRegion the
// firmware gives it (SdCardFontManager, HyphenationPacks): the window puts their bytes where the area's layout says,
// and leaves the rest of the area alone.

#include <esp_rom_crc.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "Epub/hyphenation/HyphenationPack.h"
#include "Epub/hyphenation/HyphenationStore.h"
#include "FlashAreaRegion.h"
#include "FlashFontPartition.h"
#include "GlyphStoreTestUtil.h"
#include "SimFlashArea.h"
#include "SimNorFlash.h"

namespace {

namespace ffp = FlashFontPartition;

constexpr uint32_t kAreaBytes = 1024 * 1024;  // 512 KB of font area, the 384 KB region, the 128 KB slot
constexpr uint32_t kFontArea = kAreaBytes - ffp::LANG_RESERVED_BYTES - ffp::HYPH_RESERVED_BYTES;

// A pack of `code` whose payload is `bytes` of a pattern: the store copies and checks bytes, it does not read tries.
std::vector<uint8_t> pack(const char* code, const uint32_t bytes) {
  std::vector<uint8_t> payload(bytes);
  for (uint32_t i = 0; i < bytes; ++i) payload[i] = static_cast<uint8_t>(i * 17u + 1u);
  hyphpack::Header h;
  h.code[0] = code[0];
  h.code[1] = code[1];
  h.minPrefix = 2;
  h.minSuffix = 2;
  h.rootOffset = 0;
  h.payloadSize = bytes;
  h.crc32 = esp_rom_crc32_le(0, payload.data(), bytes);
  std::vector<uint8_t> out(hyphpack::kHeaderBytes);
  hyphpack::writeHeader(h, out.data());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

class BufferSource final : public HyphenationPackSource {
 public:
  explicit BufferSource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }
  bool read(const uint32_t offset, void* dst, const uint32_t len) override {
    if (offset > bytes_.size() || len > bytes_.size() - offset) return false;
    std::memcpy(dst, bytes_.data() + offset, len);
    return true;
  }

 private:
  std::vector<uint8_t> bytes_;
};

bool erased(const uint8_t* bytes, const uint32_t from, const uint32_t to) {
  for (uint32_t i = from; i < to; ++i) {
    if (bytes[i] != 0xFF) return false;
  }
  return true;
}

class FlashAreaStoresTest : public ::testing::Test {
 protected:
  void SetUp() override { sim_area::use(&area); }
  void TearDown() override {
    EXPECT_EQ(0u, area.mappings()) << "every mapping a test made is released";
    EXPECT_EQ(0u, area.violations()) << "nothing programmed a 0 back to 1";
    sim_area::use(nullptr);
  }

  SimNorFlash area{kAreaBytes};
};

}  // namespace

TEST_F(FlashAreaStoresTest, TheGlyphStoreLivesInTheFontAreaFromTheAreasStart) {
  const glyphstore::Layout layout = gst_test::smallLayout();
  {
    FlashAreaRegion window;
    ASSERT_TRUE(window.begin());
    ASSERT_EQ(kFontArea, window.size());
    FlashGlyphStore store(window);
    ASSERT_TRUE(gst_test::installAndOpen(store, layout));
    EXPECT_EQ(1u, FlashAreaRegion::liveFontAreaMappings()) << "the store's index and log, mapped";

    uint8_t buffer[2048];
    GlyphBatch batch(buffer, sizeof buffer);
    const uint32_t key = glyphstore::makeKey(0x4E2D, 0, 0);
    ASSERT_TRUE(gst_test::stage(batch, key, 20, 24));
    ASSERT_EQ(gst_test::Commit::Stored, store.commit(batch));
    EXPECT_TRUE(gst_test::storedIntact(store, key, 20, 24));
    const uint8_t* const log = store.logBase();
    EXPECT_EQ(area.raw() + layout.geometry.logOffset, log) << "the log, read in place at its offset in the area";

    // The style's bytes where the layout put them, from the area's start.
    for (uint32_t i = 0; i < layout.styleSize[0]; ++i) {
      ASSERT_EQ(gst_test::styleByte(0, i), area.raw()[layout.styleOffset[0] + i]) << i;
    }
  }
  EXPECT_EQ(0u, FlashAreaRegion::liveFontAreaMappings());
  EXPECT_TRUE(erased(area.raw(), kFontArea, kAreaBytes)) << "nothing above the font area was touched";
}

TEST_F(FlashAreaStoresTest, TheHyphenationStoreLivesInItsRegion) {
  const uint32_t base = static_cast<uint32_t>(ffp::hyphRegionOffset());
  ASSERT_EQ(kFontArea, base);
  {
    FlashAreaRegion window;
    ASSERT_TRUE(window.beginRegion(base, ffp::HYPH_RESERVED_BYTES));
    HyphenationStore store(window);
    ASSERT_TRUE(store.mount());
    BufferSource src(pack("de", 100000));
    hyphpack::Header h;
    ASSERT_TRUE(readPackHeader(src, h));
    uint8_t scratch[1024];
    ASSERT_EQ(HyphenationStore::Fill::Stored, store.fill(src, h, scratch, sizeof scratch, true));

    SerializedHyphenationPatterns mapped{};
    ASSERT_TRUE(store.map("de", mapped));
    EXPECT_EQ(area.raw() + base, mapped.data) << "the payload at the region's start, read in place";
    EXPECT_EQ(100000u, mapped.size);
    EXPECT_EQ(0u, FlashAreaRegion::liveFontAreaMappings()) << "a trie is no reason to keep the .cpfont cache off";
    store.unmapAll();
  }
  EXPECT_TRUE(erased(area.raw(), 0, base)) << "the font area was not touched";
  EXPECT_TRUE(erased(area.raw(), base + ffp::HYPH_RESERVED_BYTES, kAreaBytes)) << "nor the language slot";
}

TEST_F(FlashAreaStoresTest, AWindowRefusesWhatLiesOutsideIt) {
  FlashAreaRegion window;
  ASSERT_TRUE(window.beginRegion(kFontArea, ffp::HYPH_RESERVED_BYTES));
  const uint8_t byte = 0;
  EXPECT_FALSE(window.program(ffp::HYPH_RESERVED_BYTES, &byte, 1));
  EXPECT_FALSE(window.erase(ffp::HYPH_RESERVED_BYTES, NorFlash::kSectorBytes));
  EXPECT_EQ(nullptr, window.map(ffp::HYPH_RESERVED_BYTES - 16, 32));
  EXPECT_FALSE(window.beginRegion(kFontArea + 4096, 4096)) << "a region starts on a mapping page";
  EXPECT_FALSE(window.beginRegion(kFontArea, kAreaBytes)) << "nor reaches past the area";
  EXPECT_TRUE(erased(area.raw(), 0, kAreaBytes));
}
