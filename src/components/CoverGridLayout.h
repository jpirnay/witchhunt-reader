#pragma once

// Geometry for a paged grid of book covers (the recent-books cover view).
//
// Nothing here is a fixed column or row count: both fall out of the space the caller hands in, so
// a higher-resolution panel yields MORE cells of the same comfortable size rather than the same
// few cells blown up. The caller supplies the content area it has after the header and button
// hints, plus the tallest cover box worth drawing — that ceiling is the cached thumbnail's own
// height, since the draw path never upscales and a taller box would only add empty frame.
//
// Pure arithmetic: no renderer, no theme, no storage, so it is exercised on the host.
namespace CoverGridLayout {

// Visual constants of the grid itself. The label block holds two small-font lines (title, author).
inline constexpr int kMargin = 10;
inline constexpr int kLabelHeight = 36;
// A cover much narrower than this is unrecognisable artwork, so this is what decides how many
// columns a panel can carry.
inline constexpr int kMinCellWidth = 200;
inline constexpr int kMinCellHeight = 96;

struct Input {
  int contentWidth = 0;   // width available to the grid
  int contentHeight = 0;  // height available below the header
  int bottomReserve = 0;  // strip at the bottom left free for hints / scroll arrows
  int maxCellHeight = 0;  // tallest cover box worth drawing (the stored thumbnail's height)
};

struct Layout {
  int cols = 1;
  int rows = 1;        // rows per page
  int cellWidth = 0;   // cover box width, 1 px frame included
  int cellHeight = 0;  // cover box height, 1 px frame included
  int rowStride = 0;   // cellHeight + label block + margin
  int labelWidth = 0;  // text width available under a cover
};

Layout compute(const Input& in);

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
