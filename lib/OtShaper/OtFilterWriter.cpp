// Ours (witchhunt: deviation 4), not upstream's. See OtFilterWriter.h.

#include "OtFilterWriter.h"

#include <cstring>

#include "OtLayout.h"

namespace ot {

namespace {

constexpr uint32_t HEADER_BYTES = 16;
constexpr uint32_t LOOKUP_BYTES = DIGEST_BYTES + 4;  // digest + first-subtable index
constexpr uint8_t STEPS = 1 + 3 * 2;                 // header, then three per table

void putU16(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

void putU32(uint8_t* p, const uint32_t v) {
  putU16(p, v >> 16);
  putU16(p + 2, v & 0xFFFF);
}

}  // namespace

FilterTableWriter::FilterTableWriter(const Face& face) : face_(face) {
  for (int t = GSUB; t <= GPOS; t++) {
    lookups_[t] = face.lookupCount(t);
    if (lookups_[t] > kMaxLookups) return;
    for (uint32_t i = 0; i < lookups_[t]; i++) {                // witchhunt: deviation 4
      subtables_[t] += Face::subtableCount(face.lookup(t, i));  // witchhunt: deviation 4
      if (subtables_[t] > kMaxSubtables) return;                // witchhunt: deviation 4
    }
  }
  if (lookups_[GSUB] + lookups_[GPOS] == 0) return;
  const uint64_t size =
      HEADER_BYTES + static_cast<uint64_t>(LOOKUP_BYTES) * (lookups_[GSUB] + lookups_[GPOS]) +
      static_cast<uint64_t>(DIGEST_BYTES) * (static_cast<uint64_t>(subtables_[GSUB]) + subtables_[GPOS]);
  if (size <= UINT32_MAX) total_ = static_cast<uint32_t>(size);
}

uint32_t FilterTableWriter::write(uint8_t* out, const uint32_t room) {
  uint32_t n = 0;
  while (n < room && written_ + n < total_) {
    if (recPos_ == recLen_ && !nextRecord()) break;
    const uint32_t left = static_cast<uint32_t>(recLen_ - recPos_);
    const uint32_t take = room - n < left ? room - n : left;
    std::memcpy(out + n, rec_ + recPos_, take);
    recPos_ = static_cast<uint8_t>(recPos_ + take);
    n += take;
  }
  written_ += n;
  return n;
}

bool FilterTableWriter::nextRecord() {
  recPos_ = 0;
  recLen_ = 0;
  if (step_ == 0) {
    putU16(rec_, 1);  // version
    putU16(rec_ + 2, 0);
    putU16(rec_ + 4, lookups_[GSUB]);
    putU16(rec_ + 6, lookups_[GPOS]);
    putU32(rec_ + 8, subtables_[GSUB]);
    putU32(rec_ + 12, subtables_[GPOS]);
    recLen_ = HEADER_BYTES;
    step_ = 1;
    return true;
  }
  while (step_ < STEPS) {
    const int table = (step_ - 1) / 3;
    const unsigned part = (step_ - 1) % 3;  // 0 lookup digests, 1 first-subtable indices, 2 subtable digests
    if (index_ >= (part == 2 ? subtables_[table] : lookups_[table])) {
      step_++;
      index_ = lookup_ = within_ = running_ = 0;
      continue;
    }
    if (part == 0) {
      writeDigest(lookupDigest(face_, table, index_), rec_);
      recLen_ = DIGEST_BYTES;
    } else if (part == 1) {
      putU32(rec_, running_);
      running_ += Face::subtableCount(face_.lookup(table, index_));
      recLen_ = 4;
    } else {
      // The lookup holding subtable `index_`: lookups without subtables (dropped ones too) are passed over.
      while (lookup_ < lookups_[table] && within_ >= Face::subtableCount(face_.lookup(table, lookup_))) {
        lookup_++;
        within_ = 0;
      }
      if (lookup_ >= lookups_[table]) return false;
      writeDigest(subtableDigest(face_, table, lookup_, static_cast<uint16_t>(within_)), rec_);
      within_++;
      recLen_ = DIGEST_BYTES;
    }
    index_++;
    return true;
  }
  return false;
}

}  // namespace ot
