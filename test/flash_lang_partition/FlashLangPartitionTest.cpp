// FlashLangPartition: a language pack slot proves its own contents before anything reads them.
//
// The slot shares flash with code that does not know it is there: a firmware from before the
// 128 KB slot keeps its own 64 KB slot in this slot's upper half and its font cache below that, so
// it can rewrite sectors here while this slot's header survives. Seen on the X3: back from such a
// firmware, the Chinese pack still matched by its header, and its 12 pt CJK face drew as boxes from
// the bytes that firmware had written over the slot's tail. These tests run the real
// FlashLangPartition over an in-memory partition (esp_partition.h here) laid out like the device's.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "FlashFontPartition.h"
#include "FlashLangPartition.h"
#include "esp_partition.h"

namespace {

namespace flp = FlashLangPartition;

// Two 128 KB halves: the font cache below, the language slot above, as on the device.
constexpr uint32_t PARTITION_BYTES = 256 * 1024;
constexpr size_t SLOT = PARTITION_BYTES - FlashFontPartition::LANG_RESERVED_BYTES;
// The 64 KB slot of a firmware from before the 128 KB one: this slot's upper half.
constexpr size_t OLD_SLOT = PARTITION_BYTES - 64 * 1024;
constexpr uint8_t LANG = 24;  // Chinese
constexpr uint32_t STAMP = 0x5EED1234u;
// What the Chinese pack carries after its strings: its two CJK UI faces, 73,172 B.
constexpr uint32_t EXT_BYTES = 73172;

// A pack shaped like the Chinese one: 875 keys, ~13 KB of strings, every seventh left to English.
struct Pack {
  std::vector<std::string> strings;
  std::vector<uint8_t> blob;  // every string NUL-terminated, the empty ones too: an older firmware's slot data
  uint16_t keyCount = 875;
  int calls = 0;

  Pack() {
    for (uint16_t i = 0; i < keyCount; ++i) {
      strings.push_back(i % 7 == 3 ? std::string() : "key " + std::to_string(i) + " translated");
      blob.insert(blob.end(), strings.back().begin(), strings.back().end());
      blob.push_back(0);
    }
  }

  // FlashLangPartition::Strings::produce: each string the pack has, by key, as I18n writes a card pack's.
  static bool produce(void* ctx, flp::StringsWriter& out) {
    auto* self = static_cast<Pack*>(ctx);
    ++self->calls;
    for (uint16_t i = 0; i < self->keyCount; ++i) {
      const std::string& s = self->strings[i];
      if (s.empty()) continue;
      if (!out.beginKey(i) || !out.write(s.c_str(), s.size() + 1)) return false;
    }
    return true;
  }
  flp::Strings source() { return flp::Strings{static_cast<uint32_t>(blob.size()), &Pack::produce, this}; }
};

uint8_t extByte(const uint32_t i) { return static_cast<uint8_t>(i * 131u + 7u + (i >> 9)); }

// Writes EXT_BYTES of a pattern, in pieces, through the slot's writer -- and checks the scratch the
// slot lends, which CjkUiFonts inflates its faces in.
struct Producer {
  int calls = 0;
  bool scratchOk = false;
  static bool produce(void* ctx, flp::SlotWriter& out) {
    auto* self = static_cast<Producer*>(ctx);
    ++self->calls;
    self->scratchOk = out.scratch() != nullptr && out.scratchBytes() >= 4096;
    uint8_t piece[1000];
    for (uint32_t at = 0; at < EXT_BYTES; at += sizeof(piece)) {
      const uint32_t n = std::min<uint32_t>(sizeof(piece), EXT_BYTES - at);
      for (uint32_t i = 0; i < n; ++i) piece[i] = extByte(at + i);
      if (!out.write(piece, n)) return false;
    }
    return true;
  }
};

class FlashLangPartitionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    flp::unmap();
    fake_flash::reset(PARTITION_BYTES);
  }
  void TearDown() override { flp::unmap(); }

  flp::Extension extension() { return flp::Extension{EXT_BYTES, &Producer::produce, &producer}; }

  bool build() {
    const flp::Extension ext = extension();
    return flp::build(LANG, STAMP, pack.keyCount, pack.source(), &ext);
  }

  bool acquire(flp::Mapped& out) {
    const flp::Extension ext = extension();
    return flp::acquire(LANG, STAMP, pack.keyCount, pack.source(), &ext, &out);
  }

  // Every string as I18n::get() reads it, and the extension byte for byte.
  void expectIntact(const flp::Mapped& m, const bool withExt = true) {
    for (uint16_t i = 0; i < pack.keyCount; ++i) {
      const uint16_t off = m.offsets[i];
      if (pack.strings[i].empty()) {
        ASSERT_EQ(off, flp::FALLBACK) << "key " << i;
      } else {
        ASSERT_EQ(off & flp::FALLBACK, 0u) << "key " << i;
        ASSERT_STREQ(m.data + off, pack.strings[i].c_str()) << "key " << i;
      }
    }
    if (!withExt) {
      EXPECT_EQ(m.ext, nullptr);
      return;
    }
    ASSERT_NE(m.ext, nullptr);
    ASSERT_EQ(m.extLen, EXT_BYTES);
    for (uint32_t i = 0; i < EXT_BYTES; ++i) ASSERT_EQ(m.ext[i], extByte(i)) << "extension byte " << i;
  }

  // Header fields straight off the fake flash, to find where things are.
  uint32_t headerWord(const size_t at) {
    uint32_t v;
    memcpy(&v, fake_flash::chip().bytes.data() + SLOT + at, 4);
    return v;
  }
  // magic, version, language, key count, stamp, then: table, data, data length, extension offset.
  size_t tableOff() { return headerWord(12); }
  size_t dataOff() { return headerWord(16); }
  size_t extOff() { return headerWord(24); }
  void flipBit(const size_t at) { fake_flash::chip().bytes[at] ^= 0x10; }

  Pack pack;
  Producer producer;
};

TEST_F(FlashLangPartitionTest, ABuiltSlotMapsVerified) {
  ASSERT_TRUE(build());
  EXPECT_TRUE(producer.scratchOk);
  flp::Mapped m{};
  ASSERT_EQ(flp::map(LANG, STAMP, pack.keyCount, &m), flp::MapResult::Ok);
  expectIntact(m);
}

TEST_F(FlashLangPartitionTest, AnIntactSlotIsNotRebuilt) {
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m));
  const int erases = fake_flash::chip().erases;
  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(fake_flash::chip().erases, erases);
  EXPECT_EQ(producer.calls, 1);
  expectIntact(m);
}

TEST_F(FlashLangPartitionTest, ACorruptExtensionByteIsWithheldAndRebuilt) {
  ASSERT_TRUE(build());
  flipBit(SLOT + extOff() + EXT_BYTES - 100);
  flp::Mapped m{};
  ASSERT_EQ(flp::map(LANG, STAMP, pack.keyCount, &m), flp::MapResult::ExtensionCorrupt);
  EXPECT_EQ(m.ext, nullptr) << "a failed extension must not be handed out";
  expectIntact(m, /*withExt=*/false);

  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(producer.calls, 2) << "rebuilt once, in the same call";
  expectIntact(m);
}

TEST_F(FlashLangPartitionTest, ACorruptStringByteFailsTheMapAndIsRebuilt) {
  ASSERT_TRUE(build());
  flipBit(SLOT + dataOff() + 40);
  flp::Mapped m{};
  EXPECT_EQ(flp::map(LANG, STAMP, pack.keyCount, &m), flp::MapResult::Corrupt);
  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(producer.calls, 2);
  expectIntact(m);
}

TEST_F(FlashLangPartitionTest, ACorruptTableEntryFailsTheMapAndIsRebuilt) {
  // A wrong offset would send a string read anywhere, past the mapping included.
  ASSERT_TRUE(build());
  flipBit(SLOT + tableOff() + 2 * 100 + 1);
  flp::Mapped m{};
  EXPECT_EQ(flp::map(LANG, STAMP, pack.keyCount, &m), flp::MapResult::Corrupt);
  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(producer.calls, 2);
  expectIntact(m);
}

TEST_F(FlashLangPartitionTest, AnOlderFirmwaresPackInTheUpperHalfIsFoundAndRebuilt) {
  // The X3 repro: this firmware builds Chinese, an older one boots and rebuilds ITS slot -- erasing and
  // rewriting the upper 64 KB -- and this firmware comes back to a header that still matches.
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m));
  ASSERT_GT(SLOT + extOff() + EXT_BYTES, OLD_SLOT) << "the extension must reach into the old slot";
  flp::unmap();

  fake_flash::Chip& chip = fake_flash::chip();
  ASSERT_EQ(esp_partition_erase_range(&chip.part, OLD_SLOT, 64 * 1024), ESP_OK);
  // The old firmware's 32 B format-1 header, its table and its strings, at the old slot.
  std::vector<uint8_t> old(32, 0);
  memcpy(old.data(), "CPLP", 4);
  old[4] = 1;
  old[5] = LANG;
  memcpy(old.data() + 6, &pack.keyCount, 2);
  const uint32_t oldStamp = 0x0B5011E5u, tableAt = 32, tableBytes = (pack.keyCount * 2u + 3u) & ~3u,
                 dataAt = tableAt + tableBytes, dataLen = static_cast<uint32_t>(pack.blob.size());
  memcpy(old.data() + 8, &oldStamp, 4);
  memcpy(old.data() + 12, &tableAt, 4);
  memcpy(old.data() + 16, &dataAt, 4);
  memcpy(old.data() + 20, &dataLen, 4);
  old.resize(dataAt, 0x5A);  // the table: anything but this firmware's
  old.insert(old.end(), pack.blob.begin(), pack.blob.end());
  ASSERT_EQ(esp_partition_write(&chip.part, OLD_SLOT, old.data(), old.size()), ESP_OK);

  // The header 128 KB from the end survived and still says Chinese with these faces...
  EXPECT_TRUE(flp::holds(LANG, STAMP, EXT_BYTES));
  // ...but its extension is not what it was written with any more.
  EXPECT_EQ(flp::map(LANG, STAMP, pack.keyCount, &m), flp::MapResult::ExtensionCorrupt);

  const int erases = chip.erases;
  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(chip.erases, erases + 1) << "rebuilt once, in the same boot";
  expectIntact(m);
}

TEST_F(FlashLangPartitionTest, ASlotOfAnEarlierFormatIsRebuilt) {
  // Format 2 held one string per StrId of the firmware that wrote it and named English by an offset:
  // read as this format, its table would send keys to the wrong strings.
  ASSERT_TRUE(build());
  fake_flash::Chip& chip = fake_flash::chip();
  chip.bytes[SLOT + 4] = 2;  // clearing bit 0 of the version, as a format-2 header holds it
  EXPECT_FALSE(flp::holds(LANG, STAMP, EXT_BYTES));
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(producer.calls, 2);
  EXPECT_EQ(pack.calls, 2);
  expectIntact(m);
}

TEST_F(FlashLangPartitionTest, StringsPastTheOffsetLimitOrAnUnknownKeyFailTheBuild) {
  // Bit 15 is FALLBACK: a string may not start at 32 KB or later, and a key past the count has no entry.
  struct Long {
    uint16_t key;
    static bool produce(void* ctx, flp::StringsWriter& out) {
      const auto* self = static_cast<const Long*>(ctx);
      std::string s(1000, 'x');
      for (int i = 0; i < 40; ++i) {
        if (!out.beginKey(self->key) || !out.write(s.c_str(), s.size() + 1)) return false;
      }
      return true;
    }
  };
  Long tooLong{1};
  EXPECT_FALSE(flp::build(LANG, STAMP, pack.keyCount, flp::Strings{40 * 1001, &Long::produce, &tooLong}));
  Long unknown{pack.keyCount};
  EXPECT_FALSE(flp::build(LANG, STAMP, pack.keyCount, flp::Strings{40 * 1001, &Long::produce, &unknown}));
  EXPECT_FALSE(flp::holds(LANG, STAMP, 0)) << "a failed build leaves no header";
}

TEST_F(FlashLangPartitionTest, StringsThatNeverWriteRightFailOnceWithoutALoop) {
  // Every write after the header's sector programs wrong while reporting success: the rebuild never
  // verifies. One build per call, then English -- and the next call builds once more, not forever.
  fake_flash::chip().corruptWritesFrom = SLOT + 1;
  flp::Mapped m{};
  EXPECT_FALSE(acquire(m));
  EXPECT_EQ(fake_flash::chip().erases, 1);
  EXPECT_FALSE(acquire(m));
  EXPECT_EQ(fake_flash::chip().erases, 2);
}

TEST_F(FlashLangPartitionTest, AnExtensionThatNeverWritesRightLeavesTheStringsStanding) {
  ASSERT_TRUE(build());
  fake_flash::chip().corruptWritesFrom = SLOT + extOff();
  flipBit(SLOT + extOff() + 5);  // the slot as found: its extension corrupt
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m)) << "the strings verify, so the language stands";
  EXPECT_EQ(producer.calls, 2) << "one rebuild";
  expectIntact(m, /*withExt=*/false);
}

TEST_F(FlashLangPartitionTest, AnExtensionTheMmuCannotMapIsNotRebuiltFor) {
  // Chinese spans two 64 KB pages. With only one to spare the strings map alone, and rebuilding would
  // not find another page.
  flp::Mapped m{};
  ASSERT_TRUE(acquire(m));
  flp::unmap();
  fake_flash::chip().mapLimit = 64 * 1024;
  const int erases = fake_flash::chip().erases;
  EXPECT_EQ(flp::map(LANG, STAMP, pack.keyCount, &m), flp::MapResult::WithoutExtension);
  ASSERT_TRUE(acquire(m));
  EXPECT_EQ(fake_flash::chip().erases, erases);
  expectIntact(m, /*withExt=*/false);
}

TEST_F(FlashLangPartitionTest, APackWithoutAnExtensionStillMapsOnePage) {
  fake_flash::chip().mapLimit = 64 * 1024;
  flp::Mapped m{};
  ASSERT_TRUE(flp::acquire(LANG, STAMP, pack.keyCount, pack.source(), nullptr, &m));
  expectIntact(m, /*withExt=*/false);
}

}  // namespace
