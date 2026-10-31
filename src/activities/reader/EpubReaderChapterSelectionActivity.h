#pragma once
#include <Epub.h>

#include <memory>

#include "activities/UiListActivity.h"

// The book's table of contents as a FreeInkUI list. A long title wraps onto a second line instead
// of ending in an ellipsis, and each level's indent holds on its wrapped line (ListItem::indent).
class EpubReaderChapterSelectionActivity final : public UiListActivity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  int currentSpineIndex = 0;
  int currentTocIndex = 0;

  // Render task only: the row the provider is filling, and the indent of one TOC level, from the
  // label font's line height. The list reads a row's strings before it asks for the next row, so
  // one buffer serves every row; TOC entries stay on the card (BookMetadataCache) until drawn.
  char labelBuf_[256] = {};
  int16_t indentStep_ = 0;

  int listCount() const override { return epub ? epub->getTocItemsCount() : 0; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

 public:
  bool drawsBookText() const override { return true; }
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const int currentSpineIndex, const int currentTocIndex)
      : UiListActivity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentTocIndex(currentTocIndex) {}
  void onEnter() override;
};
