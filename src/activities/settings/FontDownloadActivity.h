#pragma once

#include <cstdint>
#include <string>

#include "FontInstaller.h"
#include "activities/UiListActivity.h"
#include "network/HttpDownloader.h"
#include "util/FontCatalog.h"

// Adapted from upstream crosspoint-reader's FontDownloadActivity
// (develop @ cdac66ffe, src/activities/settings/FontDownloadActivity.cpp).
// Only the family list's FreeInkUI skeleton is theirs. The stash-to-SD download, resume, the v1
// manifest, the delete prompt and the reboot on exit are ours; their groups, string arena,
// mandatory CRC and delete-on-abort are not taken.
class FontDownloadActivity final : public UiListActivity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  bool preventAutoSleep() override { return state_ == LOADING_MANIFEST || state_ == DOWNLOADING; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;

  // HTTP/TLS session shared across all files of a single downloadFamily()
  // call. Each family install pays the TLS handshake once (on its first
  // file); subsequent files reuse the open keep-alive connection.
  // NOT shared with the manifest fetch: that closes its TLS connection
  // before the manifest is read, so the reader has the heap to itself.
  HttpDownloader::Session httpSession_;

  std::string baseUrl_;
  // The font list, in one block (about 8 KB for 28 families): freeing it before a download gives the
  // TLS session the heap back in one piece. `styles` in the manifest is not kept -- nothing shows it.
  // hasResumableDownload: a __staging dir from an interrupted download exists for this not-yet-
  // installed family, and the next confirm resumes it rather than restarting.
  // Read by the render task (listCount(), the row provider, the confirm label): replaced or
  // changed on the loop task only under RenderLock.
  FontCatalog families_;

  enum class PendingFontAction {
    None,
    Download,
    Delete,
  };

  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  // Cached during downloadFamily() before families_ is stashed to SD, so the
  // render path can show the family name and decide the Retry/Resume label
  // without touching families_ (which is empty during the download). A
  // fixed buffer: a valid family name always fits, and it takes no heap.
  char downloadingFamilyName_[FontInstaller::MAX_FAMILY_NAME_LEN + 1] = {};
  bool downloadingFamilyHasResumable_ = false;
  PendingFontAction pendingErrorAction_ = PendingFontAction::None;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  int previousActionCount_ = 0;
  // What the two action rows offer, counted by refreshRowTotals() so that listCount() and the row
  // provider never walk families_, and the rows' labels ("Download all (12.3 MB)") they point at.
  int uninstalledCount_ = 0;
  int updateCount_ = 0;
  char downloadAllLabel_[64] = {};
  char updateAllLabel_[64] = {};
  int lastProgressPercent_ = -1;
  unsigned long lastProgressUpdateMs_ = 0;

  // --- The family list (UiListActivity) ---
  int listCount() const override { return state_ == FAMILY_LIST ? listItemCount() : 0; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Every state but the family list: COMPLETE and ERROR read their own events, the others none.
  bool handleCustomInput() override;
  // The header in every state, and the status screens' text outside the family list.
  void drawChrome() override;
  // The list's hint strips in the family list; Back and Retry/Resume on the status screens.
  void drawFooter() override;
  [[nodiscard]] const char* footerConfirmLabel() const override;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  // Download a single family by its index into families_. Internally stashes
  // families_ to SD so the TLS handshake runs on a defragmented heap; the
  // selected family's state mutations (installed/hasUpdate/hasResumableDownload)
  // are merged back into families_ on return.
  void downloadFamily(int familyIdx);
  // Internal: the body of downloadFamily after the stash. Operates only on
  // `family`, a copy holding just the one family (at index 0), and constants
  // like familyIdx; never touches families_ (which is empty during this call).
  void downloadFamilyImpl(FontCatalog& family, int familyIdx);
  void downloadAll();
  void updateAll();
  // The transfer's input pump: true once Back has been pressed, read as an event.
  bool backPressedDuringTransfer();

  // Persist families_ to /fonts_families.bin and free its block, under the
  // RenderLock. The TLS session of the download that follows needs a
  // contiguous ~17 KB record buffer, which the block gives back in one piece.
  bool stashFamiliesToSd();
  // Read /fonts_families.bin into `out`, a block of its own. Returns true on success. The caller
  // swaps it into families_ under RenderLock.
  bool restoreFamiliesFromSd(FontCatalog& out);
  bool hasDownloadCandidates() const { return uninstalledCount_ > 0; }
  bool hasUpdateCandidates() const { return updateCount_ > 0; }
  int actionCount() const { return (hasDownloadCandidates() ? 1 : 0) + (hasUpdateCandidates() ? 1 : 0); }
  bool isDownloadAllRow(const int row) const { return hasDownloadCandidates() && row == 0; }
  bool isUpdateAllRow(const int row) const { return hasUpdateCandidates() && row == (hasDownloadCandidates() ? 1 : 0); }
  // The family a list row shows; -1 for an action row or a row past the list.
  int familyIndexFromList(const int listIndex) const {
    const int familyIndex = listIndex - actionCount();
    return familyIndex >= 0 && familyIndex < static_cast<int>(families_.count()) ? familyIndex : -1;
  }
  int listItemCount() const { return families_.empty() ? 0 : static_cast<int>(families_.count()) + actionCount(); }
  // Recounts the action rows and formats their labels. Call under RenderLock after families_ or a
  // family's installed / hasUpdate flag changes.
  void refreshRowTotals();
  // Loop task only: moves the selection through the nav when the action rows come or go.
  void syncSelectedIndexForNewActionCount();

  void promptDeleteFamily(int familyIndex);
  void deleteFamilyAtIndex(int familyIndex);

  static void formatSize(size_t bytes, char* out, size_t outSize);
};
