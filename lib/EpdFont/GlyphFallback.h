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
// The set is deliberately narrow, and driven by what real dictionaries render:
// the phonetic alphabet, the arrows and math operators they use as structural
// markers, a few bullet shapes, and the letters whose block this build does not
// ship.
//
// Latin-1 and Latin Extended-A accented letters are NOT listed: those blocks are
// shipped in full, so a font reaching here for one of them cannot render the
// surrounding word either, and stripping the accent would change the word rather
// than rescue it. Latin Extended Additional IS listed, because only its
// Vietnamese subrange is shipped -- letters from the rest of it genuinely arrive
// here, and their base letter beats a box.
uint32_t fallbackGlyphCodepoint(uint32_t cp);
