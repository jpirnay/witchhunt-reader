#pragma once

#include <Epub/BookSearch.h>

#include <memory>
#include <string>

#include "BookSearchSession.h"
#include "activities/UiListActivity.h"

class Epub;

// The book's search, over the reader: the query from the keyboard, the book read chapter by chapter from the page the
// search began on round to the page before it, and the hits listed as they come -- the text around each, its chapter
// and how far into the book it lies. A hit chosen goes back to the reader (BookSearchResult, session.current), which
// shows its page with the words outlined. The first row searches again, the second turns Whole words on or off.
//
// One chapter is read a loop() at a time, under the render lock, with the secondary framebuffer lent as the inflate
// window and the parser's state, as a section build takes it; the list is drawn again between chapters, at most once
// a second unless new hits came. Back while it reads stops it, the hits so far kept.
class BookSearchActivity final : public UiListActivity {
 public:
  // `startQuery`: a query to search for at once (a selected word). Otherwise the session's last search is listed, or
  // the keyboard opens for a first one.
  BookSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                     BookSearchSession& session, std::string startQuery = "");
  void onEnter() override;
  void loop() override;
  bool drawsBookText() const override { return true; }

 private:
  // The first two rows: search again, Whole words.
  static constexpr int kLeadRows = 2;

  std::shared_ptr<Epub> epub;
  BookSearchSession& session;
  std::string startQuery;
  BookSearch::Pattern pattern{""};
  bool searching = false;
  int step = 0;  // of getSpineItemsCount() + 1: the origin's chapter from the origin, the others, the origin's again
  unsigned long lastDrawMs = 0;
  size_t drawnCount = 0;

  // Render task only: the strings of the row the provider is filling, and the header's.
  char labelBuf_[BookSearch::Hit::kSnippetBytes + 8] = {};
  char subtitleBuf_[160] = {};
  char valueBuf_[8] = {};
  mutable char headerBuf_[96] = {};

  int listCount() const override { return kLeadRows + static_cast<int>(session.count); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  void openKeyboard();
  void startSearch();
  void searchStep();
  void stopSearch();
  // How far through the search is, in percent.
  int progressPercent() const;
};
