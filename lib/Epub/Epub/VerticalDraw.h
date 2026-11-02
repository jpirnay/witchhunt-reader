#pragma once

#include <EpdFontFamily.h>

class GfxRenderer;

// The em of vertical drawing for fontId at scale: the advance of 一, else the ascender (a font without CJK).
int columnEm(const GfxRenderer& renderer, int fontId, EpdFontFamily::Style style, float scale);

// A column's centre line and its over edge (where the ruby band, emphasis and the side line lie: right of the column),
// from the line's baseline: the ideographic em box sits 880/1000 above the baseline and 120 below.
constexpr int columnAxis(const int baseline, const int em) { return baseline - (em * 38 + 50) / 100; }
constexpr int columnOverEdge(const int baseline, const int em) { return baseline - (em * 88 + 50) / 100; }

// How far `text` runs down a column at `scale` (spec 5.4): an upright piece one em (the advance of 一; its own advance
// in a font without 一), a sideways run its horizontal advance. Summed unscaled, rounded once (int(raw * scale + 0.5)),
// as ParsedText rounds a word.
int verticalTextAdvance(const GfxRenderer& renderer, int fontId, const char* text, EpdFontFamily::Style style,
                        float scale);

// Draws `text` down a column. Called in the turned frame (Page::render's scope) with what drawText would get there:
// x the pen along the column, y the line's top. Allocates nothing; two char[kMaxColumnRunBytes + 1] buffers on the
// stack, within the 256 B constraint.
//
// An upright 、。 or small kana is placed in its column as a vertical font does (VerticalOrientation.h): a mark
// reflected into the top-right corner of its cell, or halfway there when `centredMarks` (Traditional Chinese), a small
// kana moved up and right. A font without the glyph draws it unplaced.
void drawTextVertical(const GfxRenderer& renderer, int fontId, int x, int y, const char* text,
                      EpdFontFamily::Style style, float scale, bool centredMarks);

// Where the ink of the upright code point `cp` lands along the column as drawn (at scale 1), from the cell's leading
// edge (the pen): `inkStart` and `inkSize`; `cell` is the em. False for a sideways code point or when the font has no
// glyph.
bool columnInk(const GfxRenderer& renderer, int fontId, uint32_t cp, EpdFontFamily::Style style, bool centredMarks,
               int& inkStart, int& inkSize, int& cell);

// Draws `text` upright as one block in a column cell (tate-chu-yoko), called as drawTextVertical is: x the pen along
// the column, y the line's top. A run wider than the em is shrunk to it; the block is centred across the column and,
// along it, the em box it fills is centred in the cell [pen, pen + em]. A scan pass draws it plainly. Allocates
// nothing.
void drawCombinedVertical(const GfxRenderer& renderer, int fontId, int x, int y, const char* text,
                          EpdFontFamily::Style style, float scale);
