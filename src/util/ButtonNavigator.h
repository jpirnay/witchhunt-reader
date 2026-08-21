#pragma once

#include <functional>
#include <vector>

#include "ButtonEventManager.h"
#include "MappedInputManager.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  using Buttons = std::vector<MappedInputManager::Button>;

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
                 uint32_t& lastPressMs, uint16_t& lastSeenPressCount, bool& longPressFired, const Callback& onChange);
  [[nodiscard]] static ButtonEventManager::PressLog latestPressLog(const Buttons& buttons);
  void onListPageNav(const Buttons& buttons, bool forward, int& selectedIndex, int totalItems, int pageSize,
                     const Callback& onChange);
  [[nodiscard]] static int effectivePageSize(int pageSize) { return pageSize > 0 ? pageSize : defaultListPageSize; }

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);

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
  // `pageSize` is how many rows the list currently shows (ListViewState::visibleRows); it falls
  // back to defaultListPageSize, and a list shorter than a page pages by a single item, so short
  // menus behave exactly as they always did.
  void onNextList(int& selectedIndex, int totalItems, const Callback& onChange, int pageSize = 0);
  void onPreviousList(int& selectedIndex, int totalItems, const Callback& onChange, int pageSize = 0);

  // Same, for lists whose Left/Right carry their own actions (the file browser's Options, the
  // starred-pages rename/delete): only the given buttons step, and paging is reachable by
  // double-clicking them.
  void onNextList(const Buttons& buttons, int& selectedIndex, int totalItems, const Callback& onChange,
                  int pageSize = 0);
  void onPreviousList(const Buttons& buttons, int& selectedIndex, int totalItems, const Callback& onChange,
                      int pageSize = 0);

  [[nodiscard]] static Buttons getNextButtons() {
    return {MappedInputManager::Button::Down, MappedInputManager::Button::Right};
  }
  [[nodiscard]] static Buttons getPreviousButtons() {
    return {MappedInputManager::Button::Up, MappedInputManager::Button::Left};
  }
  // The halves of the pairs above: stepping is on the side buttons, paging on the front ones.
  [[nodiscard]] static Buttons getStepNextButtons() { return {MappedInputManager::Button::Down}; }
  [[nodiscard]] static Buttons getStepPreviousButtons() { return {MappedInputManager::Button::Up}; }
  [[nodiscard]] static Buttons getPageNextButtons() { return {MappedInputManager::Button::Right}; }
  [[nodiscard]] static Buttons getPagePreviousButtons() { return {MappedInputManager::Button::Left}; }
};