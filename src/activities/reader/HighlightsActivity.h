#pragma once

#include "../Activity.h"
#include "HighlightStore.h"
#include "util/ButtonNavigator.h"

// The current book's highlights: Confirm jumps to one, logical Right deletes it (the deletion
// reaches BookOrbit on the next sync). Edits go straight to the reader's store and are saved
// when this screen closes.
class HighlightsActivity final : public Activity {
  HighlightStore& store;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  // Preview: the selected highlight's full text in place of the list.
  //
  // A mode rather than a second activity. The store, the selection and the button handling are
  // all already here, and the text is already in RAM -- the list simply throws away everything
  // past the first 60 bytes -- so showing the rest costs a branch in render() and no storage at
  // all. Select still jumps straight from the list, because the snippet is usually enough to
  // recognise a highlight; this is for when it is not.
  bool previewing = false;
  int previewScroll = 0;     // index of the first visible wrapped line
  int previewMaxScroll = 0;  // set by render(), which is where the wrapping happens

  std::string getItemLabel(int index) const;
  void deleteSelected();
  void close();

 public:
  explicit HighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HighlightStore& store)
      : Activity("Highlights", renderer, mappedInput), store(store) {}
  void onEnter() override;
  void onExit() override;
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;
};
