#pragma once

// Geometry for a paged grid of book covers (the recent-books cover view).
//
// Nothing here is a fixed column or row count: both fall out of the space the caller hands in, so
// a higher-resolution panel yields MORE cells of the same comfortable size rather than the same
// few cells blown up. The caller supplies the content area it has after the header and button
// hints, plus the largest cover box worth drawing -- the full-size thumbnail's, since the draw path
// never upscales and a bigger box would only add empty frame.
//
// Rows come first, at full size. A page that would leave at least half a row empty takes one more
// row instead, its cells shrunk to fit (on a 540x960 panel: three rows of ~210 px covers rather
// than two rows of 240 and a fifth of the screen blank). Columns then follow from how wide a
// cover is at that height. The thumbnail is made at the cell's size (thumbWidth x thumbHeight), so
// a cover always draws 1:1: a dithered 1-bit image must never be resampled.
//
// Pure arithmetic: no renderer, no theme, no storage, so it is exercised on the host.
namespace CoverGridLayout {

// Visual constants of the grid itself. The label block holds two small-font lines (title, then
// author and series), and a third where the panel has the height to spare (Layout::labelLines).
inline constexpr int kMargin = 10;
inline constexpr int kLabelHeight = 36;
inline constexpr int kLabelLineHeight = 17;  // one more small-font line
inline constexpr int kMinCellHeight = 96;

// The full-size grid thumbnail's box. The cover is FITTED inside it (kThumbCrop false), whole: a
// 2:3 cover comes out about 160x240, a square one 196x196. Filling the box and cropping, as the
// carousel does, cut a third off the top and bottom of every ordinary cover.
//
// A grid makes its thumbnail at the size its cells come out at (Layout::thumbWidth x thumbHeight),
// never larger than this: a dithered 1-bit image must never be resampled. On the X3 and X4 that is
// this box exactly, so the finished-book screen, which always uses this box, shares the grid's file
// there. Both numbers are in the cache filename, so changing either regenerates every grid cover.
inline constexpr int kThumbWidth = 196;
inline constexpr int kThumbHeight = 240;
inline constexpr bool kThumbCrop = false;
// Largest cover box the grid draws: the full-size thumbnail plus its 1 px frame on each side.
// Raising the height means raising kThumbHeight (every cover regenerates), and is bounded by what
// the label block leaves free.
inline constexpr int kMaxCellHeight = kThumbHeight + 2;
inline constexpr int kMaxCellWidth = kThumbWidth + 2;

struct Input {
  int contentWidth = 0;   // width available to the grid
  int contentHeight = 0;  // height available below the header
  int bottomReserve = 0;  // strip at the bottom left free for hints / scroll arrows
  int maxCellHeight = 0;  // tallest cover box worth drawing (the full-size thumbnail's height + frame)
  int maxCellWidth = 0;   // widest cover box worth drawing (the full-size thumbnail's width + frame); 0: no cap
};

struct Layout {
  int cols = 1;
  int rows = 1;         // rows per page
  int cellWidth = 0;    // cover box width, 1 px frame included
  int cellHeight = 0;   // cover box height, 1 px frame included
  int rowStride = 0;    // cellHeight + label block + margin, and an equal share of the spare height
  int labelLines = 2;   // 3 where each row's share of the spare height holds another line
  int labelHeight = 0;  // the label block: kLabelHeight, plus kLabelLineHeight for a third line
  int labelWidth = 0;   // text width available under a cover
  int thumbWidth = 0;   // the stored thumbnail's box: what the cover is fitted into, drawn 1:1
  int thumbHeight = 0;  //   (the cell less its frame, no wider than the full-size thumbnail)
};

Layout compute(const Input& in);

// The cell a row step lands on, wrapping at both ends like the lists do: Down from the last row goes
// to the same column of the first, Up from the first row to the same column of the last row that
// has one. A step down into a partial last row from a column it does not reach lands on its last
// cell. `count` items, `cols` per row; pure arithmetic, exercised on the host.
int rowBelow(int index, int count, int cols);
int rowAbove(int index, int count, int cols);

// Which cell a point falls in, as an absolute item index, or -1 for a miss.
//
// The inverse of the caller's cell placement, and deliberately expressed in the same terms so
// the two cannot drift: `originX`/`originY` are the grid's top-left BEFORE the margin (i.e. the
// content rect's x and the content top, exactly what the render passes), `pageStartRow` is the
// first row on the visible page, and `itemCount` bounds the last partial row.
//
// The tappable cell is the cover box plus the label block under it -- what a reader sees as one
// entry -- but not the kMargin gutter after it, so a tap between two covers is a miss rather
// than a coin flip. Pure arithmetic, exercised on the host.
int hitTest(const Layout& l, int originX, int originY, int pageStartRow, int itemCount, int px, int py);

}  // namespace CoverGridLayout
