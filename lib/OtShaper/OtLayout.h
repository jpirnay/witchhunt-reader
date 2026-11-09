#pragma once

#include <cstdint>

#include "OtBuffer.h"
#include "OtFace.h"
#include "OtFixedVector.h"  // witchhunt: deviation 1 - in place of <vector>

// Applying GSUB and GPOS lookups (see OtLayoutInternal.h for how the work is
// split across files).
namespace ot {

// witchhunt: deviation 18 - subtables a group mask covers.
constexpr unsigned SUBTABLE_GROUP = 8;

// Largest value a feature can carry (HB_OT_MAP_MAX_VALUE); the 'rand'
// feature uses it to ask for a random alternate.
constexpr unsigned MAX_FEATURE_VALUE = 255;

// Per-lookup settings from the feature map.
struct LookupSettings {
  // Glyphs the lookup can start at (lookupDigest()); lets application skip
  // the others without reading the lookup's subtables.
  // witchhunt: deviation 13 - no hasDigest flag: every plan holds every
  // planned lookup's digest, so Plan::settings() always sets it. The default,
  // every glyph, is for a call without a plan: it filters nothing.
  Digest digest{{~0ull, ~0ull, ~0ull}};
  // Per subtable, the glyphs it can start at: DIGEST_BYTES-byte records.
  // nullptr: subtables are not filtered.
  const uint8_t* subtableDigests = nullptr;
  // witchhunt: deviation 18 - per group of SUBTABLE_GROUP subtables, the low masks (digestRecordLowMask()) of their
  // records ORed, 8 bytes each (little-endian, unaligned): a group whose mask cannot hold a glyph is skipped without
  // reading its records. nullptr: no groups (the lookup has SUBTABLE_GROUP subtables or fewer, or no records).
  const uint8_t* subtableGroups = nullptr;
  uint32_t mask = 0;
  bool autoZwnj = true;
  bool autoZwj = true;
  bool random = false;
  bool perSyllable = false;
};

// Union of the coverage of every subtable of a lookup: the glyphs it can
// start at.
Digest lookupDigest(const Face& face, int table, uint32_t lookupIndex);
// Appends the same digest for each subtable of the lookup, as records.
// witchhunt: deviation 1 - into a FixedVector: it stops at the first record
// that does not fit, and writes nothing past `out`'s size.
void appendSubtableDigests(const Face& face, int table, uint32_t lookupIndex, FixedVector<uint8_t>& out);

// witchhunt: deviation 4 - the glyphs subtable `subtable` of `lookup` (as
// Face::lookup() gives it) can start at: a record of appendSubtableDigests().
// The caller reads the lookup once for all its subtables: Face::lookup()
// checks every subtable of an Extension lookup.
Digest subtableDigest(int table, const Table& lookup, uint16_t subtable);

// Applies one GSUB or GPOS lookup across the buffer. GPOS requires buffer.pos
// to be sized.
void applyLookup(const Face& face, const Scale& scale, Buffer& buffer, int table, uint32_t lookupIndex,
                 const LookupSettings& settings);

// Whether GSUB lookup `lookupIndex` would substitute `glyphs` (HarfBuzz's
// hb_ot_layout_lookup_would_substitute). `digest` is lookupDigest().
bool wouldSubstitute(const Face& face, uint32_t lookupIndex, const Digest& digest, const uint32_t* glyphs,
                     unsigned count, bool zeroContext);

// Joins mark and cursive attachment offsets once GPOS is done, in the
// buffer's direction (witchhunt: deviation 11 - buffer.backward).
void propagateAttachmentOffsets(Buffer& buffer);

}  // namespace ot
