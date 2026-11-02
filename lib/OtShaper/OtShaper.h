#pragma once

#include <cstdint>

#include "OtBuffer.h"
#include "OtFace.h"
#include "OtLayout.h"
#include "OtPlan.h"

// OpenType shaping of the Indic scripts (Devanagari, Bengali, Gurmukhi,
// Gujarati, Oriya, Tamil, Telugu, Kannada, Malayalam, Sinhala) without
// HarfBuzz.
//
// The font's own GSUB/GPOS tables drive every substitution and position,
// read in place from the font data; this code supplies what the tables
// cannot: syllable segmentation, the Indic and Universal Shaping Engine
// reordering rules, normalization and the feature/stage order. All of it
// follows HarfBuzz 14.5.0, and the host tests check the output against
// HarfBuzz glyph for glyph.
namespace ot {

// Shapes one run of a single script (plus the joiners, dandas and dotted
// circle any Indic run may contain) into buffer.info[].codepoint glyph IDs
// and buffer.pos[] advances/offsets in scaled units. Returns false when the
// run could not be shaped: the buffer's storage cannot take it (maxRunLength())  // witchhunt: deviation 1
// (buffer.successful stays true), or the font's lookups failed the run by
// growing it past its limit, nesting too deep or running too long.
//
// `context` marks the first `before` and last `after` codepoints as context:
// they shape with the rest, so a piece of a longer run sees its neighbours
// (word-start 'init', lookahead rules, kerning), but their glyphs are dropped
// from the result.
bool shape(const Face& face, const Scale& scale, const Plan& plan, const uint32_t* codepoints, unsigned count,
           Buffer& buffer, const RunContext& context = {});

// witchhunt: deviation 1 - the longest run shape() can take in the buffer's
// storage (setStorage()); a longer one fails with buffer.successful still true.
unsigned maxRunLength(const Buffer& buffer);

// The OpenType language systems to try for a BCP 47 language tag (the
// book's dc:language), zero-terminated. Unknown languages select the
// script's default language system.
const uint32_t* languageTagsFor(const char* bcp47);

}  // namespace ot
