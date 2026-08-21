#pragma once

#include <cstdint>

// Last-resort visual stand-in for a codepoint no font in the build has a glyph
// for. Returns `cp` unchanged when there is no sensible stand-in.
//
// The alternative, and what happened before this existed, is U+FFFD -- a box.
// A box carries nothing: a dictionary's pronunciation line came out as a row of
// identical rectangles, which is strictly less readable than an approximation.
// So where a codepoint has a close Latin or ASCII relative that every font
// carries, draw that instead.
//
// This is a LEGIBILITY fallback and not a claim of equivalence. It only ever
// applies after the real glyph has been looked up and found absent, so a font
// that has the character always wins. Substitutions are chosen for shape first
// (a small-capital I stands in as "I", a turned v as "v"), because the reader
// is being shown a shape, not being told a fact.
//
// The set is deliberately narrow: the phonetic alphabet, because dictionaries
// are full of it and most fonts are not, plus a couple of bullet shapes. Latin
// letters with diacritics are NOT listed -- fonts that carry the base letter
// almost always carry those too, and stripping an accent changes a word.
uint32_t fallbackGlyphCodepoint(uint32_t cp);
