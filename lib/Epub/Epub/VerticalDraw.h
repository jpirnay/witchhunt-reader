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
// x the pen along the column, y the line's top. Allocates nothing; stack under 100 B.
void drawTextVertical(const GfxRenderer& renderer, int fontId, int x, int y, const char* text,
                      EpdFontFamily::Style style, float scale);
