#include <ExpectedShaping.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

#include "OtShaperTestHelpers.h"

// Deviation 1 for the plans: Plan::build and Plan::load take their blocks
// nothrow, a refused block fails the plan cleanly or leaves its table
// unfiltered, and nothing in the engine allocates through a throwing
// operator new, which aborts under -fno-exceptions. An executable of its
// own: it replaces the global operator new and delete.

// A plan owns its blocks: it moves, and never copies.
static_assert(!std::is_copy_constructible_v<ot::Plan> && !std::is_copy_assignable_v<ot::Plan>);
static_assert(std::is_nothrow_move_constructible_v<ot::Plan> && std::is_nothrow_move_assignable_v<ot::Plan>);

namespace {

constexpr size_t NEVER = SIZE_MAX;

// While armed: throwing and nothrow calls since arming, and the nothrow call
// (counted from 0) that is refused. The live bytes are those of the nothrow
// blocks made while armed, until each is freed, armed or not.
struct AllocCounts {
  bool armed = false;
  size_t refuseAt = NEVER;
  size_t throwing = 0;
  size_t nothrow = 0;
  size_t liveNothrowBytes = 0;
  size_t peakNothrowBytes = 0;  // the most liveNothrowBytes held while armed
  size_t sizes[8] = {};         // the size of each armed nothrow call, by index
};
AllocCounts gAlloc;

// Each block carries a header: malloc's pointer, the size asked for, and
// whether the block is in liveNothrowBytes.
constexpr size_t HEADER = 32;
struct Header {
  void* base;
  size_t size;
  bool counted;
};
static_assert(sizeof(Header) <= HEADER);

void* countedAllocate(const size_t size, size_t align, const bool throwing) {
  bool counted = false;
  if (gAlloc.armed) {
    if (throwing) {
      gAlloc.throwing++;
    } else {
      const size_t index = gAlloc.nothrow++;
      if (index < sizeof(gAlloc.sizes) / sizeof(gAlloc.sizes[0])) gAlloc.sizes[index] = size;
      if (index == gAlloc.refuseAt) return nullptr;
      counted = true;
    }
  }
  if (align < alignof(std::max_align_t)) align = alignof(std::max_align_t);
  void* const base = std::malloc(size + HEADER + align);
  if (!base) {
    if (throwing) std::abort();  // nothing to throw: the firmware has no exceptions
    return nullptr;
  }
  const uintptr_t user =
      (reinterpret_cast<uintptr_t>(base) + HEADER + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
  *reinterpret_cast<Header*>(user - HEADER) = Header{base, size, counted};
  if (counted) {
    gAlloc.liveNothrowBytes += size;
    if (gAlloc.liveNothrowBytes > gAlloc.peakNothrowBytes) gAlloc.peakNothrowBytes = gAlloc.liveNothrowBytes;
  }
  return reinterpret_cast<void*>(user);
}

void countedFree(void* const p) {
  if (!p) return;
  const Header* const h = reinterpret_cast<const Header*>(static_cast<uint8_t*>(p) - HEADER);
  if (h->counted) gAlloc.liveNothrowBytes -= h->size;
  std::free(h->base);
}

}  // namespace

void* operator new(const size_t n) { return countedAllocate(n, 0, true); }
void* operator new[](const size_t n) { return countedAllocate(n, 0, true); }
void* operator new(const size_t n, const std::nothrow_t&) noexcept { return countedAllocate(n, 0, false); }
void* operator new[](const size_t n, const std::nothrow_t&) noexcept { return countedAllocate(n, 0, false); }
void* operator new(const size_t n, const std::align_val_t a) {
  return countedAllocate(n, static_cast<size_t>(a), true);
}
void* operator new[](const size_t n, const std::align_val_t a) {
  return countedAllocate(n, static_cast<size_t>(a), true);
}
void* operator new(const size_t n, const std::align_val_t a, const std::nothrow_t&) noexcept {
  return countedAllocate(n, static_cast<size_t>(a), false);
}
void* operator new[](const size_t n, const std::align_val_t a, const std::nothrow_t&) noexcept {
  return countedAllocate(n, static_cast<size_t>(a), false);
}
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, size_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t) noexcept { countedFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { countedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { countedFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { countedFree(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { countedFree(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { countedFree(p); }

namespace {

// Counts from zero while alive, refusing nothrow call `refuseAt`.
class Armed {
 public:
  explicit Armed(const size_t refuseAt = NEVER) {
    gAlloc.refuseAt = refuseAt;
    gAlloc.throwing = 0;
    gAlloc.nothrow = 0;
    gAlloc.peakNothrowBytes = gAlloc.liveNothrowBytes;
    for (size_t& s : gAlloc.sizes) s = 0;
    gAlloc.armed = true;
  }
  ~Armed() { gAlloc.armed = false; }
  Armed(const Armed&) = delete;
  Armed& operator=(const Armed&) = delete;
};

using ottest::readFixture;
using ottest::scriptOf;

// The shaping buffer, over static storage: shaping itself allocates nothing.
uint8_t gStorage[ot::Buffer::bytesFor(256)];
ot::Buffer gBuffer;

// Shapes a word into gBuffer, decoded into fixed storage. A word of more
// code points than that storage takes is a test failure, not a truncation.
bool shapeInto(const ot::Face& face, const ot::Plan& plan, const char* utf8) {
  constexpr size_t MAX_WORD = 64;
  uint32_t cps[MAX_WORD];
  const size_t count = ottest::decodeUtf8(utf8, cps, MAX_WORD);
  if (count > MAX_WORD) {
    ADD_FAILURE() << "a word of " << count << " code points, past " << MAX_WORD << ": " << utf8;
    return false;
  }
  return ottest::shapeRun(face, plan, cps, static_cast<unsigned>(count), gBuffer);
}

// The shaped run; empty when shaping fails.
std::vector<int32_t> shaped(const ot::Face& face, const ot::Plan& plan, const char* utf8) {
  if (!shapeInto(face, plan, utf8)) return {};
  return ottest::glyphsOf(gBuffer);
}

// The fixture's tables; `asStored` keeps CPac and CPpl, else both are dropped
// (the TrueType route: filters computed, plans built).
bool tablesOf(const std::vector<uint8_t>& font, const bool asStored, ot::FaceTables* out) {
  if (!ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), out)) return false;
  if (!asStored) {
    (*out)[ot::FaceTables::CPAC] = ot::Table();
    (*out)[ot::FaceTables::CPPL] = ot::Table();
  }
  return true;
}

// A fixture's font, its script and one face of it.
struct FixtureFace {
  std::vector<uint8_t> bytes;
  ot::Script script = ot::Script::Devanagari;
  ot::Face face;
};

bool openFixture(const ShapingFixture& fx, const bool asStored, FixtureFace* out) {
  out->bytes = readFixture(fx.layoutFile);
  ot::FaceTables tables;
  return scriptOf(fx.script, &out->script) && tablesOf(out->bytes, asStored, &tables) && out->face.init(tables);
}

bool isEmpty(const ot::Plan& plan) {
  return plan.lookups[ot::GSUB].size() == 0 && plan.lookups[ot::GPOS].size() == 0 &&
         plan.shaper == ot::ShaperKind::Default;
}

const uint32_t* noLanguage() { return ot::languageTagsFor(""); }

}  // namespace

// Face::init, Plan::build on both routes, Plan::load and ot::shape of every
// fixture word: no throwing allocation anywhere, and none at all in shape().
TEST(OtShaperAlloc, NothingThrowingIsAllocated) {
  gBuffer.setStorage(gStorage, sizeof(gStorage));
  size_t words = 0;
  for (const ShapingFixture& fx : kShapingFixtures) {
    ot::Script script = ot::Script::Devanagari;
    ASSERT_TRUE(scriptOf(fx.script, &script)) << fx.script;
    const std::vector<uint8_t> font = readFixture(fx.layoutFile);
    ASSERT_FALSE(font.empty()) << fx.layoutFile;

    size_t throwing = 0;
    size_t inShape = 0;
    unsigned failures = 0;
    {
      const Armed armed;
      ot::FaceTables storedTables, computedTables;
      ot::Face stored, computed;
      failures += !tablesOf(font, true, &storedTables) || !tablesOf(font, false, &computedTables);
      failures += !stored.init(storedTables);
      failures += !computed.init(computedTables);
      for (size_t w = 0; w < fx.count; w++) {
        const ExpectedShaping& word = fx.words[w];
        const uint32_t* const tags = ot::languageTagsFor(word.language);
        ot::Plan builtStored, builtComputed, loaded;
        failures += !builtStored.build(stored, script, tags);
        failures += !builtComputed.build(computed, script, tags);
        failures += !loaded.load(stored, script, tags);
        for (const ot::Plan* plan : {&builtStored, &builtComputed, &loaded}) {
          const ot::Face& face = plan == &builtComputed ? computed : stored;
          const size_t before = gAlloc.nothrow;
          failures += !shapeInto(face, *plan, word.utf8);
          inShape += gAlloc.nothrow - before;
        }
        words++;
      }
      throwing = gAlloc.throwing;
    }
    EXPECT_EQ(failures, 0u) << fx.script;
    EXPECT_EQ(throwing, 0u) << fx.script;
    EXPECT_EQ(inShape, 0u) << fx.script;
  }
  EXPECT_EQ(words, 56u);
  EXPECT_EQ(gAlloc.liveNothrowBytes, 0u);
}

// Plan::build takes six blocks, in order: the builder's scratch, GSUB's and
// GPOS's lists, the lookup digests (deviation 13), GSUB's and GPOS's computed
// subtable filters. A refused scratch, list or digest block fails the build
// with the plan empty and nothing held; a refused filter block leaves its
// table's subtables unfiltered, which shapes the same.
TEST(OtShaperAlloc, EachRefusedBlockFailsTheBuildCleanly) {
  gBuffer.setStorage(gStorage, sizeof(gStorage));
  for (const ShapingFixture& fx : kShapingFixtures) {
    FixtureFace font;
    ASSERT_TRUE(openFixture(fx, false, &font)) << fx.script;

    std::vector<std::vector<int32_t>> expected;
    size_t blocks = 0;
    size_t fullBytes = 0;
    int filtered[2] = {};  // the tables with lookups, in the order their filter blocks come
    size_t filteredCount = 0;
    {
      ot::Plan plan;
      {
        const Armed armed;
        ASSERT_TRUE(plan.build(font.face, font.script, noLanguage())) << fx.script;
        blocks = gAlloc.nothrow;
      }
      fullBytes = plan.memoryBytes();
      for (const int t : {ot::GSUB, ot::GPOS}) {
        if (plan.lookups[t].size() > 0) filtered[filteredCount++] = t;
      }
      // A filter block per table with lookups. Every fixture has lookups in
      // both tables, so k below reaches both filter blocks.
      ASSERT_EQ(filteredCount, 2u) << fx.script;
      ASSERT_EQ(blocks, 4 + filteredCount) << fx.script;
      for (size_t i = 0; i < filteredCount; i++) {
        EXPECT_TRUE(plan.settings(filtered[i], 0).hasDigest) << fx.script << " table " << filtered[i];
        EXPECT_NE(plan.settings(filtered[i], 0).subtableDigests, nullptr) << fx.script << " table " << filtered[i];
      }
      for (size_t w = 0; w < fx.count; w++) {
        expected.push_back(shaped(font.face, plan, fx.words[w].utf8));
        ASSERT_FALSE(expected.back().empty()) << fx.script << " word " << w;
      }
    }
    EXPECT_EQ(gAlloc.liveNothrowBytes, 0u) << fx.script;

    for (size_t k = 0; k < blocks; k++) {
      {
        ot::Plan plan;
        bool built = false;
        {
          const Armed armed(k);
          built = plan.build(font.face, font.script, noLanguage());
        }
        if (k < 4) {
          EXPECT_FALSE(built) << fx.script << " refused " << k;
          EXPECT_TRUE(isEmpty(plan)) << fx.script << " refused " << k;
          EXPECT_EQ(plan.memoryBytes(), sizeof(ot::Plan)) << fx.script << " refused " << k;
          EXPECT_EQ(gAlloc.liveNothrowBytes, 0u) << fx.script << " refused " << k;
        } else {
          EXPECT_TRUE(built) << fx.script << " refused " << k;
          EXPECT_LT(plan.memoryBytes(), fullBytes) << fx.script << " refused " << k;
          // The refused table's subtables run unfiltered; the other keeps its
          // filters, and both keep their lookup digests.
          const int refusedTable = filtered[k - 4];
          for (size_t i = 0; i < filteredCount; i++) {
            const int t = filtered[i];
            EXPECT_TRUE(plan.settings(t, 0).hasDigest) << fx.script << " refused " << k << " table " << t;
            EXPECT_EQ(plan.settings(t, 0).subtableDigests != nullptr, t != refusedTable)
                << fx.script << " refused " << k << " table " << t;
          }
          for (size_t w = 0; w < fx.count; w++) {
            EXPECT_EQ(shaped(font.face, plan, fx.words[w].utf8), expected[w])
                << fx.script << " refused " << k << " word " << w;
          }
        }
      }
      EXPECT_EQ(gAlloc.liveNothrowBytes, 0u) << fx.script << " refused " << k;
    }
  }
}

// The computed route allocates the scratch, GSUB's and GPOS's lists, then the
// lookup digests and the two filters; the scratch is freed before the
// digests, so the peak is the lists plus the larger of the scratch and the
// digests with the two filters. With CPac in the font there are no filters,
// and the peak is the lists plus the larger of the scratch and the digests.
TEST(OtShaperAlloc, TheComputedRouteFreesTheScratchBeforeTheFilters) {
  for (const ShapingFixture& fx : kShapingFixtures) {
    for (const bool asStored : {false, true}) {
      FixtureFace font;
      ASSERT_TRUE(openFixture(fx, asStored, &font)) << fx.script;
      ot::Plan plan;
      size_t calls = 0;
      size_t peak = 0;
      size_t sizes[8] = {};
      {
        const Armed armed;
        ASSERT_TRUE(plan.build(font.face, font.script, noLanguage())) << fx.script;
        calls = gAlloc.nothrow;
        peak = gAlloc.peakNothrowBytes;
        std::copy(std::begin(gAlloc.sizes), std::end(gAlloc.sizes), std::begin(sizes));
      }
      if (asStored) {
        EXPECT_EQ(calls, 4u) << fx.script;
        EXPECT_EQ(peak, sizes[1] + sizes[2] + std::max(sizes[0], sizes[3])) << fx.script;
      } else {
        EXPECT_EQ(calls, 6u) << fx.script;
        EXPECT_EQ(peak, sizes[1] + sizes[2] + std::max(sizes[0], sizes[3] + sizes[4] + sizes[5])) << fx.script;
      }
    }
  }
}

// Plan::load takes GSUB's and GPOS's lists and the lookup digests; with CPac
// in the font, no filters. Any of them refused fails the load with the plan
// empty.
TEST(OtShaperAlloc, EachRefusedBlockFailsTheLoadCleanly) {
  for (const ShapingFixture& fx : kShapingFixtures) {
    FixtureFace font;
    ASSERT_TRUE(openFixture(fx, true, &font)) << fx.script;
    {
      ot::Plan plan;
      bool loaded = false;
      size_t blocks = 0;
      {
        const Armed armed;
        loaded = plan.load(font.face, font.script, noLanguage());
        blocks = gAlloc.nothrow;
      }
      EXPECT_TRUE(loaded) << fx.script;
      EXPECT_EQ(blocks, 3u) << fx.script;
    }
    for (size_t k = 0; k < 3; k++) {
      {
        ot::Plan plan;
        bool loaded = false;
        {
          const Armed armed(k);
          loaded = plan.load(font.face, font.script, noLanguage());
        }
        EXPECT_FALSE(loaded) << fx.script << " refused " << k;
        EXPECT_TRUE(isEmpty(plan)) << fx.script << " refused " << k;
        EXPECT_EQ(gAlloc.liveNothrowBytes, 0u) << fx.script << " refused " << k;
      }
      EXPECT_EQ(gAlloc.liveNothrowBytes, 0u) << fx.script << " refused " << k;
    }
  }
}

// appendSubtableDigests() into a FixedVector with room for one record, of a
// lookup with more subtables: one record, the flag cleared, and nothing
// written past the room.
TEST(OtShaperAlloc, SubtableDigestsStopAtTheFirstRecordThatDoesNotFit) {
  const ShapingFixture& fx = kShapingFixtures[0];
  FixtureFace font;
  ASSERT_TRUE(openFixture(fx, true, &font)) << fx.script;
  uint32_t lookup = 0;
  while (lookup < font.face.lookupCount(ot::GSUB) && ot::Face::subtableCount(font.face.lookup(ot::GSUB, lookup)) < 2) {
    lookup++;
  }
  ASSERT_LT(lookup, font.face.lookupCount(ot::GSUB)) << "no GSUB lookup with two subtables";

  constexpr uint8_t SENTINEL = 0xA5;
  uint8_t room[2 * ot::DIGEST_BYTES];
  std::memset(room, SENTINEL, sizeof(room));
  bool fits = true;
  ot::FixedVector<uint8_t> out;
  out.bind(room, ot::DIGEST_BYTES, &fits);
  ot::appendSubtableDigests(font.face, ot::GSUB, lookup, out);
  EXPECT_EQ(out.size(), ot::DIGEST_BYTES);
  EXPECT_FALSE(fits);
  for (size_t i = ot::DIGEST_BYTES; i < sizeof(room); i++) EXPECT_EQ(room[i], SENTINEL) << i;

  // The record written is the lookup's first subtable's, as with room for all.
  std::vector<uint8_t> all(ot::Face::subtableCount(font.face.lookup(ot::GSUB, lookup)) * ot::DIGEST_BYTES);
  ot::FixedVector<uint8_t> whole;
  whole.bind(all.data(), all.size());
  ot::appendSubtableDigests(font.face, ot::GSUB, lookup, whole);
  ASSERT_EQ(whole.size(), all.size());
  EXPECT_EQ(0, std::memcmp(room, all.data(), ot::DIGEST_BYTES));
}

// A plan whose build failed builds again: the same Plan object, once the
// heap allows, shapes as a fresh one.
TEST(OtShaperAlloc, AFailedBuildCanBeRetried) {
  gBuffer.setStorage(gStorage, sizeof(gStorage));
  for (const ShapingFixture& fx : kShapingFixtures) {
    FixtureFace font;
    ASSERT_TRUE(openFixture(fx, false, &font)) << fx.script;
    ot::Plan fresh;
    ASSERT_TRUE(fresh.build(font.face, font.script, noLanguage())) << fx.script;

    ot::Plan plan;
    bool built = true;
    {
      const Armed armed(1);  // GSUB's lists
      built = plan.build(font.face, font.script, noLanguage());
    }
    EXPECT_FALSE(built) << fx.script;
    ASSERT_TRUE(plan.build(font.face, font.script, noLanguage())) << fx.script;
    for (size_t w = 0; w < fx.count; w++) {
      const std::vector<int32_t> expected = shaped(font.face, fresh, fx.words[w].utf8);
      ASSERT_FALSE(expected.empty()) << fx.script << " word " << w;
      EXPECT_EQ(shaped(font.face, plan, fx.words[w].utf8), expected) << fx.script << " word " << w;
    }
  }
}
