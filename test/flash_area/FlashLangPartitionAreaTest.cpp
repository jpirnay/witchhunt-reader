// FlashLangPartition, the UI language slot, over a simulated flash area instead of the `spiffs` partition: it builds,
// verifies and maps the slot at the area's end through FlashArea::get(), whatever backend that is, and leaves the
// .cpfont cache below it alone as the cache leaves the slot alone.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "FlashFontPartition.h"
#include "FlashLangPartition.h"
#include "SimFlashArea.h"
#include "SimNorFlash.h"

namespace {

namespace flp = FlashLangPartition;

constexpr uint32_t kAreaBytes = 1024 * 1024;
constexpr uint32_t kSlot = kAreaBytes - FlashFontPartition::LANG_RESERVED_BYTES;
constexpr uint8_t kLang = 3;
constexpr uint32_t kStamp = 0xC0FFEE01u;
constexpr uint16_t kKeys = 300;
constexpr uint32_t kExtBytes = 70000;  // past the slot's first page, as a CJK language's UI faces are

std::string text(const uint16_t key) { return key % 5 == 2 ? std::string() : "string " + std::to_string(key); }
uint8_t extByte(const uint32_t i) { return static_cast<uint8_t>(i * 13u + 5u); }

struct Producers {
  int strings = 0;
  int extensions = 0;

  static bool produceStrings(void* ctx, flp::StringsWriter& out) {
    ++static_cast<Producers*>(ctx)->strings;
    for (uint16_t key = 0; key < kKeys; ++key) {
      const std::string s = text(key);
      if (s.empty()) continue;
      if (!out.beginKey(key) || !out.write(s.c_str(), s.size() + 1)) return false;
    }
    return true;
  }
  static bool produceExtension(void* ctx, flp::SlotWriter& out) {
    ++static_cast<Producers*>(ctx)->extensions;
    uint8_t piece[700];
    for (uint32_t at = 0; at < kExtBytes; at += sizeof piece) {
      const uint32_t n = std::min<uint32_t>(sizeof piece, kExtBytes - at);
      for (uint32_t i = 0; i < n; ++i) piece[i] = extByte(at + i);
      if (!out.write(piece, n)) return false;
    }
    return true;
  }
};

class FlashLangPartitionAreaTest : public ::testing::Test {
 protected:
  void SetUp() override { sim_area::use(&area); }
  void TearDown() override {
    flp::unmap();
    EXPECT_EQ(0u, area.mappings()) << "every mapping a test made is released";
    EXPECT_EQ(0u, area.violations()) << "nothing programmed a 0 back to 1";
    sim_area::use(nullptr);
  }

  bool acquire(flp::Mapped& out) {
    const flp::Strings strings{8192, &Producers::produceStrings, &producers};
    const flp::Extension ext{kExtBytes, &Producers::produceExtension, &producers};
    return flp::acquire(kLang, kStamp, kKeys, strings, &ext, &out);
  }

  void expectIntact(const flp::Mapped& m) {
    for (uint16_t key = 0; key < kKeys; ++key) {
      const std::string s = text(key);
      if (s.empty()) {
        ASSERT_EQ(flp::FALLBACK, m.offsets[key]) << "key " << key;
      } else {
        ASSERT_STREQ(s.c_str(), m.data + m.offsets[key]) << "key " << key;
      }
    }
    ASSERT_NE(nullptr, m.ext);
    ASSERT_EQ(kExtBytes, m.extLen);
    for (uint32_t i = 0; i < kExtBytes; ++i) ASSERT_EQ(extByte(i), m.ext[i]) << "extension byte " << i;
  }

  SimNorFlash area{kAreaBytes};
  Producers producers;
};

}  // namespace

TEST_F(FlashLangPartitionAreaTest, TheSlotIsBuiltAtTheAreasEndAndMappedInPlace) {
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m));
  expectIntact(m);
  EXPECT_EQ(area.raw() + kSlot + flp::HEADER_BYTES, reinterpret_cast<const uint8_t*>(m.offsets));
  EXPECT_EQ(0, std::memcmp(area.raw() + kSlot, "CPLP", 4));
  EXPECT_EQ(1u, area.mappings()) << "one mapping for the strings and the extension";
  for (uint32_t i = 0; i < kSlot; ++i) ASSERT_EQ(0xFF, area.raw()[i]) << "below the slot, at " << i;

  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(1, producers.strings) << "a slot that holds the pack is not built again";
  EXPECT_TRUE(flp::holds(kLang, kStamp, kExtBytes));
}

TEST_F(FlashLangPartitionAreaTest, BytesChangedBehindTheHeaderAreRebuiltOnce) {
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m));
  flp::unmap();
  area.raw()[kSlot + flp::HEADER_BYTES + 2 * kKeys + 7] ^= 0x10;  // a string byte
  EXPECT_EQ(flp::MapResult::Corrupt, flp::map(kLang, kStamp, kKeys, &m));
  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(2, producers.strings);
  expectIntact(m);
}

TEST_F(FlashLangPartitionAreaTest, AnAreaWithNoMappingLeftServesNoStrings) {
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m));
  flp::unmap();
  area.setFreePages(0);
  EXPECT_FALSE(acquire(m));
  EXPECT_EQ(1, producers.strings) << "a rebuild cannot give the strings a page";
}

TEST_F(FlashLangPartitionAreaTest, TheCpfontCacheAndTheSlotLeaveEachOtherAlone) {
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m));
  flp::unmap();
  ASSERT_TRUE(FlashFontPartition::beginWrite("Bitter"));
  EXPECT_FALSE(FlashFontPartition::finaliseWrite());
  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(1, producers.strings) << "the cache's erase stopped below the slot";
  expectIntact(m);
}

TEST_F(FlashLangPartitionAreaTest, WithoutAnAreaTheUiStaysEnglish) {
  sim_area::use(nullptr);
  flp::Mapped m{};
  EXPECT_FALSE(acquire(m));
  EXPECT_FALSE(flp::holds(kLang, kStamp, kExtBytes));
  EXPECT_EQ(0, producers.strings);
}
