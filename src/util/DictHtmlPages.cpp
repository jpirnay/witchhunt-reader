#include "DictHtmlPages.h"

#include <Arduino.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <Logging.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "DictHtmlNormalize.h"

namespace {

// Keep enough contiguous heap for the parser's own state and the page/layout
// allocations that follow. Falling back to plain text is much cheaper than
// discovering the shortfall halfway through a layout.
constexpr size_t MIN_STYLED_FREE_HEAP = 40 * 1024;
constexpr size_t MIN_STYLED_MAX_ALLOC = 20 * 1024;

// Bound the retained layout independently of the input size: compact markup can
// emit far more page elements than its byte count suggests, and every page here
// is held in RAM at once rather than streamed to a section file.
constexpr size_t MAX_STYLED_PAGES = 64;
constexpr size_t MAX_STYLED_PAGE_ELEMENTS = 512;

bool heapAllowsStyledLayout() {
  return ESP.getFreeHeap() >= MIN_STYLED_FREE_HEAP && ESP.getMaxAllocHeap() >= MIN_STYLED_MAX_ALLOC;
}

}  // namespace

bool buildDictionaryHtmlPages(GfxRenderer& renderer, const std::string& definition, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, std::vector<std::unique_ptr<Page>>& pagesOut) {
  pagesOut.clear();
  if (definition.empty()) return false;
  if (!heapAllowsStyledLayout()) {
    LOG_ERR("DHTML", "Low heap for styled definition (%lu free, %lu max block)",
            static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    return false;
  }

  // One fixed allocation (256 bytes on the C3). The pages must outlive the
  // parser, so they are moved into the caller's vector as they complete.
  pagesOut.reserve(MAX_STYLED_PAGES);

  bool resourceLimitHit = false;
  size_t retainedElements = 0;
  bool parsed = false;
  {
    // Heap-allocated as Section does -- the parser object is far too large for a
    // stack local. A null epub is safe here because imageRendering=2 suppresses
    // <img> entirely, and every path that dereferences epub sits behind either
    // that check or a non-empty image src that only that path can set.
    auto parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
        nullptr, renderer, SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
        SETTINGS.extraParagraphSpacing != 0, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
        SETTINGS.hyphenationEnabled != 0, SETTINGS.fontSizeNormalization != 0, SETTINGS.bionicReading != 0,
        [&pagesOut, &resourceLimitHit, &retainedElements](std::unique_ptr<Page> page) {
          if (resourceLimitHit) return;
          const size_t pageElements = page->elements.size();
          if (pagesOut.size() >= MAX_STYLED_PAGES || retainedElements + pageElements > MAX_STYLED_PAGE_ELEMENTS ||
              !heapAllowsStyledLayout()) {
            resourceLimitHit = true;
            pagesOut.clear();
            return;
          }
          retainedElements += pageElements;
          pagesOut.push_back(std::move(page));
        },
        /*embeddedStyle=*/false, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/2);
    if (!parser) {
      LOG_ERR("DHTML", "OOM: ChapterHtmlSlimParser");
      pagesOut.clear();
      return false;
    }

    // The size only drives the parser's progress reporting and its page-LUT
    // reserve; no progress callback is installed here, and the normalized form
    // is within a few percent of the source, so the definition's own length is
    // the right estimate.
    if (!parser->setup(definition.size())) {
      pagesOut.clear();
      return false;
    }

    // Normalize straight into the parser. A rejected write means the XML parser
    // has already failed, so this stops at the first bad byte rather than
    // pushing the rest of the fragment through a dead parser.
    const bool streamed = normalizeDictionaryHtml(definition, *parser);
    // finalize() regardless: it is what flushes the last page and tears the SAX
    // state down, and it reports a document that ended mid-element.
    parsed = parser->finalize() && streamed && parser->streamSucceeded();
    if (!parsed) LOG_ERR("DHTML", "Definition is not parseable as XHTML; falling back to plain text");
  }

  if (resourceLimitHit) LOG_ERR("DHTML", "Styled definition exceeded the page budget");
  if (!parsed || resourceLimitHit || pagesOut.empty()) {
    pagesOut.clear();
    return false;
  }
  return true;
}
