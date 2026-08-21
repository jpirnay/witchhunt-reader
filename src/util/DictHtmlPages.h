#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;

// Lay out a StarDict HTML definition through the EPUB chapter parser, yielding
// the same styled, paginated Pages the reader renders.
//
// The definition is normalized into well-formed XHTML (see
// DictHtmlNormalize.h) and streamed straight into the parser -- it is a Print
// sink, so nothing is staged on the SD card on the way.
//
// Returns false when the fragment cannot be parsed, when the page budget below
// would be exceeded, or on low heap; the caller then falls back to the
// plain-text viewer. pagesOut is left empty on every false return.
// `fontId` is the caller's choice rather than the reader's setting: the
// dictionary pins its own font (see DictionaryDefinitionActivity.h).
bool buildDictionaryHtmlPages(GfxRenderer& renderer, const std::string& definition, int fontId, uint16_t viewportWidth,
                              uint16_t viewportHeight, std::vector<std::unique_ptr<Page>>& pagesOut);
