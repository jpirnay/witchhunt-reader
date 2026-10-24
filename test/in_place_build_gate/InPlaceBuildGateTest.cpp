// InPlaceBuildGate: whether a section build may run in place (resident) or borrows the secondary buffer.
#include <gtest/gtest.h>

#include <cstdint>

#include "activities/reader/InPlaceBuildGate.h"

namespace {

using InPlaceBuildGate::Admission;
using InPlaceBuildGate::admitsResident;
using InPlaceBuildGate::kContigSlackBytes;
using InPlaceBuildGate::kMaxRingBytes;
using InPlaceBuildGate::Request;

constexpr size_t kSmallChapter = 1539;    // the 1.5 KB Simplified Chinese chapter: its ring stays under the bases
constexpr size_t kLargeChapter = 583991;  // a whole book in one entry: the ring is capped at 32 KB
constexpr uint32_t kPlenty = 120 * 1024;  // more than the X4's reading heap ever has

Request request(const bool css, const size_t inflated, const uint32_t freeHeap, const uint32_t contigHeap,
                const bool cjk = false, const bool trueType = false) {
  Request r;
  r.embeddedStyle = css;
  r.inflatedSize = inflated;
  r.cjkRepertoire = cjk;
  r.trueTypeSet = trueType;
  r.freeHeap = freeHeap;
  r.contigHeap = contigHeap;
  return r;
}

// The floors a Latin book has on master, from the build's own constants.
struct Floors {
  bool css;
  size_t inflated;
  uint32_t freeFloor;
  uint32_t contigFloor;
};

const Floors kFloors[] = {
    {false, kSmallChapter, IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES,
     IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES - kContigSlackBytes},
    {true, kSmallChapter, IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES,
     IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES - kContigSlackBytes},
    {false, kLargeChapter, IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES + kMaxRingBytes,
     kMaxRingBytes + 8 * 1024 - kContigSlackBytes},
    {true, kLargeChapter, IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES + kMaxRingBytes,
     kMaxRingBytes + 8 * 1024 - kContigSlackBytes},
};

// Admitted exactly at both floors and refused one byte under either, as on master.
void expectLatinFloors(const bool cjk, const bool trueType) {
  for (const Floors& f : kFloors) {
    SCOPED_TRACE(testing::Message() << "css=" << f.css << " inflated=" << f.inflated << " cjk=" << cjk
                                    << " trueType=" << trueType);
    const Admission at = admitsResident(request(f.css, f.inflated, f.freeFloor, f.contigFloor, cjk, trueType));
    EXPECT_TRUE(at.resident);
    EXPECT_FALSE(at.cjkTrueType);
    EXPECT_EQ(f.freeFloor, at.freeFloor);
    EXPECT_EQ(f.contigFloor, at.contigFloor);
    EXPECT_FALSE(admitsResident(request(f.css, f.inflated, f.freeFloor - 1, kPlenty, cjk, trueType)).resident);
    EXPECT_FALSE(admitsResident(request(f.css, f.inflated, kPlenty, f.contigFloor - 1, cjk, trueType)).resident);
    EXPECT_TRUE(admitsResident(request(f.css, f.inflated, kPlenty, kPlenty, cjk, trueType)).resident);
  }
}

TEST(InPlaceBuildGate, ALatinBookIsAdmittedAtTheFloorsAndRefusedUnderThem) { expectLatinFloors(false, false); }

TEST(InPlaceBuildGate, TheRingRaisesTheFloorsOnlyPastTheBases) {
  // A small chapter's ring hides under the parse bases; a large one's sets both floors.
  EXPECT_EQ(kSmallChapter, admitsResident(request(true, kSmallChapter, 0, 0)).ringBytes);
  EXPECT_EQ(kMaxRingBytes, admitsResident(request(true, kLargeChapter, 0, 0)).ringBytes);
  EXPECT_EQ(0u, admitsResident(request(false, 0, 0, 0)).ringBytes);  // size unknown
}

TEST(InPlaceBuildGate, ACjkBookInATrueTypeSetNeverBuildsInPlace) {
  for (const bool css : {false, true}) {
    for (const size_t inflated : {kSmallChapter, size_t{9602}, kLargeChapter}) {
      SCOPED_TRACE(testing::Message() << "css=" << css << " inflated=" << inflated);
      // The X4's measured Japanese chapter (2026-10-23), a heap it never has, and all of it.
      for (const uint32_t heap : {uint32_t{68368}, kPlenty, UINT32_MAX}) {
        const Admission a = admitsResident(request(css, inflated, heap, heap, /*cjk=*/true, /*trueType=*/true));
        EXPECT_FALSE(a.resident) << heap;
        EXPECT_TRUE(a.cjkTrueType) << heap;
      }
    }
  }
}

TEST(InPlaceBuildGate, ACjkBookInABuiltInOrCpfontFamilyKeepsTheFloors) { expectLatinFloors(true, false); }

TEST(InPlaceBuildGate, ALatinBookInATrueTypeSetKeepsTheFloors) { expectLatinFloors(false, true); }

}  // namespace
