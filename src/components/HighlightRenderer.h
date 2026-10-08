#pragma once

#include <vector>

class GfxRenderer;
class Page;
struct Highlight;

namespace HighlightRenderer {

/**
 * Underlines the words of `page` that belong to any of `highlights` (all of the current
 * chapter). Matching is by text, with whitespace, hyphens and soft hyphens ignored on both
 * sides, so it survives reflow; a highlight that runs onto the previous or next page is
 * matched by the part of it this page shows. Word boxes are measured only for matched words,
 * so a page with no highlight on it costs one text pass and no measurement.
 */
void drawUnderlines(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                    const std::vector<const Highlight*>& highlights);

}  // namespace HighlightRenderer
