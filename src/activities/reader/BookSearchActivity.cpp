#include "BookSearchActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <optional>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/RenderLock.h"
#include "activities/util/KeyboardEntryActivity.h"

namespace fui = freeink::ui;

namespace {

// How often the list is drawn again while the search reads on and finds nothing new.
constexpr unsigned long kRedrawEveryMs = 1000;

}  // namespace

BookSearchActivity::BookSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       std::shared_ptr<Epub> epub, BookSearchSession& session, std::string startQuery)
    : UiListActivity("BookSearch", renderer, mappedInput),
      epub(std::move(epub)),
      session(session),
      startQuery(std::move(startQuery)) {}

void BookSearchActivity::onEnter() {
  UiListActivity::onEnter();
  if (!startQuery.empty()) {
    session.query = startQuery;
    startSearch();
    return;
  }
  if (session.query.empty()) {
    openKeyboard();
    return;
  }
  // The last search, its hit shown in the reader selected.
  if (session.current >= 0) nav.selected = kLeadRows + session.current;
}

void BookSearchActivity::openKeyboard() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), session.query,
                                                                 64, InputType::Text),
                         [this](const ActivityResult& result) {
                           const auto* keyboard =
                               result.isCancelled ? nullptr : std::get_if<KeyboardResult>(&result.data);
                           if (keyboard == nullptr || BookSearch::Pattern(keyboard->text).empty()) {
                             // Nothing to search for: back to the list there is, or to the book.
                             if (session.count == 0 && !searching) {
                               finish();
                             } else {
                               requestUpdate();
                             }
                             return;
                           }
                           session.query = keyboard->text;
                           startSearch();
                         });
}

void BookSearchActivity::startSearch() {
  if (!epub || !session.ensureHits()) {
    LOG_ERR("BSR", "No memory for the search's hits");
    finish();
    return;
  }
  {
    RenderLock lock(*this);
    session.clearResults();
    pattern = BookSearch::Pattern(session.query);
    step = 0;
    searching = !pattern.empty() && epub->getSpineItemsCount() > 0;
    drawnCount = 0;
    nav.selected = 0;
  }
  LOG_INF("BSR", "Searching for \"%s\"%s from spine %u", session.query.c_str(),
          session.wholeWords ? " (whole words)" : "", session.originSpine);
  lastDrawMs = millis();
  requestUpdate();
}

void BookSearchActivity::stopSearch() {
  {
    RenderLock lock(*this);
    searching = false;
  }
  LOG_INF("BSR", "Search stopped at step %d: %u hits", step, static_cast<unsigned>(session.count));
  requestUpdate();
}

void BookSearchActivity::searchStep() {
  const int spines = epub->getSpineItemsCount();
  int spine = session.originSpine;
  uint32_t from = 0;
  uint32_t until = UINT32_MAX;
  if (step == 0) {
    from = session.originOffset;
  } else if (step < spines) {
    spine = (session.originSpine + step) % spines;
  } else {
    until = session.originOffset;
  }
  const unsigned long started = millis();
  bool parsed = true;
  if (until > from && spine >= 0 && spine < spines) {
    RenderLock lock(*this);
    // The secondary framebuffer, lent as a section build takes it: the inflate window and the parser's state, not
    // the heap. No render runs under the lock, so nothing draws while it is out.
    size_t lentSize = 0;
    uint8_t* const lent = renderer.borrowSecondaryBuffer(&lentSize);
    {
      std::optional<BuildArena> arena;
      if (lent != nullptr) arena.emplace(lent, lentSize);
      BuildArena* const scratch = arena ? &*arena : nullptr;
      BookSearch::ChapterSearch chapter(pattern, session.wholeWords, session.hits.get(), BookSearchSession::kMaxHits,
                                        session.count, static_cast<uint16_t>(spine), from, until);
      parsed = chapter.begin(scratch) && epub->readItemContentsToStream(epub->getSpineItem(spine).href, chapter.sink(),
                                                                        1024, chapter.stopFlag(), scratch);
      parsed = chapter.finish() && parsed;
    }
    if (lent != nullptr) renderer.returnSecondaryBuffer();
  }
  if (!parsed) LOG_ERR("BSR", "Spine %d could not be searched", spine);
  LOG_DBG("BSR", "Spine %d searched in %lu ms: %u hits so far", spine, millis() - started,
          static_cast<unsigned>(session.count));
  ++step;
  if (session.full() || step > spines) {
    {
      RenderLock lock(*this);
      searching = false;
      session.complete = !session.full();
    }
    LOG_INF("BSR", "Search done: %u hits%s", static_cast<unsigned>(session.count), session.full() ? " (full)" : "");
    requestUpdate();
    return;
  }
  // The list again once a second, or as soon as there is something new on it.
  if (session.count != drawnCount || millis() - lastDrawMs >= kRedrawEveryMs) {
    drawnCount = session.count;
    lastDrawMs = millis();
    requestUpdate();
  }
}

void BookSearchActivity::loop() {
  if (searching) {
    // Back stops the search; another key is let go rather than left for the list to act on once it is done. Taken
    // from the event queue the list reads, not wasReleased(): the Back that stopped the search, left queued, then
    // closed the list as well.
    bool stop = false;
    ButtonEventManager::ButtonEvent event;
    while (buttonEvents.consumeEvent(event)) stop = stop || event.button == MappedInputManager::Button::Back;
    if (stop) {
      stopSearch();
      return;
    }
    searchStep();
    return;
  }
  UiListActivity::loop();
}

void BookSearchActivity::onBackButton() { finish(); }

int BookSearchActivity::progressPercent() const {
  const int spines = epub ? epub->getSpineItemsCount() : 0;
  return spines > 0 ? std::min(99, step * 100 / (spines + 1)) : 0;
}

const char* BookSearchActivity::headerTitle() const {
  if (searching) {
    snprintf(headerBuf_, sizeof(headerBuf_), "%s %d%%", tr(STR_SEARCHING), progressPercent());
  } else {
    snprintf(headerBuf_, sizeof(headerBuf_), "“%s” · %u%s", session.query.c_str(), static_cast<unsigned>(session.count),
             session.full() ? "+" : "");
  }
  return headerBuf_;
}

void BookSearchActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);
  fui::ListProps props = listProps(screen);
  props.rowProvider = &BookSearchActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listCount());
  addList(screen, props);
}

void BookSearchActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<BookSearchActivity*>(ctx);
  item.actionValue = static_cast<int16_t>(index);
  if (index == 0) {
    item.label = tr(STR_SEARCH);
    snprintf(self->subtitleBuf_, sizeof(self->subtitleBuf_), "“%s”", self->session.query.c_str());
    item.subtitle = self->subtitleBuf_;
    return;
  }
  if (index == 1) {
    item.label = tr(STR_SEARCH_WHOLE_WORDS);
    item.toggle = true;
    item.toggleChecked = self->session.wholeWords;
    return;
  }
  const size_t hitIndex = index - kLeadRows;
  if (hitIndex >= self->session.count) return;
  const BookSearch::Hit& hit = self->session.hits[hitIndex];
  // The text around the hit, an ellipsis at each end it was cut at.
  snprintf(self->labelBuf_, sizeof(self->labelBuf_), "…%.*s…", static_cast<int>(hit.snippetLength), hit.snippet);
  item.label = self->labelBuf_;
  // How far into the book it lies, and its chapter: the percentage first, where a long title cut at the row's end
  // leaves it.
  const Epub& book = *self->epub;
  const float within = hit.chapterPermille == BookSearch::ChapterSearch::kPermilleUnknown
                           ? 0.0f
                           : static_cast<float>(hit.chapterPermille) / 1000.0f;
  const int percent = static_cast<int>(book.calculateProgress(hit.spine, within) * 100.0f + 0.5f);
  const int toc = book.getTocIndexForSpineIndex(hit.spine);
  const std::string title = toc >= 0 ? book.getTocItem(toc).title : std::string();
  const int titleBytes = utf8SafeTruncateBuffer(
      title.c_str(), static_cast<int>(std::min<size_t>(title.size(), sizeof(self->subtitleBuf_) - 16)));
  snprintf(self->subtitleBuf_, sizeof(self->subtitleBuf_), "%d%%%s%.*s", percent, titleBytes > 0 ? " · " : "",
           titleBytes, title.c_str());
  item.subtitle = self->subtitleBuf_;
}

void BookSearchActivity::activateIndex(const int index) {
  if (index == 0) {
    openKeyboard();
    return;
  }
  if (index == 1) {
    session.wholeWords = !session.wholeWords;
    startSearch();
    return;
  }
  const int hit = index - kLeadRows;
  if (hit < 0 || static_cast<size_t>(hit) >= session.count) return;
  session.current = hit;
  setResult(BookSearchResult{hit});
  finish();
}
