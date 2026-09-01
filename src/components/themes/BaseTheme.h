#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class GfxRenderer;
class Epub;
class Txt;
class Xtc;
struct RecentBook;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct TabInfo {
  const char* label;
  bool selected;
};

// A list's live view state, owned by the activity and handed to drawList on every render. Two
// things need somewhere to live between frames and neither belongs to a stateless theme:
//
//   * maxTitleLines > 1 wraps a title too long for one line over up to that many lines, growing
//     its row to match, so items sharing a long prefix ("Author - Series 01 - ...") stay
//     distinguishable instead of all truncating to the same visible text. Rows of different
//     heights cannot page by index, so such a list scrolls, and firstVisible is where it is
//     scrolled to — drawList keeps it in step with the selection.
//   * visibleRows reports how many rows the last render actually fit on screen. That is the page
//     size for Left/Right paging (ButtonNavigator::onNextList), and only the renderer knows it.
//
// Default-constructed and never passed, a list behaves exactly as it always did: fixed-height
// rows, whole-page flips, titles truncated with an ellipsis.
struct ListViewState {
  int maxTitleLines = 1;  // in: 1 = classic single-line rows
  int firstVisible = 0;   // in/out: index of the top row (wrapped lists only)
  int visibleRows = 0;    // out: rows on screen after the last render

  [[nodiscard]] bool wraps() const { return maxTitleLines > 1; }
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu = false;
  int homeMenuTopOffset = 0;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;

  int keyboardKeyWidth;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  int keyboardBottomKeyHeight;
  int keyboardBottomKeySpacing;
  bool keyboardBottomAligned;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;
  int keyboardKeyCornerRadius = 0;
};

enum UIIcon { Folder, Text, Image, Book, File, Recent, Settings, Transfer, Library, Wifi, Hotspot, Weather };

enum class KeyboardKeyType { Normal, Shift, Mode, Reveal, Space, Del, Ok, Disabled };

enum class HomeNavigation { Linear, Carousel };

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 65,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 22,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  virtual void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  virtual void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                               bool showPercentage = true) const;  // Left aligned (reader mode)
  virtual void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                                bool showPercentage = true) const;  // Right aligned (UI headers)
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  virtual void drawSideButtonHints(GfxRenderer& renderer, const char* upBtn, const char* downBtn) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        ListViewState* view = nullptr) const;
  virtual void drawListSeparator(const GfxRenderer& renderer, Rect rowRect, int textX, int textWidth,
                                 const std::string& title) const;
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  // Draws a centered message box and ships it. By default the box is overlaid on the frame that is
  // currently on screen: displayBuffer() ends in a buffer swap, so the write buffer holds the frame
  // from two refreshes ago, and a bare overlay would diff stale content around the box (ghosting).
  // Syncing the write buffer from the displayed frame first fixes that. Callers that have already
  // composed a full fresh frame into the write buffer (clearScreen + render, then popup in the same
  // displayBuffer) must pass overlayDisplayedFrame=false so their render is not discarded.
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message, bool overlayDisplayedFrame = true) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  virtual void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                             const int pageCount, std::string title, const int paddingBottom = 0,
                             const bool isStarred = false, const std::string& printedPageLabel = std::string(),
                             const bool fillMargin = true, const bool pageCountApproximate = false) const;
  virtual void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                               const char* secondaryLabel = nullptr, KeyboardKeyType keyType = KeyboardKeyType::Normal,
                               bool inactiveSelection = false) const;
  virtual bool showsFileIcons() const { return false; }

  // ---- Home screen navigation / rendering contract ----

  // Return Carousel to opt in to left/right book navigation and tryFastHomeRender().
  virtual HomeNavigation getHomeNavigation() const { return HomeNavigation::Linear; }

  // Return the cover thumbnail sizes this theme needs for its home screen.
  // HomeActivity calls generateThumbBmp() for each pair. Empty = use height-only path.
  virtual std::vector<std::pair<int, int>> getCoverThumbSizes(int coverHeight) const { return {}; }

  // Attempt a full home-screen render from pre-cached state.
  // Return true if the theme handled the render (HomeActivity must not draw anything else).
  // Return false to fall through to the standard slow-path render in HomeActivity.
  virtual bool tryFastHomeRender(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, int selectorIndex,
                                 int menuCount, const std::function<std::string(int)>& menuLabel,
                                 const std::function<UIIcon(int)>& menuIcon, const char* hintBtn1, const char* hintBtn2,
                                 const char* hintBtn3, const char* hintBtn4) const {
    return false;
  }

  // Called by readers just before releasing the book object. Themes that cache
  // cover thumbnails can generate them here while the book is still loaded.
  // Only one of epub/xtc/txt will be non-null depending on the reader.
  virtual void onBookWillClose(const std::string& path, Epub* epub, Xtc* xtc, Txt* txt) {}

  // Called when HomeActivity exits. Themes that hold heap-allocated render caches
  // should free them here so the memory is available to child activities.
  virtual void invalidateFrameCache() {}

  // Mark the frame cache stale without freeing it. tryFastHomeRender will
  // rebuild on the next render pass. Multiple dirty marks coalesce into one
  // rebuild — use this instead of invalidateFrameCache() when a cover BMP was
  // just written so back-to-back cover arrivals don't each trigger an SD re-read.
  virtual void markFrameCacheDirty() {}

  // ---- Shared constants and helpers for battery drawing (used by all themes) ----
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);

  // Footprint actually occupied by drawBatteryLeft(): the icon plus, when shown, the live
  // percentage text. Anything placed to its right (the clock, the title) must reserve this rather
  // than estimate it — a three digit percentage ("100%") is wider than any fixed guess, which is
  // how the clock ended up drawn on top of the battery percentage (issue #214).
  static int statusBarBatteryWidth(const GfxRenderer& renderer, const ThemeMetrics& metrics, bool showPercentage);

  // Hard ceiling on wrapped title lines. Past three the row stops reading as one entry in a list
  // and starts reading as a paragraph, and a single item can fill the screen.
  static constexpr int maxWrappedTitleLines = 3;

 protected:
  // Per-theme look of a wrapped (variable-height) row. drawWrappedList() owns the layout; these
  // are the few things the themes actually disagree about.
  struct WrappedListStyle {
    int hPadding = 0;          // horizontal padding inside the selection shape
    int iconSize = 0;          // 0 = this theme draws no row icons
    int titleTextOffsetY = 0;  // first text line's offset from the row top
    int cornerRadius = 0;      // 0 = square selection shape
    int scrollBarWidth = 0;    // 0 = this theme marks overflow with arrows instead of a bar
    int scrollBarRightOffset = 0;
    bool selectionIsBlack = true;  // black fill + inverted text, vs a light-gray fill
    // Classic draws the selection edge to edge; the Lyra themes inset it into a rounded pill.
    bool fullWidthSelection = true;
  };
  virtual WrappedListStyle wrappedListStyle() const { return {}; }
  // Bitmap for a row icon, or nullptr when the theme draws none. Mirrors what each theme's own
  // drawList does with rowIcon.
  virtual const uint8_t* rowIconBitmap(UIIcon icon, int size) const { return nullptr; }

  // The variable-height list, shared by every theme: layout and text wrapping live here, and the
  // per-theme look comes from wrappedListStyle() / rowIconBitmap(). Each drawList() delegates
  // here when the caller opted in, so there is one scrolling implementation, not one per theme.
  void drawWrappedList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                       const std::function<std::string(int index)>& rowTitle,
                       const std::function<UIIcon(int index)>& rowIcon, ListViewState& view) const;

  // Up/down triangles marking that the list continues past the visible rows. Used by themes
  // without a scroll bar.
  static void drawListOverflowArrows(const GfxRenderer& renderer, Rect rect);
};
