#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "ButtonEventManager.h"
#include "MappedInputManager.h"
#include "util/FunctionRef.h"
#include "util/InlineList.h"

class ButtonNavigator final {
  // Inspired by crosspoint-reader PRs #3698 ("perf: remove ButtonNavigator allocation churn") and
  // #3848 ("perf: avoid list navigation callback allocations"), Sung-jin Brian Hong / @serialx.
  // Theirs: the diagnosis. Their fix made the buttons a static initializer_list and trimmed
  // call-site captures to fit std::function's buffer. Here the buttons resolve through the live
  // orientation and cannot be static, so they live in an InlineList; and the callbacks are
  // FunctionRefs, which never allocate whatever they capture.
  //
  // Every list screen calls in here on every loop pass, pressed or not, so nothing on these paths
  // may touch the heap. Callbacks run before the call returns and are never kept, so they are
  // FunctionRefs: on the C3 a std::function keeps only 8 bytes inline, and the page jump's
  // captures went to the heap every pass.
  using Callback = FunctionRef<void()>;
  // The most buttons one role holds: getNextButtons()/getPreviousButtons(), a step button and a
  // page button. The sets are resolved afresh each pass, so they live inline rather than in a
  // std::vector; a longer set fails to compile until this is raised.
  static constexpr std::size_t maxButtonsPerRole = 2;
  using Buttons = InlineList<MappedInputManager::Button, maxButtonsPerRole>;
  using Direction = MappedInputManager::Direction;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  static const MappedInputManager* mappedInput;
  std::function<bool(int)> selectablePredicate;
  int selectableTotalItems = 0;

  // Timestamp of the press this navigator last acted on, and the sampler press count it had seen
  // by then — both per direction. The count is what makes a tap impossible to miss: it rises even
  // when two presses land inside one loop tick, where a polled edge flag reports only one.
  uint32_t lastNextPressMs = 0;
  uint32_t lastPreviousPressMs = 0;
  uint16_t lastNextPressCount = 0;
  uint16_t lastPreviousPressCount = 0;
  bool longPressNextFired = false;
  bool longPressPreviousFired = false;
  int indexBeforePress = 0;

  // Matches ButtonEventManager::DOUBLE_WINDOW_MS: one physical gesture, one definition of how long
  // a double-tap may take. It was 200 ms, which is short for a deliberate double-tap even before
  // anything measured it wrong.
  static constexpr uint16_t listDoubleClickMs = ButtonEventManager::DOUBLE_WINDOW_MS;
  static constexpr uint32_t listLongPressMs = 1500;

  [[nodiscard]] bool shouldNavigateContinuously() const;
  void onListNav(const Buttons& buttons, bool forward, int& selectedIndex, int totalItems, int pageSize,
                 uint32_t& lastPressMs, uint16_t& lastSeenPressCount, bool& longPressFired, Callback onChange);
  [[nodiscard]] static ButtonEventManager::PressLog latestPressLog(const Buttons& buttons);
  void onListPageNav(const Buttons& buttons, bool forward, int& selectedIndex, int totalItems, int pageSize,
                     Callback onChange);
  [[nodiscard]] static int effectivePageSize(int pageSize) { return pageSize > 0 ? pageSize : defaultListPageSize; }

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(Callback callback);
  void onPrevious(Callback callback);
  void onPressAndContinuous(const Buttons& buttons, Callback callback);

  void onNextPress(Callback callback);
  void onPreviousPress(Callback callback);
  void onPress(const Buttons& buttons, Callback callback);

  void onNextRelease(Callback callback);
  void onPreviousRelease(Callback callback);
  void onRelease(const Buttons& buttons, Callback callback);

  void onNextContinuous(Callback callback);
  void onPreviousContinuous(Callback callback);
  void onContinuous(const Buttons& buttons, Callback callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int nextIndex(int currentIndex, const std::vector<bool>& selectable);
  [[nodiscard]] static int previousIndex(int currentIndex, const std::vector<bool>& selectable);
  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems,
                                     const std::function<bool(int index)>& isSelectable);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems,
                                         const std::function<bool(int index)>& isSelectable);

  [[nodiscard]] int nextIndex(int currentIndex) const;
  [[nodiscard]] int previousIndex(int currentIndex) const;
  void setSelectablePredicate(std::function<bool(int)> selectablePredicate, int totalItems);
  void clearSelectablePredicate();

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  // Rows to jump when the list has not told us how many rows are on screen. Roughly a screenful
  // of single-line rows on every theme, and the value the double-click jump used before page size
  // became a parameter.
  static constexpr int defaultListPageSize = 10;

  // List navigation: one item per press, a page per double-click, and the far end on a long press.
  //
  // Up/Down step; Left/Right jump a page. They used to be interchangeable — both pairs stepped one
  // item — which left no way to cross a few hundred chapters or files without holding a button.
  // `pageSize` is how many rows the list currently shows, when the caller knows; it falls
  // back to defaultListPageSize, and a list shorter than a page pages by a single item — that
  // holds for the double-tap jump too, so on a one-screen list every tap is a step and taps that
  // land inside the double-click window cannot throw the selection to the far end.
  void onNextList(int& selectedIndex, int totalItems, Callback onChange, int pageSize = 0);
  void onPreviousList(int& selectedIndex, int totalItems, Callback onChange, int pageSize = 0);

  // Same, for lists whose Left/Right carry their own actions (the file browser's Options, the
  // starred-pages rename/delete): only the given buttons step, and paging is reachable by
  // double-clicking them.
  void onNextList(const Buttons& buttons, int& selectedIndex, int totalItems, Callback onChange, int pageSize = 0);
  void onPreviousList(const Buttons& buttons, int& selectedIndex, int totalItems, Callback onChange, int pageSize = 0);

  // Same again, for a selection that lives in freeink::ui::ListNav.
  //
  // The SDK made ListNav::selected a std::atomic<int> (upstream d78c3b2, "thread-safe navigation
  // with async input handling"): input and render now touch it from different tasks. An atomic
  // converts to int but cannot bind to the int& these take, and the fix at each call site would be
  // the same three lines -- load, navigate, store. It is written once here instead, so a list that
  // keeps its selection in the nav reads exactly like one that keeps its own int.
  void onNextList(std::atomic<int>& selectedIndex, int totalItems, Callback onChange, int pageSize = 0) {
    int index = selectedIndex.load();
    onNextList(index, totalItems, onChange, pageSize);
    selectedIndex.store(index);
  }
  void onPreviousList(std::atomic<int>& selectedIndex, int totalItems, Callback onChange, int pageSize = 0) {
    int index = selectedIndex.load();
    onPreviousList(index, totalItems, onChange, pageSize);
    selectedIndex.store(index);
  }
  void onNextList(const Buttons& buttons, std::atomic<int>& selectedIndex, int totalItems, Callback onChange,
                  int pageSize = 0) {
    int index = selectedIndex.load();
    onNextList(buttons, index, totalItems, onChange, pageSize);
    selectedIndex.store(index);
  }
  void onPreviousList(const Buttons& buttons, std::atomic<int>& selectedIndex, int totalItems, Callback onChange,
                      int pageSize = 0) {
    int index = selectedIndex.load();
    onPreviousList(buttons, index, totalItems, onChange, pageSize);
    selectedIndex.store(index);
  }

  // Resolved through MappedInputManager::buttonFor on every call, so a list navigates by what the
  // reader sees rather than by which edge of the panel a button happens to sit on: in landscape
  // the front strip stands vertically and steps, while the side buttons lie across the bottom (or
  // top) and page. Cheap enough to re-resolve per tick — a switch on the current orientation, into
  // a list returned by value with no heap behind it.
  [[nodiscard]] static Buttons getNextButtons() {
    return {MappedInputManager::buttonFor(Direction::Down), MappedInputManager::buttonFor(Direction::Right)};
  }
  [[nodiscard]] static Buttons getPreviousButtons() {
    return {MappedInputManager::buttonFor(Direction::Up), MappedInputManager::buttonFor(Direction::Left)};
  }
  // The halves of the pairs above: stepping is on the buttons that run up/down the screen, paging
  // on the ones that run across it.
  [[nodiscard]] static Buttons getStepNextButtons() { return {MappedInputManager::buttonFor(Direction::Down)}; }
  [[nodiscard]] static Buttons getStepPreviousButtons() { return {MappedInputManager::buttonFor(Direction::Up)}; }
  [[nodiscard]] static Buttons getPageNextButtons() { return {MappedInputManager::buttonFor(Direction::Right)}; }
  [[nodiscard]] static Buttons getPagePreviousButtons() { return {MappedInputManager::buttonFor(Direction::Left)}; }
};