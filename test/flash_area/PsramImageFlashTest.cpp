// PsramImageFlash: the flash area held in PSRAM and kept as a file on the card, for a board without a `spiffs`
// partition. A std::vector stands in for the PSRAM, SimImageFile for the card. What these tests hold it to:
// - NOR semantics, so every consumer's write-once and torn-write rules hold as on flash;
// - a reboot finds what was synced, and reads only the pages something touches;
// - a power cut anywhere, inside a sync included, leaves the last synced image, the new one, or an erased one: never a
//   mix of two.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "FlashAreaRegion.h"
#include "FlashLangPartition.h"
#include "GlyphStoreTestUtil.h"
#include "PsramImageFlash.h"
#include "SimFlashArea.h"
#include "SimImageFile.h"

namespace {

constexpr uint32_t kPage = NorFlash::kMapPageBytes;
constexpr uint32_t kSector = NorFlash::kSectorBytes;
constexpr uint32_t kImageBytes = 4 * kPage;
constexpr uint32_t kFileBytes = PsramImageFlash::kHeaderBlockBytes + kImageBytes;

// A boot: fresh PSRAM (not zeroed: nothing may rely on what the allocator left) over the card as it is.
struct Boot {
  explicit Boot(SimImageFile& file, const uint32_t bytes = kImageBytes)
      : psram(bytes, 0xA5), area(psram.data(), bytes, file) {}
  std::vector<uint8_t> psram;
  PsramImageFlash area;
};

std::vector<uint8_t> contents(PsramImageFlash& area) {
  std::vector<uint8_t> out(area.size());
  EXPECT_TRUE(area.read(0, out.data(), area.size()));
  return out;
}

bool allErased(const std::vector<uint8_t>& bytes) {
  for (const uint8_t b : bytes) {
    if (b != 0xFF) return false;
  }
  return true;
}

// Programs `len` bytes of a pattern seeded by `seed` at `offset`.
void fill(PsramImageFlash& area, const uint32_t offset, const uint32_t len, const uint8_t seed) {
  std::vector<uint8_t> bytes(len);
  for (uint32_t i = 0; i < len; ++i) bytes[i] = static_cast<uint8_t>(seed + i * 7u);
  ASSERT_TRUE(area.program(offset, bytes.data(), len));
}

}  // namespace

TEST(PsramImageFlash, KeepsNorRules) {
  SimImageFile file;
  Boot boot(file);
  PsramImageFlash& area = boot.area;
  ASSERT_TRUE(area.begin());
  EXPECT_EQ(kImageBytes, area.size());
  EXPECT_EQ(4u, area.freeMapPages()) << "a mapping costs no MMU page";

  const uint8_t a = 0xF0, b = 0x0F;
  ASSERT_TRUE(area.program(100, &a, 1));
  ASSERT_TRUE(area.program(100, &b, 1));
  uint8_t got = 0;
  ASSERT_TRUE(area.read(100, &got, 1));
  EXPECT_EQ(0x00, got) << "a program only clears bits";

  EXPECT_FALSE(area.erase(100, kSector)) << "an erase starts on a sector";
  EXPECT_FALSE(area.erase(0, 100)) << "and covers whole ones";
  ASSERT_TRUE(area.erase(0, kSector));
  ASSERT_TRUE(area.read(100, &got, 1));
  EXPECT_EQ(0xFF, got);

  EXPECT_FALSE(area.program(kImageBytes - 1, &a, 2)) << "nothing past the end";
  EXPECT_FALSE(area.read(kImageBytes, &got, 1));
  EXPECT_EQ(nullptr, area.map(kImageBytes - 16, 32));
  EXPECT_EQ(nullptr, area.map(0, 0));

  const uint8_t* const mapped = area.map(kPage + 8, 64);
  ASSERT_EQ(boot.psram.data() + kPage + 8, mapped) << "a mapping is the image itself";
  fill(area, kPage + 8, 4, 0x11);
  EXPECT_EQ(0x11, mapped[0]) << "and sees later programs";
  area.unmap(mapped);
}

TEST(PsramImageFlash, AFreshImageIsErasedAndItsFirstSyncWritesTheWholeFile) {
  SimImageFile file;
  {
    Boot boot(file);
    ASSERT_TRUE(boot.area.begin());
    EXPECT_TRUE(allErased(contents(boot.area)));
    fill(boot.area, 3 * kPage + 100, 5000, 0x21);
    ASSERT_TRUE(boot.area.sync());
    EXPECT_EQ(kFileBytes, file.card().size());
    const uint64_t written = file.bytesWritten();
    ASSERT_TRUE(boot.area.sync());
    EXPECT_EQ(written, file.bytesWritten()) << "nothing changed, nothing written";
  }
  file.powerCycle();
  Boot again(file);
  ASSERT_TRUE(again.area.begin());
  uint8_t got[5000];
  ASSERT_TRUE(again.area.read(3 * kPage + 100, got, sizeof got));
  for (uint32_t i = 0; i < sizeof got; ++i) ASSERT_EQ(static_cast<uint8_t>(0x21 + i * 7u), got[i]) << i;
}

TEST(PsramImageFlash, ALaterSyncWritesOnlyTheSectorsThatChanged) {
  SimImageFile file;
  Boot boot(file);
  ASSERT_TRUE(boot.area.begin());
  ASSERT_TRUE(boot.area.sync());
  const uint64_t before = file.bytesWritten();
  fill(boot.area, kPage + 10, 20, 0x31);              // one sector
  fill(boot.area, 2 * kPage + kSector - 4, 8, 0x41);  // across two
  ASSERT_TRUE(boot.area.sync());
  // Three sectors and the header twice (dirty, then clean).
  EXPECT_EQ(3u * kSector + 2u * 16u, file.bytesWritten() - before);
}

TEST(PsramImageFlash, ARebootReadsOnlyThePagesSomethingTouches) {
  SimImageFile file;
  {
    Boot boot(file);
    ASSERT_TRUE(boot.area.begin());
    fill(boot.area, 0, 64, 0x51);
    fill(boot.area, 2 * kPage, 64, 0x61);
    ASSERT_TRUE(boot.area.sync());
  }
  file.powerCycle();
  Boot again(file);
  ASSERT_TRUE(again.area.begin());
  const uint32_t afterHeader = file.reads();
  const uint8_t* const page2 = again.area.map(2 * kPage, 64);
  ASSERT_NE(nullptr, page2);
  EXPECT_EQ(0x61, page2[0]);
  EXPECT_EQ(afterHeader + 1, file.reads()) << "one page read for the mapping";
  uint8_t byte = 0;
  ASSERT_TRUE(again.area.read(2 * kPage + 10, &byte, 1));
  EXPECT_EQ(afterHeader + 1, file.reads()) << "a loaded page is not read again";
  ASSERT_TRUE(again.area.erase(3 * kPage, kPage));
  EXPECT_EQ(afterHeader + 1, file.reads()) << "a page erased whole is not read either";
  ASSERT_TRUE(again.area.read(0, &byte, 1));
  EXPECT_EQ(0x51, byte);
  EXPECT_EQ(afterHeader + 2, file.reads());
}

// Issue #733: the image is synced on every planned restart and before every sleep, so a sync with nothing changed must
// cost the card nothing at all: no sector, and no header either. A boot that only reads and maps is such a run.
TEST(PsramImageFlash, ASyncWithNothingChangedWritesNothingNotEvenTheHeader) {
  SimImageFile file;
  {
    Boot boot(file);
    ASSERT_TRUE(boot.area.begin());
    fill(boot.area, kPage + 64, 128, 0x91);
    ASSERT_TRUE(boot.area.sync());
  }
  file.powerCycle();
  Boot again(file);
  ASSERT_TRUE(again.area.begin());
  uint8_t byte = 0;
  ASSERT_TRUE(again.area.read(kPage + 64, &byte, 1));
  ASSERT_NE(nullptr, again.area.map(2 * kPage, 64));
  const uint64_t before = file.bytesWritten();
  const std::vector<uint8_t> card = file.card();
  ASSERT_TRUE(again.area.sync());
  ASSERT_TRUE(again.area.sync());
  EXPECT_EQ(before, file.bytesWritten()) << "a clean image writes nothing";
  EXPECT_EQ(card, file.card());
  // One changed byte: that sector and the two header writes, nothing else.
  fill(again.area, 3 * kPage + 5, 1, 0x00);
  ASSERT_TRUE(again.area.sync());
  EXPECT_EQ(kSector + 2u * 16u, file.bytesWritten() - before);
}

TEST(PsramImageFlash, WhatWasNotSyncedIsLostToAPowerCut) {
  SimImageFile file;
  std::vector<uint8_t> synced;
  {
    Boot boot(file);
    ASSERT_TRUE(boot.area.begin());
    fill(boot.area, 500, 300, 0x71);
    ASSERT_TRUE(boot.area.sync());
    synced = contents(boot.area);
    fill(boot.area, 2 * kPage, 300, 0x72);
    ASSERT_TRUE(boot.area.erase(0, kSector));
  }
  file.powerCycle();
  Boot again(file);
  ASSERT_TRUE(again.area.begin());
  EXPECT_EQ(synced, contents(again.area));
}

TEST(PsramImageFlash, ACutAnywhereInASyncLeavesTheOldImageTheNewOneOrAnErasedOne) {
  for (const bool writeThrough : {false, true}) {
    // The bytes a sync of these changes writes: two headers and three sectors.
    const uint64_t syncBytes = 2 * 16 + 3 * kSector;
    int old = 0, fresh = 0, erased = 0;
    for (uint64_t cut = 0; cut <= syncBytes; cut += (cut < 40 || cut % kSector < 24 || cut > syncBytes - 40) ? 1 : 97) {
      SimImageFile file;
      std::vector<uint8_t> before, after;
      {
        Boot boot(file);
        ASSERT_TRUE(boot.area.begin());
        fill(boot.area, 0, 3000, 0x81);
        fill(boot.area, 3 * kPage, 200, 0x82);
        ASSERT_TRUE(boot.area.sync());
        before = contents(boot.area);
        fill(boot.area, 3000, 1000, 0x83);  // the first sector again
        ASSERT_TRUE(boot.area.erase(3 * kPage, kSector));
        fill(boot.area, kPage + kSector, 10, 0x84);
        after = contents(boot.area);
        file.writeThrough(writeThrough);
        file.cutAfterBytes(static_cast<int64_t>(cut));
        boot.area.sync();
      }
      file.powerCycle();
      Boot again(file);
      ASSERT_TRUE(again.area.begin());
      const std::vector<uint8_t> found = contents(again.area);
      EXPECT_TRUE(found == before || found == after || allErased(found))
          << "cut after " << cut << " B" << (writeThrough ? ", writes through" : "");
      if (cut == syncBytes) EXPECT_EQ(after, found) << "the whole sync landed";
      old += found == before;
      fresh += found == after;
      erased += allErased(found);
    }
    // Before the dirty header lands, after it, and once the clean one has: each outcome is reached.
    EXPECT_GT(old, 0);
    EXPECT_GT(erased, 0);
    EXPECT_GT(fresh, 0);
  }
}

TEST(PsramImageFlash, ACutInTheFirstSyncStartsErasedAndTheNextSyncCompletes) {
  SimImageFile file;
  {
    Boot boot(file);
    ASSERT_TRUE(boot.area.begin());
    fill(boot.area, 7, 100, 0x91);
    file.cutAfterBytes(kFileBytes / 2);
    EXPECT_FALSE(boot.area.sync());
  }
  file.powerCycle();
  Boot again(file);
  ASSERT_TRUE(again.area.begin());
  EXPECT_TRUE(allErased(contents(again.area)));
  fill(again.area, 7, 100, 0x92);
  ASSERT_TRUE(again.area.sync());
  EXPECT_EQ(kFileBytes, file.card().size());
}

TEST(PsramImageFlash, ARefusedSyncKeepsItsChangesForTheNext) {
  SimImageFile file;
  Boot boot(file);
  ASSERT_TRUE(boot.area.begin());
  ASSERT_TRUE(boot.area.sync());
  fill(boot.area, kPage, 50, 0xA1);
  file.refuseWrites(true);  // the card is away (USB mass storage, say)
  EXPECT_FALSE(boot.area.sync());
  file.refuseWrites(false);
  ASSERT_TRUE(boot.area.sync());
  const std::vector<uint8_t> synced = contents(boot.area);
  file.powerCycle();
  Boot again(file);
  ASSERT_TRUE(again.area.begin());
  EXPECT_EQ(synced, contents(again.area));
}

TEST(PsramImageFlash, AFileThatWentAwayIsWrittenAgainOnlyFromAWholeImage) {
  SimImageFile file;
  {
    Boot boot(file);
    ASSERT_TRUE(boot.area.begin());
    fill(boot.area, 0, 100, 0xB1);
    fill(boot.area, 3 * kPage, 100, 0xB2);
    ASSERT_TRUE(boot.area.sync());
  }
  file.powerCycle();
  {
    Boot partly(file);
    ASSERT_TRUE(partly.area.begin());
    fill(partly.area, 50, 10, 0x00);  // loads page 0 only
    file.remove();
    EXPECT_FALSE(partly.area.sync()) << "pages 1-3 were never read: no whole image to write";
    uint8_t byte = 0;
    EXPECT_FALSE(partly.area.read(3 * kPage, &byte, 1)) << "and can no longer be";
  }
  file.powerCycle();
  {
    Boot fresh(file);
    ASSERT_TRUE(fresh.area.begin());
    EXPECT_TRUE(allErased(contents(fresh.area))) << "the next boot starts erased";
    fill(fresh.area, 0, 100, 0xB3);
    ASSERT_TRUE(fresh.area.sync());
    file.remove();
    fill(fresh.area, 200, 10, 0xB4);
    ASSERT_TRUE(fresh.area.sync()) << "every page is in PSRAM: the file is written again";
    EXPECT_EQ(kFileBytes, file.card().size());
  }
}

TEST(PsramImageFlash, AFileOfAnotherSizeOrABadHeaderIsNoImage) {
  SimImageFile file;
  {
    Boot boot(file);
    ASSERT_TRUE(boot.area.begin());
    fill(boot.area, 0, 100, 0xC1);
    ASSERT_TRUE(boot.area.sync());
  }
  file.powerCycle();
  {
    Boot larger(file, 2 * kImageBytes);
    ASSERT_TRUE(larger.area.begin());
    EXPECT_TRUE(allErased(contents(larger.area))) << "an image of another size";
  }
  std::vector<uint8_t> corrupt = file.card();
  ASSERT_GT(corrupt.size(), 8u);
  file.powerCycle();
  {
    Boot boot(file);
    ASSERT_TRUE(boot.area.begin());
    uint8_t byte = 0;
    ASSERT_TRUE(boot.area.read(0, &byte, 1));
    EXPECT_EQ(0xC1, byte) << "the same file, read at its own size";
  }
  file.powerCycle();
  file.writeThrough(true);
  const uint8_t flipped = static_cast<uint8_t>(corrupt[8] ^ 0x01);  // the area's size in the header
  ASSERT_TRUE(file.write(8, &flipped, 1));
  file.powerCycle();
  Boot bad(file);
  ASSERT_TRUE(bad.area.begin());
  EXPECT_TRUE(allErased(contents(bad.area))) << "a header that fails its CRC";
}

TEST(PsramImageFlash, RefusesASizeThatIsNotWholePages) {
  SimImageFile file;
  Boot boot(file, kImageBytes + kSector);
  EXPECT_FALSE(boot.area.begin());
  uint8_t byte = 0;
  EXPECT_FALSE(boot.area.read(0, &byte, 1));
}

// The language slot over the PSRAM image, as on a board without the partition: built once, on the card as soon as
// it is built (FlashLangPartition syncs the area), and found again after a reboot without a rebuild.
TEST(PsramImageFlash, TheLanguageSlotIsOnTheCardOnceBuiltAndSurvivesAReboot) {
  namespace flp = FlashLangPartition;
  constexpr uint32_t kAreaBytes = 1024 * 1024;
  constexpr uint16_t kKeys = 40;
  struct Strings {
    int calls = 0;
    static bool produce(void* ctx, flp::StringsWriter& out) {
      ++static_cast<Strings*>(ctx)->calls;
      for (uint16_t key = 0; key < kKeys; ++key) {
        const std::string s = "text " + std::to_string(key);
        if (!out.beginKey(key) || !out.write(s.c_str(), s.size() + 1)) return false;
      }
      return true;
    }
  } producer;
  const flp::Strings strings{2048, &Strings::produce, &producer};

  SimImageFile file;
  {
    Boot boot(file, kAreaBytes);
    ASSERT_TRUE(boot.area.begin());
    sim_area::use(&boot.area);
    flp::Mapped m{};
    ASSERT_TRUE(flp::acquire(1, 0x1234, kKeys, strings, nullptr, &m));
    EXPECT_STREQ("text 39", m.data + m.offsets[39]);
    flp::unmap();
    sim_area::use(nullptr);
  }
  file.powerCycle();  // no sync of our own: the build's
  Boot again(file, kAreaBytes);
  ASSERT_TRUE(again.area.begin());
  sim_area::use(&again.area);
  const uint32_t readsBefore = file.reads();
  flp::Mapped m{};
  ASSERT_TRUE(flp::acquire(1, 0x1234, kKeys, strings, nullptr, &m));
  EXPECT_EQ(1, producer.calls) << "the slot came back from the card, not from a rebuild";
  EXPECT_STREQ("text 7", m.data + m.offsets[7]);
  EXPECT_EQ(readsBefore + 1, file.reads()) << "one page: the slot's";
  flp::unmap();
  sim_area::use(nullptr);
}

// The TTF glyph store over the PSRAM image, through its font-area window: an install is on the card as soon as it
// finishes (the store syncs its flash), so a reboot opens it without installing again.
TEST(PsramImageFlash, AGlyphStoreInstallIsOnTheCardOnceFinished) {
  constexpr uint32_t kAreaBytes = 1024 * 1024;
  const glyphstore::Layout layout = gst_test::smallLayout();
  SimImageFile file;
  {
    Boot boot(file, kAreaBytes);
    ASSERT_TRUE(boot.area.begin());
    sim_area::use(&boot.area);
    FlashAreaRegion window;
    ASSERT_TRUE(window.begin());
    FlashGlyphStore store(window);
    ASSERT_TRUE(gst_test::install(store, layout));
    sim_area::use(nullptr);
  }
  file.powerCycle();
  Boot again(file, kAreaBytes);
  ASSERT_TRUE(again.area.begin());
  sim_area::use(&again.area);
  {
    FlashAreaRegion window;
    ASSERT_TRUE(window.begin());
    FlashGlyphStore store(window);
    EXPECT_EQ(gst_test::OpenResult::Ready, store.open(gst_test::kIdentityA));
  }
  sim_area::use(nullptr);
}
