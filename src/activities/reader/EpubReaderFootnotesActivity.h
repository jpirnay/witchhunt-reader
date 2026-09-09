#pragma once

#include <Epub/FootnoteEntry.h>

#include <cstdint>
#include <vector>

#include "../Activity.h"

class EpubReaderFootnotesActivity final : public Activity {
 public:
  // previews: optional note text per footnote (parallel to footnotes, resolved from the
  // book-level footnotes.bin when it exists); empty strings render as the plain marker.
  // isNote: 1 where the entry is a real footnote, 0 where it is navigation (a contents link, a
  // cross-reference). Parallel to footnotes; an empty vector means "all notes", which is what a
  // book with no preview store looks like.
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes,
                                       std::vector<std::string> previews = {}, std::vector<uint8_t> isNote = {})
      : Activity("EpubReaderFootnotes", renderer, mappedInput),
        footnotes(footnotes),
        previews(std::move(previews)),
        isNote(std::move(isNote)) {}

  void onEnter() override;
  void onExit() override;
  // Tap on a footnote row -> move the selection there; ActivityManager synthesizes Confirm.
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void advanceSelection(int delta);
  // True when entry `i` is a footnote rather than navigation. Absent kinds read as notes.
  bool entryIsNote(size_t i) const { return i >= isNote.size() || isNote[i] != 0; }

  const std::vector<FootnoteEntry>& footnotes;
  const std::vector<std::string> previews;
  const std::vector<uint8_t> isNote;
  // Display order: notes first, then navigation links, each keeping its page order. Holds indices
  // into `footnotes`, so selection and activation go through it rather than indexing directly.
  // Grouping rather than interleaving is what makes ONE divider enough to separate them.
  std::vector<uint16_t> order;
  // Row in `order` where the navigation links begin, or -1 when the page has only one kind. The
  // divider is drawn at the top of that row; it is not a row of its own, so nothing has to be
  // skipped when moving the selection and the touch band stays a plain contiguous run.
  int firstLinkRow = -1;
  int selectedIndex = 0;
  int scrollOffset = 0;
};
