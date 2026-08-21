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

}  // namespace CoverGridLayout
