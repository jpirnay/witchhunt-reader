#pragma once

#include <cstddef>  // witchhunt: deviation 9
#include <string>

// Indic syllable reordering for fonts rendered without OpenType shaping.
//
// The glyph pipeline draws codepoints left to right, so logical-order text
// puts pre-base vowel signs after their consonant (कि renders as क + ि
// instead of ि + क). This rewrites each syllable into visual order:
//   * pre-base vowel signs (ि, ি, ੇ, ெ, െ, ෙ, ...) move in front of the
//     consonant cluster,
//   * two-part vowels split around it (ো -> ে + cluster + া),
//   * nukta sequences use the precomposed letters (ड़, য়, ਜ਼, ...).
// A syllable-initial reph (ra + virama before a consonant) stays in front of
// the cluster. Conjuncts are not formed; they still render with a visible
// virama. The per-script rules are in IndicReorder.cpp.
//
// Returns true and writes the reordered text to `out` when anything changed.
// Returns false, leaving `out` unspecified, when `text` needs no reordering
// (the common case: no Indic block bytes, found by a byte scan).
bool indicReorderForDisplay(const char* text, std::string& out);

// witchhunt: deviation 9 - the same form into `out` (`room` bytes, NUL-terminated), allocating nothing: true when the
// form differs from the text and fits; false, `out` unspecified, otherwise. Room for 3 * len + 1 bytes always holds
// the form: every vowel sign is 3 bytes and becomes at most three 3-byte code points (Sinhala U+0DDD: one before the
// cluster, two after), and any other code point is copied once. `text` is `len` bytes and need not be NUL-terminated;
// `written` is set (the form's length, without the NUL) only when true.
bool indicReorderForDisplay(const char* text, size_t len, char* out, size_t room, size_t* written);
