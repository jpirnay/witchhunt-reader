// A power cut at every step of every write. Whatever the store reported as stored is intact after the
// reboot, nothing is served torn, and writing carries on without programming any byte twice.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

constexpr uint32_t kBodyPpem = 29 << 6;
constexpr uint32_t kHeadingPpem = 35 << 6;

// The face indices a session claimed; a torn claim makes the next one land elsewhere, so keys are built
// from what faceIndex() returned, not assumed. The accent is a second family of the set at the body's size.
struct Sizes {
  uint8_t body = 0xFF;
  uint8_t heading = 0xFF;
  uint8_t accent = 0xFF;
};

enum class Face : uint8_t { Body, Heading, Accent };

struct GlyphSpec {
  uint32_t codepoint;
  uint8_t style;
  Face face;
  uint8_t width;
  uint8_t height;

  uint32_t key(const Sizes& sizes) const {
    return makeKey(codepoint, style,
                   face == Face::Heading  ? sizes.heading
                   : face == Face::Accent ? sizes.accent
                                          : sizes.body);
  }
};

// 'B' is 16 x 24 so that a record lands at an offset whose slot value, cut short, would once have read as the
// "no glyph" marker. The accent's 'C' is the body's codepoint and style in another family.
const GlyphSpec kBatchA[] = {{'A', 0, Face::Body, 9, 12}, {'B', 0, Face::Body, 16, 24}, {' ', 0, Face::Body, 0, 0}};
const GlyphSpec kBatchB[] = {{'C', 0, Face::Body, 11, 13},
                             {'d', 1, Face::Body, 8, 12},
                             {0x4E2D, 0, Face::Heading, 24, 26},
                             {'C', 0, Face::Accent, 12, 14}};
const GlyphSpec kAfter[] = {{'E', 0, Face::Body, 9, 13}, {'F', 0, Face::Accent, 9, 12}};
constexpr uint32_t kNoGlyphCodepoint = 0x2603;

// What each step of a session reported.
struct Outcome {
  bool installed = false;
  Sizes sizes;
  bool batchA = false;
  bool noGlyph = false;
  bool batchB = false;
  bool pair = false;
  bool accentPair = false;
};

Layout sweepLayout() {
  Layout layout = smallLayout();
  layout.styleSize[0] = 256;  // a short style keeps the sweep fast
  return layout;
}

// A body style in flash mode and an accent style in SD mode (2c-4): its header record carries its file and outlines.
Layout mixedLayout() {
  Layout layout = sweepLayout();
  layout.geometry.styleCount = 2;
  layout.styleOffset[1] = layout.styleOffset[0] + kSectorBytes;
  layout.styleSize[1] = 256;
  layout.styleMode[1] = kModeSd;
  return layout;
}

StyleInfo sdAccent() {
  StyleInfo info{};
  info.fileStamp = 0x5EED;
  info.fileSize = 200000;
  info.outlines = {4000, 256, 150000};
  return info;
}

const StyleInfo kMixedInfo[kMaxStyles] = {{}, sdAccent()};

// What a session installs.
struct Install {
  Layout layout;
  const StyleInfo* sdInfo;
};

bool claim(FlashGlyphStore& store, Sizes& sizes) {
  return store.faceIndex(0, kBodyPpem, &sizes.body) && store.faceIndex(0, kHeadingPpem, &sizes.heading) &&
         store.faceIndex(1, kBodyPpem, &sizes.accent);
}

bool commitAll(FlashGlyphStore& store, const GlyphSpec* glyphs, const size_t count, const Sizes& sizes) {
  alignas(4) uint8_t buffer[2048];
  GlyphBatch batch(buffer, sizeof buffer);
  for (size_t i = 0; i < count; ++i) {
    if (!stage(batch, glyphs[i].key(sizes), glyphs[i].width, glyphs[i].height)) return false;
  }
  return store.commit(batch) == Commit::Stored;
}

// Every kind of write a session makes, from the install on.
Outcome runSession(SimNorFlash& flash, const Install& what) {
  Outcome out;
  FlashGlyphStore store(flash);
  out.installed = install(store, what.layout, kIdentityA, what.sdInfo);
  if (!out.installed || store.open(kIdentityA) != OpenResult::Ready || !claim(store, out.sizes)) return out;
  out.batchA = commitAll(store, kBatchA, std::size(kBatchA), out.sizes);
  out.noGlyph = store.markNoGlyph(makeKey(kNoGlyphCodepoint, 0, out.sizes.body)) == Commit::Stored;
  out.batchB = commitAll(store, kBatchB, std::size(kBatchB), out.sizes);
  out.pair = store.storePair('A', 'V', 0, 0, -74);
  out.accentPair = store.storePair('A', 'V', 1, 0, -31);
  return out;
}

void checkGlyphs(const FlashGlyphStore& store, const GlyphSpec* glyphs, const size_t count, const Sizes& sizes,
                 const bool reported, const std::string& where) {
  for (size_t i = 0; i < count; ++i) {
    const GlyphSpec& g = glyphs[i];
    if (reported) {
      EXPECT_TRUE(storedIntact(store, g.key(sizes), g.width, g.height)) << where << " U+" << std::hex << g.codepoint;
    } else {
      EXPECT_TRUE(absentOrIntact(store, g.key(sizes), g.width, g.height)) << where << " U+" << std::hex << g.codepoint;
    }
  }
}

void checkAfterReboot(SimNorFlash& flash, const Install& what, const Outcome& out, const std::string& where) {
  FlashGlyphStore store(flash);
  const OpenResult opened = store.open(kIdentityA);
  if (out.installed) {
    ASSERT_EQ(OpenResult::Ready, opened) << where;
  } else if (opened != OpenResult::Ready) {
    EXPECT_EQ(OpenResult::NotInstalled, opened) << where;
    return;  // the next install starts from scratch
  }
  // An installed store says every style's mode, and an SD-mode style's file, as the install gave them.
  for (uint8_t s = 0; s < what.layout.geometry.styleCount; ++s) {
    StyleInfo info{};
    ASSERT_TRUE(store.styleInfo(s, info)) << where;
    EXPECT_EQ(what.layout.styleMode[s], info.mode) << where;
    if (info.mode != kModeSd) continue;
    EXPECT_EQ(what.sdInfo[s].fileStamp, info.fileStamp) << where;
    EXPECT_EQ(what.sdInfo[s].fileSize, info.fileSize) << where;
    EXPECT_EQ(what.sdInfo[s].outlines.fileOffset, info.outlines.fileOffset) << where;
    EXPECT_EQ(what.sdInfo[s].outlines.fontOffset, info.outlines.fontOffset) << where;
    EXPECT_EQ(what.sdInfo[s].outlines.length, info.outlines.length) << where;
  }
  // A size claimed in the session answers the same now; a torn claim lands on a fresh entry instead.
  Sizes sizes;
  ASSERT_TRUE(claim(store, sizes)) << where;
  if (out.sizes.body != 0xFF) {
    EXPECT_EQ(out.sizes.body, sizes.body) << where;
  }
  if (out.sizes.heading != 0xFF) {
    EXPECT_EQ(out.sizes.heading, sizes.heading) << where;
  }
  if (out.sizes.accent != 0xFF) {
    EXPECT_EQ(out.sizes.accent, sizes.accent) << where;
  }
  EXPECT_NE(sizes.body, sizes.accent) << where;  // one size, two families: two faces
  checkGlyphs(store, kBatchA, std::size(kBatchA), sizes, out.batchA, where);
  checkGlyphs(store, kBatchB, std::size(kBatchB), sizes, out.batchB, where);
  const Lookup noGlyph = store.find(makeKey(kNoGlyphCodepoint, 0, sizes.body), nullptr);
  if (out.noGlyph) {
    EXPECT_EQ(Lookup::NoGlyph, noGlyph) << where;
  } else {
    EXPECT_NE(Lookup::Found, noGlyph) << where;
  }
  int16_t kern = 0;
  if (store.findPair('A', 'V', 0, 0, &kern)) {
    EXPECT_EQ(-74, kern) << where;
  } else {
    EXPECT_FALSE(out.pair) << where;
  }
  if (store.findPair('A', 'V', 1, 0, &kern)) {
    EXPECT_EQ(-31, kern) << where;
  } else {
    EXPECT_FALSE(out.accentPair) << where;
  }
  // No glyph of the session reads as "the font has none" unless it was marked so.
  for (const GlyphSpec& g : kBatchA) EXPECT_NE(Lookup::NoGlyph, store.find(g.key(sizes), nullptr)) << where;
  for (const GlyphSpec& g : kBatchB) EXPECT_NE(Lookup::NoGlyph, store.find(g.key(sizes), nullptr)) << where;
  // The store carries on: a new batch lands, and everything is still there after another reboot.
  ASSERT_FALSE(store.wipePending()) << where;
  ASSERT_TRUE(commitAll(store, kAfter, std::size(kAfter), sizes)) << where;
  store.close();
  FlashGlyphStore again(flash);
  ASSERT_EQ(OpenResult::Ready, again.open(kIdentityA)) << where;
  checkGlyphs(again, kAfter, std::size(kAfter), sizes, true, where);
  EXPECT_EQ(0u, flash.violations()) << where;
  EXPECT_EQ(0u, flash.reprograms()) << where;
}

// 200 more glyphs, 12.8 KB of log over four segments, so a cut can leave a wipe half done.
constexpr uint32_t kManyFirst = 0x4E00;
constexpr uint32_t kMany = 200;
constexpr Sizes kPopulateSizes{0, 1, 2};  // what claim() returns on a fresh store

bool commitMany(FlashGlyphStore& store) {
  alignas(4) uint8_t buffer[4096];
  GlyphBatch batch(buffer, sizeof buffer);
  for (uint32_t i = 0; i < kMany; ++i) {
    const uint32_t key = makeKey(kManyFirst + i, 0, kPopulateSizes.body);
    if (stage(batch, key, 11, 16)) continue;
    if (store.commit(batch) != Commit::Stored || !stage(batch, key, 11, 16)) return false;
  }
  return store.commit(batch) == Commit::Stored;
}

// A store with data; with wipe pending as the full path leaves it, or not (a wipe asked for directly).
void populate(SimNorFlash& flash, const bool pending) {
  FlashGlyphStore store(flash);
  ASSERT_TRUE(install(store, sweepLayout()));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  Sizes sizes;
  ASSERT_TRUE(claim(store, sizes));
  ASSERT_EQ(kPopulateSizes.body, sizes.body);
  ASSERT_EQ(kPopulateSizes.heading, sizes.heading);
  ASSERT_EQ(kPopulateSizes.accent, sizes.accent);
  ASSERT_TRUE(commitAll(store, kBatchA, std::size(kBatchA), sizes));
  ASSERT_TRUE(store.storePair('A', 'V', 0, 0, -74));
  ASSERT_TRUE(store.storePair('A', 'V', 1, 0, -31));
  ASSERT_TRUE(commitMany(store));
  if (pending) fillSizeTable(store);
}

// Cuts the power at every step of a wipe. After the reboot either wipe is still pending (and the next wipe()
// finishes it), or the wipe had not begun, or it had completed; never half. Then the store goes on working.
void sweepWipe(const bool pending) {
  SimNorFlash dry(kFlashBytes);
  populate(dry, pending);
  if (::testing::Test::HasFatalFailure()) return;
  const uint64_t before = dry.steps();
  {
    FlashGlyphStore store(dry);
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
    ASSERT_TRUE(store.wipe());
  }
  const uint64_t wipeSteps = dry.steps() - before;
  ASSERT_GT(wipeSteps, 0u);
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < wipeSteps; ++cut) {
      const std::string where = std::string(pending ? "pending" : "not pending") + " wipe cut " + std::to_string(cut) +
                                (partial ? " partial" : "");
      SimNorFlash flash(kFlashBytes);
      populate(flash, pending);
      if (::testing::Test::HasFatalFailure()) return;
      {
        FlashGlyphStore store(flash);
        ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << where;
        flash.partialCut(partial);
        flash.cutAfterSteps(static_cast<int64_t>(cut));
        EXPECT_FALSE(store.wipe()) << where;
      }
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      FlashGlyphStore store(flash);
      ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << where;  // a wipe never touches the header
      checkGlyphs(store, kBatchA, std::size(kBatchA), kPopulateSizes, false, where);
      if (store.wipePending()) {
        ASSERT_TRUE(store.wipe()) << where;  // the next safe point finishes the job
        for (const GlyphSpec& g : kBatchA) {
          EXPECT_EQ(Lookup::NotStored, store.find(g.key(kPopulateSizes), nullptr)) << where;
        }
        EXPECT_FALSE(store.findPair('A', 'V', 0, 0, nullptr)) << where;
        EXPECT_FALSE(store.findPair('A', 'V', 1, 0, nullptr)) << where;
      } else {
        // Not pending, so nothing will finish it: the wipe must not have begun, or must have completed.
        uint32_t intact = 0;
        uint32_t absent = 0;
        for (uint32_t i = 0; i < kMany; ++i) {
          const uint32_t key = makeKey(kManyFirst + i, 0, kPopulateSizes.body);
          if (storedIntact(store, key, 11, 16)) ++intact;
          if (store.find(key, nullptr) == Lookup::NotStored) ++absent;
        }
        EXPECT_TRUE(intact == kMany || absent == kMany) << where << ": intact " << intact << ", absent " << absent;
      }
      // The store goes on working, without programming any byte twice.
      Sizes sizes;
      ASSERT_TRUE(claim(store, sizes)) << where;
      ASSERT_TRUE(commitAll(store, kAfter, std::size(kAfter), sizes)) << where;
      EXPECT_EQ(0u, flash.violations()) << where;
      EXPECT_EQ(0u, flash.reprograms()) << where;
      if (::testing::Test::HasFailure()) return;
    }
  }
}

// Cuts the power at every step of a session, from the install on.
void sweepSession(const Install& what) {
  SimNorFlash dry(kFlashBytes);
  const Outcome all = runSession(dry, what);
  ASSERT_TRUE(all.installed && all.batchA && all.noGlyph && all.batchB && all.pair && all.accentPair);
  const uint64_t steps = dry.steps();
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < steps; ++cut) {
      SimNorFlash flash(kFlashBytes);
      flash.partialCut(partial);
      flash.cutAfterSteps(static_cast<int64_t>(cut));
      const Outcome out = runSession(flash, what);
      const std::string where = "cut " + std::to_string(cut) + (partial ? " partial" : "");
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      checkAfterReboot(flash, what, out, where);
      if (::testing::Test::HasFailure()) return;  // one cut's report is enough to debug from
    }
  }
}

// --- Making room: a head move, a reclaim and an index rebuild ---
// Each builds its store once and copies the chip for every cut point. Record n is a glyph under ringKey(n).

using Budget = FlashGlyphStore::Budget;
using Room = FlashGlyphStore::Room;
using RoomWork = FlashGlyphStore::RoomWork;

// Commits records first to last - 1 as one batch, each width x height.
Commit commitRecords(FlashGlyphStore& store, const uint32_t first, const uint32_t last, const uint8_t width,
                     const uint8_t height) {
  alignas(4) uint8_t buffer[8192];
  GlyphBatch batch(buffer, sizeof buffer);
  for (uint32_t n = first; n < last; ++n) {
    if (!stage(batch, ringKey(n), width, height)) return Commit::Refused;
  }
  return store.commit(batch);
}

// The segment holding record n; UINT32_MAX when it is not found.
uint32_t segmentOf(const FlashGlyphStore& store, const uint32_t n) {
  const EpdGlyph* glyph = nullptr;
  if (store.find(ringKey(n), &glyph) != Lookup::Found) return UINT32_MAX;
  return glyph->dataOffset / store.geometry().segmentBytes;
}

void draw(const FlashGlyphStore& store, const uint32_t n, const uint32_t times = 1) {
  const EpdGlyph* glyph = nullptr;
  ASSERT_EQ(Lookup::Found, store.find(ringKey(n), &glyph)) << n;
  for (uint32_t i = 0; i < times; ++i) store.noteDrawn(glyph);
}

// Three sessions after a cut, each as a reader's goes: open, make room as a chapter open does, store two new glyphs,
// close; then a last boot that only checks. After each boot no wipe is pending, what was stored before the cut
// (records 0 to before - 1) is found whole or not at all, and what the sessions stored is found whole. atBoot(store)
// runs in the first session before it makes room, afterRoom(store) after.
template <typename AtBoot, typename AfterRoom>
void sessionsAfterACut(SimNorFlash& flash, const uint32_t before, const std::string& where, AtBoot&& atBoot,
                       AfterRoom&& afterRoom) {
  uint32_t next = before;  // the next new glyph
  for (uint32_t session = 0; session <= 3; ++session) {
    const std::string at = where + ", boot " + std::to_string(session);
    FlashGlyphStore store(flash);
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << at;
    ASSERT_FALSE(store.wipePending()) << at;
    for (uint32_t n = 0; n < before; ++n) {
      ASSERT_TRUE(absentOrIntact(store, ringKey(n), kRingWidth, kRingHeight)) << at << ": record " << n;
    }
    for (uint32_t n = before; n < next; ++n) {
      ASSERT_TRUE(storedIntact(store, ringKey(n), kRingWidth, kRingHeight)) << at << ": record " << n;
    }
    if (session == 3) break;  // the last boot only checks
    if (session == 0) atBoot(store);
    ASSERT_EQ(Room::Enough, store.makeRoom(Budget::Open)) << at;
    if (session == 0) afterRoom(store);
    ASSERT_EQ(Commit::Stored, commitRecords(store, next, next + 2, kRingWidth, kRingHeight)) << at;
    next += 2;
    for (uint32_t n = before; n < next; ++n) {
      ASSERT_TRUE(storedIntact(store, ringKey(n), kRingWidth, kRingHeight)) << at << ": record " << n;
    }
    if (::testing::Test::HasFailure()) return;
  }
  EXPECT_EQ(0u, flash.violations()) << where;
  EXPECT_EQ(0u, flash.reprograms()) << where;
}

// --- A batch across a segment's end ---

// 60 x 60 is a 920-byte record. The head, segment 0, holds all but one of what fits it, so of the batch's four the
// first ends the head and the other three open segment 1.
constexpr uint8_t kSide = 60;
constexpr uint32_t kMoveBatch = 4;

// The 60 x 60 records that fit a segment past its header.
uint32_t sidesPerSegment(const Layout& layout) {
  return (layout.geometry.segmentBytes - kSegmentHeaderBytes) / recordSize(kSide, kSide);
}

std::string sectorsOf(const Layout& layout) {
  return std::to_string(layout.geometry.segmentBytes / kSectorBytes) + "-sector segments, ";
}

// With `tornNext`, segment 1 holds a header a cut tore, so the head move erases it (its first sector) before writing
// its own.
SimNorFlash headMoveStore(const Layout& layout, const bool tornNext) {
  SimNorFlash flash(kFlashBytes);
  {
    FlashGlyphStore store(flash);
    EXPECT_TRUE(installAndOpen(store, layout));
    EXPECT_EQ(Commit::Stored, commitRecords(store, 0, sidesPerSegment(layout) - 1, kSide, kSide));
  }
  if (tornNext) {
    const SegmentHeader torn{kSegmentMagic, 2, kErased, kErased};
    EXPECT_TRUE(flash.program(layout.geometry.logOffset + layout.geometry.segmentBytes, &torn, 6));
  }
  return flash;
}

void sweepHeadMove(const Layout& layout, const bool tornNext) {
  const uint32_t first = sidesPerSegment(layout) - 1;  // the batch's first record
  const uint32_t last = first + kMoveBatch;
  const SimNorFlash before = headMoveStore(layout, tornNext);
  if (::testing::Test::HasFailure()) return;
  // The dry run: every write of the batch is a cut point. The record that ends the head, the erase of segment 1 when it
  // holds a torn header, segment 1's header, the three records after it, and the four slots.
  uint64_t steps = 0;
  {
    SimNorFlash dry = before;
    FlashGlyphStore store(dry);
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
    const uint64_t start = dry.steps();
    ASSERT_EQ(Commit::Stored, commitRecords(store, first, last, kSide, kSide));
    steps = dry.steps() - start;
    ASSERT_EQ(0u, segmentOf(store, first));
    for (uint32_t n = first + 1; n < last; ++n) ASSERT_EQ(1u, segmentOf(store, n)) << n;
  }
  const uint32_t record = recordSize(kSide, kSide);
  ASSERT_EQ(record + (tornNext ? 1u : 0u) + offsetof(SegmentHeader, retired) + (kMoveBatch - 1) * record +
                kMoveBatch * kSlotBytes,
            steps);
  const uint32_t all = last + 1;  // and one record more after the cut
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < steps; ++cut) {
      const std::string where = sectorsOf(layout) + (tornNext ? "torn next, " : "") + "head move cut " +
                                std::to_string(cut) + (partial ? " partial" : "");
      SimNorFlash flash = before;
      {
        FlashGlyphStore store(flash);
        ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << where;
        flash.partialCut(partial);
        flash.cutAfterSteps(static_cast<int64_t>(cut));
        EXPECT_EQ(Commit::WriteFailed, commitRecords(store, first, last, kSide, kSide)) << where;
      }
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      FlashGlyphStore store(flash);
      ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << where;
      ASSERT_FALSE(store.wipePending()) << where;
      for (uint32_t n = 0; n < first; ++n) {
        EXPECT_TRUE(storedIntact(store, ringKey(n), kSide, kSide)) << where << ": record " << n;
      }
      for (uint32_t n = first; n < last; ++n) {
        EXPECT_TRUE(absentOrIntact(store, ringKey(n), kSide, kSide)) << where << ": record " << n;
      }
      // Writing goes on: what the cut lost of the batch is stored again, with one record more, and all of it is found,
      // after another reboot too.
      alignas(4) uint8_t buffer[8192];
      GlyphBatch batch(buffer, sizeof buffer);
      for (uint32_t n = first; n < all; ++n) {
        if (store.find(ringKey(n), nullptr) == Lookup::NotStored) {
          ASSERT_TRUE(stage(batch, ringKey(n), kSide, kSide)) << where;
        }
      }
      ASSERT_EQ(Commit::Stored, store.commit(batch)) << where;
      store.close();
      ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << where;
      for (uint32_t n = 0; n < all; ++n) {
        EXPECT_TRUE(storedIntact(store, ringKey(n), kSide, kSide)) << where << ": record " << n;
      }
      EXPECT_EQ(0u, flash.violations()) << where;
      EXPECT_EQ(0u, flash.reprograms()) << where;
      if (::testing::Test::HasFailure()) return;
    }
  }
}

// --- A reclaim ---

// The helper ring, built once and reopened at every cut. Each session stores records in the head until it has room for
// one more, which keeps the counts again (one segment is free), and draws glyphs of segment 0. The reclaim copies the
// first it keeps to the head and the rest to the segment it opens, so that segment's header is among the cut points.
struct ReclaimCase {
  Layout layout;
  std::vector<uint32_t> drawn;  // records of segment 0, drawn once each
  std::vector<uint32_t> kept;   // the ones it copies, in log order: at most half a segment's payload, the newest first
};

// Of four drawn, three fit half a segment; the oldest is dropped.
ReclaimCase oneSectorReclaim() { return {smallLayout(), {0, 2, 3, 5}, {2, 3, 5}}; }

// All four are kept. A copy a cut tears in the first sector of the segment it opened leaves the rest of that segment
// to later sessions, past the next sector boundary, as on the device's 64 KB segments.
ReclaimCase twoSectorReclaim() { return {smallLayout(1024, 512, 65536, 2 * kSectorBytes), {1, 3, 5, 7}, {1, 3, 5, 7}}; }

struct Ring {
  SimNorFlash flash{kFlashBytes};
  uint32_t perSegment = 0;  // records a segment holds
  uint32_t filled = 0;      // records fillRing() stored: the head holds one
  uint32_t before = 0;      // with the session's in the head, every record stored before the cut
};

void buildRing(const Layout& layout, Ring& ring) {
  FlashGlyphStore store(ring.flash);
  alignas(4) uint8_t buffer[1024];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(installAndOpen(store, layout));
  ring.perSegment = (layout.geometry.segmentBytes - kSegmentHeaderBytes) / recordSize(kRingWidth, kRingHeight);
  ring.filled = fillRing(store, batch);
  ring.before = ring.filled + ring.perSegment - 2;
}

void reclaimSession(FlashGlyphStore& store, const Ring& ring, const ReclaimCase& what) {
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_EQ(Commit::Stored, commitRecords(store, ring.filled, ring.before, kRingWidth, kRingHeight));
  ASSERT_TRUE(store.keepCountsIfDue());
  ASSERT_EQ(1u, store.freeSegments());
  for (const uint32_t n : what.drawn) draw(store, n);
}

void sweepReclaim(const ReclaimCase& what) {
  Ring ring;
  buildRing(what.layout, ring);
  if (::testing::Test::HasFailure()) return;
  uint64_t steps = 0;
  {
    SimNorFlash dry = ring.flash;
    FlashGlyphStore store(dry);
    reclaimSession(store, ring, what);
    if (::testing::Test::HasFailure()) return;
    const uint32_t head = segmentOf(store, ring.before - 1);
    const uint64_t start = dry.steps();
    RoomWork work;
    ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
    steps = dry.steps() - start;
    ASSERT_EQ(1u, work.reclaimed);
    ASSERT_EQ(what.kept.size(), work.kept);
    ASSERT_EQ(ring.perSegment - what.kept.size(), work.dropped);
    ASSERT_EQ(head, segmentOf(store, what.kept[0]));
    for (size_t i = 1; i < what.kept.size(); ++i) ASSERT_EQ(head + 1, segmentOf(store, what.kept[i])) << i;
  }
  // The copies, each its record then its slot, the opened segment's header, segment 0's retire word and its erase.
  const uint32_t copy = recordSize(kRingWidth, kRingHeight) + kSlotBytes;
  ASSERT_EQ(what.kept.size() * copy + offsetof(SegmentHeader, retired) + sizeof(uint32_t) +
                what.layout.geometry.segmentBytes / kSectorBytes,
            steps);
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < steps; ++cut) {
      const std::string where =
          sectorsOf(what.layout) + "reclaim cut " + std::to_string(cut) + (partial ? " partial" : "");
      SimNorFlash flash = ring.flash;
      {
        FlashGlyphStore store(flash);
        reclaimSession(store, ring, what);
        if (::testing::Test::HasFailure()) return;
        flash.partialCut(partial);
        flash.cutAfterSteps(static_cast<int64_t>(cut));
        EXPECT_EQ(Room::Failed, store.makeRoom(Budget::Idle)) << where;
      }
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      sessionsAfterACut(
          flash, ring.before, where,
          [&](const FlashGlyphStore& store) {
            // The reclaim writes only past the head's records and into segment 0: the segments in between are whole.
            for (uint32_t n = ring.perSegment; n < ring.before; ++n) {
              EXPECT_TRUE(storedIntact(store, ringKey(n), kRingWidth, kRingHeight)) << where << ": record " << n;
            }
          },
          [](const FlashGlyphStore&) {});
      if (::testing::Test::HasFailure()) return;
    }
  }
}

// --- An index rebuild ---

// A crowded index of 512 slots, one sector, so a cut in its erase leaves half of it erased and half holding the old
// slots. A few "no glyph" marks, then records stored one at a time, with an idle reclaim whenever fewer than two
// segments are free, until the index stops the commits: 358 slots in use, 70 % of 512, most of them stale.
constexpr uint32_t kCrowdedSlots = 512;
constexpr uint32_t kMarks = 8;

uint32_t markKey(const uint32_t i) { return makeKey(0x3000 + i, 0, 0); }

SimNorFlash crowdedIndex(uint32_t& stored) {
  SimNorFlash flash(kFlashBytes);
  FlashGlyphStore store(flash);
  EXPECT_TRUE(installAndOpen(store, smallLayout(kCrowdedSlots)));
  for (uint32_t i = 0; i < kMarks; ++i) EXPECT_EQ(Commit::Stored, store.markNoGlyph(markKey(i))) << i;
  uint32_t n = 0;
  Commit commit;
  while ((commit = commitRecords(store, n, n + 1, kRingWidth, kRingHeight)) == Commit::Stored && n < 1000) {
    ++n;
    if (store.freeSegments() < kFreeSegmentsTarget) {
      EXPECT_NE(Room::Failed, store.makeRoom(Budget::Idle)) << n;
    }
  }
  EXPECT_EQ(Commit::Full, commit);
  EXPECT_EQ(0u, store.rebuilds());
  store.close();
  stored = n;
  return flash;
}

// The commit of record `stored` finds the index past its load limit again, which stops writes; the glyphs found are
// drawn, 0 to 3 times each, so the rebuild carries counts.
void rebuildSession(FlashGlyphStore& store, const uint32_t stored) {
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  store.keepCountsIfDue();  // the font set does at the end of its load
  ASSERT_EQ(Commit::Full, commitRecords(store, stored, stored + 1, kRingWidth, kRingHeight));
  ASSERT_TRUE(store.needsRoom());
  for (uint32_t n = 0; n < stored; ++n) {
    if (store.find(ringKey(n), nullptr) == Lookup::Found) draw(store, n, n % 4);
  }
}

void sweepRebuild() {
  uint32_t stored = 0;
  const SimNorFlash crowded = crowdedIndex(stored);
  if (::testing::Test::HasFailure()) return;
  uint64_t steps = 0;
  uint32_t slots = 0;
  {
    SimNorFlash dry = crowded;
    FlashGlyphStore store(dry);
    rebuildSession(store, stored);
    if (::testing::Test::HasFailure()) return;
    ASSERT_TRUE(store.countsKept());
    const uint32_t stale = store.usedSlots() - store.liveSlots();
    ASSERT_GT(uint64_t{stale} * 100, uint64_t{kCrowdedSlots} * kRebuildStalePercent);
    const uint64_t start = dry.steps();
    RoomWork work;
    ASSERT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
    steps = dry.steps() - start;
    ASSERT_TRUE(work.rebuilt);
    ASSERT_EQ(0u, work.reclaimed);
    slots = store.usedSlots();
    ASSERT_EQ(slots, store.liveSlots());
    ASSERT_GT(slots, 0u);
  }
  // The journal entry's base, the index's erase (one sector), the slots, and the entry's done word.
  ASSERT_EQ(sizeof(uint32_t) + 1 + slots * kSlotBytes + sizeof(uint32_t), steps);
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < steps; ++cut) {
      const std::string where = "rebuild cut " + std::to_string(cut) + (partial ? " partial" : "");
      SimNorFlash flash = crowded;
      {
        FlashGlyphStore store(flash);
        rebuildSession(store, stored);
        if (::testing::Test::HasFailure()) return;
        flash.partialCut(partial);
        flash.cutAfterSteps(static_cast<int64_t>(cut));
        EXPECT_EQ(Room::Failed, store.makeRoom(Budget::Idle)) << where;
      }
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      // A rebuild whose done word reads programmed, even in part, had every slot in: it counts as finished. Any other
      // is redone by the first Open, an interrupted one for its journal entry and one never begun for the stale slots.
      bool finished = false;
      sessionsAfterACut(
          flash, stored + 1, where,
          [&](const FlashGlyphStore& store) {
            JournalEntry entry{};
            ASSERT_TRUE(flash.read(kJournalOffset, &entry, sizeof entry)) << where;
            finished = entry.done != kErased;
            if (finished) {
              EXPECT_EQ(slots, store.usedSlots()) << where;
              EXPECT_EQ(slots, store.liveSlots()) << where;
            }
            EXPECT_NE(finished, store.needsRoom(Budget::Open)) << where;
            for (uint32_t i = 0; i < kMarks; ++i) {
              EXPECT_NE(Lookup::Found, store.find(markKey(i), nullptr)) << where << ": mark " << i;
            }
          },
          [&](const FlashGlyphStore& store) {
            EXPECT_EQ(finished ? 0u : 1u, store.rebuilds()) << where;
            EXPECT_EQ(store.usedSlots(), store.liveSlots()) << where;
          });
      if (::testing::Test::HasFailure()) return;
    }
  }
}

}  // namespace

TEST(GlyphStorePowerCut, EveryCutDuringASessionLeavesAConsistentStore) { sweepSession({sweepLayout(), nullptr}); }

TEST(GlyphStorePowerCut, EveryCutDuringASessionWithAnSdModeStyleLeavesAConsistentStore) {
  // The install writes a flash-mode block, an SD-mode block and a header whose records carry the SD-mode file: a cut
  // anywhere before the header's last byte reads as not installed, and an installed store's records are whole.
  sweepSession({mixedLayout(), kMixedInfo});
}

TEST(GlyphStorePowerCut, EveryCutDuringAWipeIsRecoverable) { sweepWipe(true); }

TEST(GlyphStorePowerCut, EveryCutDuringAWipeOfAStoreNotFullIsRecoverable) { sweepWipe(false); }

TEST(GlyphStorePowerCut, EveryCutDuringAHeadMoveLeavesAConsistentStore) {
  // Segments of one sector, where a torn record closes its segment, and of two, where it leaves the rest of the
  // segment to later sessions past the next sector boundary, as on the device's 64 KB segments.
  for (const Layout& layout : {smallLayout(), smallLayout(1024, 512, 65536, 2 * kSectorBytes)}) {
    for (const bool tornNext : {false, true}) {
      sweepHeadMove(layout, tornNext);
      if (HasFailure()) return;
    }
  }
}

TEST(GlyphStorePowerCut, EveryCutDuringAReclaimLeavesAConsistentStore) {
  sweepReclaim(oneSectorReclaim());
  if (HasFailure()) return;
  sweepReclaim(twoSectorReclaim());
}

TEST(GlyphStorePowerCut, EveryCutDuringARebuildLeavesAConsistentStore) { sweepRebuild(); }
