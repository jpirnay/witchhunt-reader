#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "OpdsServerStore.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    BOOK_DETAIL,
    FORMAT_SELECTION,
    DOWNLOADING,
    ERROR,
    SEARCH_INPUT
  };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server,
                                   std::string initialQuery = {})
      : Activity("OpdsBookBrowser", renderer, mappedInput),
        buttonNavigator(),
        server(std::move(server)),
        initialQuery_(std::move(initialQuery)) {}

  void onEnter() override;
  void onExit() override;
  // Tap on a row -> move the selection there; ActivityManager synthesizes Confirm. Serves
  // BOTH of this screen's lists (the entry list and the format picker), keyed on state.
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<uint32_t> entryOffsets;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;
  bool memoryTrimmed = false;
  bool coverAvailable = false;
  int selectorIndex = 0;
  int selectedBookIndex = -1;
  int formatSelectorIndex = 0;
  std::vector<std::string> formatSelectionLabels;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing
  std::string initialQuery_;

  OpdsEntry getEntry(size_t index) const;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book, const OpdsAcquisitionLink& acquisition);
  void chooseBookFormat(const OpdsEntry& book);
  void fetchOsdTemplate(const std::string& osdUrl);
  void launchSearch();
  void performSearch(const std::string& query);
  void fetchCoverForEntry(const OpdsEntry& entry);
  bool preventAutoSleep() override { return true; }
};
