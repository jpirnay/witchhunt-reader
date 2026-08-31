#pragma once

#include <Epub/FootnoteEntry.h>

#include <vector>

#include "../Activity.h"

class EpubReaderFootnotesActivity final : public Activity {
 public:
  // previews: optional note text per footnote (parallel to footnotes, resolved from the
  // book-level footnotes.bin when it exists); empty strings render as the plain marker.
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes,
                                       std::vector<std::string> previews = {})
      : Activity("EpubReaderFootnotes", renderer, mappedInput), footnotes(footnotes), previews(std::move(previews)) {}

  void onEnter() override;
  void onExit() override;
  // Tap on a footnote row -> move the selection there; ActivityManager synthesizes Confirm.
  bool selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void advanceSelection(int delta);
  const std::vector<FootnoteEntry>& footnotes;
  const std::vector<std::string> previews;
  int selectedIndex = 0;
  int scrollOffset = 0;
};
