#pragma once
#include <Epub.h>
#include <I18n.h>

#include <array>
#include <string>
#include <vector>

#include "../TabbedUiListActivity.h"
#include "activities/settings/SettingInfo.h"
#include "components/UITheme.h"

class EpubReaderMenuActivity final : public TabbedUiListActivity {
 public:
  // Menu actions identified by StrId of the menu item.
  // Used by the parent activity to interpret the result.
  enum class MenuAction {
    NONE,
    SELECT_CHAPTER,
    FOOTNOTES,
    DICTIONARY,
    HIGHLIGHT_TEXT,
    HIGHLIGHTS,
    EMBEDDED_STYLE,
    IMAGE_RENDERING,
    TEXT_DARKNESS,
    GO_TO_PERCENT,
    GO_TO_PRINTED_PAGE,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    PULL_REMOTE,
    PUSH_LOCAL,
    STARRED_PAGES,
    STAR_PAGE,
    MARK_AS_READ,
    DELETE_CACHE,
    RENDER_BENCHMARK,
    READING_STATS_FOR_BOOK,
    BOOK_INFO,
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes,
                                  const int8_t initialEmbeddedStyleOverride, const int8_t initialImageRenderingOverride,
                                  const int8_t initialFontFamilyOverride,
                                  const std::string& initialSdFontFamilyOverride, const int8_t initialFontSizeOverride,
                                  const uint8_t initialTextDarkness, const bool initialBionicReadingOverride,
                                  const int8_t initialGuideDotsOverride, const int8_t initialParagraphAlignmentOverride,
                                  const int8_t initialTextAntiAliasingOverride, const int8_t initialHyphenationOverride,
                                  const int8_t initialFontSizeNormalizationOverride,
                                  const int8_t initialInlineFootnotePreviewsOverride, const bool hasStarredPages,
                                  const bool isCurrentPageStarred, const bool hasPrintedPages);

  void onEnter() override;
  void onExit() override;

 private:
  enum class MenuTab : uint8_t { Navigation, Settings, Sync, Tools, Count };
  static constexpr size_t MENU_TAB_COUNT = static_cast<size_t>(MenuTab::Count);

  void buildMenuItems(bool hasFootnotes, bool hasStarredPages, bool hasPrintedPages);
  [[nodiscard]] MenuTab activeTab() const { return visibleTabs[selectedTab()]; }
  [[nodiscard]] size_t activeTabIndex() const { return static_cast<size_t>(activeTab()); }
  [[nodiscard]] std::vector<SettingInfo>& activeMenuItems() { return tabMenuItems[activeTabIndex()]; }
  [[nodiscard]] const std::vector<SettingInfo>& activeMenuItems() const { return tabMenuItems[activeTabIndex()]; }

  bool currentPageStarred = false;
  void finishWithAction(MenuAction action);

  // UiListActivity / TabbedUiListActivity overrides
  int listCount() const override { return static_cast<int>(activeMenuItems().size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Each tab keeps its own scroll position and selection, so a reader who steps away to another
  // tab and back finds the row they left rather than the top of the list.
  freeink::ui::ListNav& activeNav() override { return tabNav[activeTabIndex()]; }
  int tabCount() const override { return visibleTabCount; }
  const char* tabLabel(int slot) const override;
  // Taller than the theme's tab bar because this one carries icons, not just a label. Grows
  // with the UI font size so the tab labels keep their clearance at the larger step.
  int16_t tabBarHeight() const override { return static_cast<int16_t>(54 + UITheme::fontGrowth().body); }
  void customizeTabBar(UiScreen& screen, freeink::ui::TabBarProps& props) override;
  void onBackFromTabs() override { onBackPressed(); }
  void drawChrome() override;
  void drawFooter() override;
  [[nodiscard]] std::string getItemValueString(int index) const;
  void onActionSelected(int index);
  void onBackPressed();
  void onSettingToggled(int index);
  void materializeListWindow();

  static bool paintTabIcon(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, const freeink::ui::TabItem& tab,
                           uint8_t index, void* user);

  // Map from StrId to MenuAction for result passing
  static MenuAction actionForNameId(StrId nameId);

  std::array<std::vector<SettingInfo>, MENU_TAB_COUNT> tabMenuItems;
  std::array<MenuTab, MENU_TAB_COUNT> visibleTabs{};
  std::array<freeink::ui::ListNav, MENU_TAB_COUNT> tabNav;
  uint8_t visibleTabCount = 0;

  static constexpr size_t LIST_WINDOW_CAPACITY = 24;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowLabels;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowValues;
  std::array<freeink::ui::ListItem, LIST_WINDOW_CAPACITY> windowItems;
  uint16_t windowFirst = 0;
  uint16_t windowCount = 0;

  // Pending state (mutated locally, returned to parent on finish)
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  int8_t pendingEmbeddedStyleOverride = -1;
  int8_t pendingImageRenderingOverride = -1;
  int8_t pendingFontFamilyOverride = -1;
  std::string pendingSdFontFamilyOverride;
  int8_t pendingFontSizeOverride = -1;
  uint8_t pendingTextDarkness = 1;
  bool pendingBionicReading = false;
  int8_t pendingGuideDotsOverride = -1;
  int8_t pendingParagraphAlignmentOverride = -1;
  int8_t pendingTextAntiAliasingOverride = -1;
  int8_t pendingHyphenationOverride = -1;
  int8_t pendingFontSizeNormalizationOverride = -1;
  int8_t pendingInlineFootnotePreviewsOverride = -1;

  static constexpr const char* pageTurnLabels[] = {"", "1", "3", "6", "12"};

  std::string title = "Reader Menu";
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
