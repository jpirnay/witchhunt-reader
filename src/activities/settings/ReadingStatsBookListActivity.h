#pragma once

#include <cstdint>
#include <vector>

#include "ReadingStats.h"
#include "activities/UiListActivity.h"

// Phase-2 sub-screen: scrollable list of all books with recorded reading time.
// Sorted by total time descending so the most-read books are easiest to reach.
// Selecting a row pushes ReadingStatsBookDetailActivity.
//
// The history is never loaded: one scan of the stats file gives the order by time (8 bytes a
// book), and only the rows around the window on screen are decoded, each read at its offset
// (util/ListRowCache.h decides which).
class ReadingStatsBookListActivity final : public UiListActivity {
 public:
  bool drawsBookText() const override { return true; }
  explicit ReadingStatsBookListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("ReadingStatsBookList", renderer, mappedInput) {}

  void onEnter() override;

 private:
  // Until the first render publishes how many rows it drew, decode as if this many did. Opening on
  // the first row that decodes twice as many (the window and the one after it): more than any
  // panel shows, so the opening screen is fully decoded.
  static constexpr int kOpeningRows = 10;

  int listCount() const override { return static_cast<int>(index_.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  [[nodiscard]] const char* footerConfirmLabel() const override;
  // The per-tick coverage check: a render can draw a window the rows were not decoded for.
  bool handleCustomInput() override;
  void onSelectionChanged(int index) override;
  // A page turn knows the top the next render starts from; it hands it to onSelectionChanged().
  void showPositionPage(int position, int topPosition) override;

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  // Rows are decoded on the loop task and swapped in under RenderLock; the render only reads them.
  std::vector<ReadingStatsStore::IndexEntry> index_;
  uint32_t indexSeq_ = 0;  // the history's generation index_ was taken at
  std::vector<BookReadingStats> rows_;
  int rowsFirst_ = 0;
  // The top a page turn in progress has set, for the onSelectionChanged() it calls; -1 otherwise.
  int pageTop_ = -1;
  // Render task only: the row the provider is filling. The list reads a row's strings before it
  // asks for the next row, so one buffer of each serves every row.
  char labelBuf_[128] = {};
  char valueBuf_[24] = {};

  void rebuildIndex();
  // Decodes the rows the next render draws, and a window either side, unless they are held.
  void ensureRowsFor(int selected, int windowTop);
  // The rows the last render drew, or kOpeningRows before any render has published.
  int windowRows() const;
  // Rows [first, first + page) of the index; false when the history changed since it was taken.
  bool readRows(int first, int page, std::vector<BookReadingStats>& rows) const;
  const BookReadingStats* rowAt(int index) const;
};
