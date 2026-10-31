#include "EpubReaderChapterSelectionActivity.h"

#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace fui = freeink::ui;

namespace {
// Levels past this many share the deepest indent, so a deeply nested TOC keeps room for its titles.
constexpr int kMaxIndentLevels = 6;
}  // namespace

void EpubReaderChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();
  if (!epub) return;

  int selected = (currentTocIndex >= 0 && currentTocIndex < epub->getTocItemsCount())
                     ? currentTocIndex
                     : epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selected < 0) selected = 0;
  nav.selected = selected;
}

const char* EpubReaderChapterSelectionActivity::headerTitle() const { return tr(STR_SELECT_CHAPTER); }

// Ported from crosspoint-reader 9e3cea2e (#2957, "convert all lists and tabs to FUI"), which builds
// the TOC as a FreeInkUI list. Rows come from a provider rather than upstream's materialized
// window, and the level is a ListItem::indent rather than leading spaces, which wrapping drops.
void EpubReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);
  if (!epub) return;
  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props = listProps(screen);
  props.rowProvider = &EpubReaderChapterSelectionActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listCount());
  indentStep_ = static_cast<int16_t>(screen.target().lineHeight(props.labelText.font) * 3 / 4);
  addList(screen, props);
}

void EpubReaderChapterSelectionActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(ctx);
  const auto toc = self->epub->getTocItem(index);
  const size_t length = std::min(toc.title.size(), sizeof(self->labelBuf_) - 1);
  std::memcpy(self->labelBuf_, toc.title.data(), length);
  // Cut on a character boundary, never inside a multi-byte one.
  self->labelBuf_[utf8SafeTruncateBuffer(self->labelBuf_, static_cast<int>(length))] = '\0';
  item.label = self->labelBuf_;
  item.actionValue = static_cast<int16_t>(index);
  const int level = std::clamp(static_cast<int>(toc.level) - 1, 0, kMaxIndentLevels);
  item.indent = static_cast<int16_t>(level * self->indentStep_);
}

void EpubReaderChapterSelectionActivity::activateIndex(const int index) {
  // The chosen row leaves this screen; a lingering tap flash would gray the reader's next frame.
  app.clearTapFlash();
  const int spineIndex = epub->getSpineIndexForTocIndex(index);
  if (spineIndex == -1) {
    onBackButton();
    return;
  }
  setResult(ChapterResult{spineIndex, index});
  finish();
}

void EpubReaderChapterSelectionActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
