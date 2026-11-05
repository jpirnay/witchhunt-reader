#pragma once

// Ours (witchhunt: deviation 4), not upstream's.
// Ported from crosspoint-reader PR #3787 (SSafayet / @SSafayet): the `CPac` format and `lookup_filters()` in
// `lib/EpdFont/scripts/shaping_blob.py`. Here in C++ over an `ot::Face`, written in pieces at install, its counts as
// `Face::lookup()` reads the lookups (witchhunt: deviation 4).

#include <cstdint>

#include "OtBuffer.h"
#include "OtFace.h"
#include "OtPlan.h"

namespace ot {

// The CPac table (crosspoint-reader #3787's docs/file-formats.md) for a face's GSUB and GPOS: what Face::init()
// validates and Plan::settings() reads. Written in order, a piece at a time, so an install streams it into flash.
// Allocates nothing: the state is counters and the one record being written. The face must outlive the writer.
class FilterTableWriter {
 public:
  // Lookups per table past this get no table at all (the probe stops counting there too).
  static constexpr uint32_t kMaxLookups = MAX_PLANNED_LOOKUPS;  // witchhunt: deviation 4
  // Subtables per table (the sum of its lookups' counts) past this get no table at all either: the lookup cap alone
  // lets 4,096 lookups each declare 65,535 subtables, sharing one offset, for gigabytes of records.
  static constexpr uint32_t kMaxSubtables = 16384;  // witchhunt: deviation 4

  explicit FilterTableWriter(const Face& face);

  // The table's bytes: 16 + 28 a lookup + 24 a subtable; 0 with no lookups, or past kMaxLookups or kMaxSubtables in a
  // table (witchhunt: deviation 4).
  uint32_t bytes() const { return total_; }

  // Writes the next min(room, bytes left) bytes to `out` and returns how many; 0 once all are written.
  uint32_t write(uint8_t* out, uint32_t room);

 private:
  // Fills rec_ with the next record; false when there is none.
  bool nextRecord();

  const Face& face_;
  uint32_t lookups_[2] = {0, 0};
  uint32_t subtables_[2] = {0, 0};
  uint32_t total_ = 0;
  uint32_t written_ = 0;

  // The record being written: the header, a lookup digest, a first-subtable index or a subtable digest.
  uint8_t rec_[DIGEST_BYTES] = {};
  uint8_t recLen_ = 0;
  uint8_t recPos_ = 0;

  // Where the next record comes from: step 0 is the header, then per table its lookup digests, first-subtable
  // indices and subtable digests (steps 1-6).
  uint8_t step_ = 0;
  uint32_t index_ = 0;    // records done in this step
  uint32_t lookup_ = 0;   // subtable steps: the lookup `within_` counts in
  uint32_t within_ = 0;   // subtable steps: subtables of `lookup_` done
  uint32_t running_ = 0;  // index steps: subtables of the lookups before this one
};

}  // namespace ot
