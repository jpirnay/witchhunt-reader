#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "FontInstaller.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL "https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  // freshBoot: true only when entered via the silent-restart boot target
  // (ActivityManager::goToFontManager). Any other entry restarts into that
  // target first — the manifest TLS handshake needs ~36KB contiguous heap,
  // which only a fresh boot reliably provides. Mirrors the onExit restart.
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool freshBoot = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
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

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;  // false = legacy v1 manifest, fall back to size-only check
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    // `styles` was once parsed here but never rendered — dropped to avoid
    // ArduinoJson string allocations that fragmented the heap before the
    // first TLS download. Resurrect if a UI surfaces style names.
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
    // True iff a leftover __staging dir from a previous interrupted download
    // exists for this not-yet-installed family — the next confirm will resume
    // rather than restart.
    bool hasResumableDownload = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // HTTP/TLS session shared across all files of a single downloadFamily()
  // call. Each family install pays the TLS handshake once (on its first
  // file); subsequent files reuse the open keep-alive connection.
  // NOT shared with the manifest fetch — holding the TLS context open
  // through the JSON parse aborts on the ~36 KB contiguous allocation
  // collision with ArduinoJson's working memory.
  HttpDownloader::Session httpSession_;

  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  int selectedIndex_ = 0;

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
  // without touching families_ (which is empty during the download).
  std::string downloadingFamilyName_;
  bool downloadingFamilyHasResumable_ = false;
  PendingFontAction pendingErrorAction_ = PendingFontAction::None;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  bool freshBoot_ = false;
  // True while the display framebuffer is lent to the TLS stack. The RSA
  // handshake peaks around ~50KB against ~54KB free-after-WiFi on low-heap
  // devices; the released 48KB buffer is the difference between
  // MBEDTLS_ERR_X509_*ALLOC failures and a clean handshake. The e-ink panel
  // physically retains the last frame, so the UI stays visible. While set,
  // nothing may render, and every code path must end in finishNetworkPhase()
  // (which reboots to re-arm the display).
  bool framebufferReleased_ = false;
  // WiFi only survives within one boot; after a marker resume it is down
  // until the next network action re-runs WifiSelectionActivity.
  bool wifiUp_ = false;
  // One-shot continuation invoked after a successful WiFi (re)connect, set
  // when a network action is confirmed while WiFi is down.
  std::function<void()> afterWifi_;
  int previousActionCount_ = 0;

  // Phase persisted across the mid-flow silent restarts. Network work runs
  // with the framebuffer released; the restart re-arms the display and the
  // fresh instance resumes into the recorded state via tryResumeFromMarker().
  enum class ResumePhase : uint8_t { None = 0, List = 1, Complete = 2, Error = 3 };

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  // Render the loading screen, release the framebuffer, fetch + stash the
  // manifest, then restart into the family list (or the error screen).
  void runManifestPhase();
  // Run `action` now if WiFi is up, otherwise after WifiSelectionActivity.
  void startNetworkAction(std::function<void()> action);
  // Confirm dispatch for the family list (delete needs no network).
  void onConfirmFromList();
  // Release both display buffers to give the TLS stack the ~48KB it is short
  // of. No rendering allowed afterwards; ends with a finishNetworkPhase().
  void releaseFramebufferForNetwork();
  // Stash families_ + write the resume marker, then silent-restart back into
  // the font manager. Never returns during normal operation.
  void finishNetworkPhase(ResumePhase phase);
  // Restore state from a resume marker left by finishNetworkPhase().
  // Returns false when there is no (valid) marker.
  bool tryResumeFromMarker();
  // Download a single family by its index into families_, with the
  // framebuffer released for TLS heap headroom. Ends in finishNetworkPhase()
  // (i.e. a restart); never returns to the caller's flow.
  void downloadFamily(int familyIdx);
  // Shared per-family body for downloadFamily/downloadAll/updateAll: sets up
  // progress state (rendering only while the framebuffer is still armed),
  // releases the framebuffer, runs the impl and merges the family's mutations
  // back into families_. Returns false on error/cancel (state_ tells which).
  bool downloadOne(int familyIdx);
  // Internal: the download/verify/activate body. Operates on the local family
  // copy; runs with the framebuffer released, so it must not render.
  void downloadFamilyImpl(ManifestFamily& family, int familyIdx);
  void downloadAll();
  void updateAll();

  // Persist families_ to /fonts_families.bin and clear the in-memory vector.
  // Used to free the ~10 KB of scattered std::string allocations that fragment
  // the heap enough to break the TLS handshake during font downloads.
  bool stashFamiliesToSd();
  // Read /fonts_families.bin back into families_. Returns true on success.
  bool restoreFamiliesFromSd();
  bool isDownloadAllSelected() const { return hasDownloadCandidates() && selectedIndex_ == 0; }
  bool isUpdateAllSelected() const {
    if (!hasUpdateCandidates()) return false;
    return selectedIndex_ == (hasDownloadCandidates() ? 1 : 0);
  }
  bool hasDownloadCandidates() const;
  bool hasUpdateCandidates() const;
  int actionCount() const { return (hasDownloadCandidates() ? 1 : 0) + (hasUpdateCandidates() ? 1 : 0); }
  int familyIndexFromList(int listIndex) const {
    return listIndex > actionCount() - 1 ? listIndex - actionCount() : -1;
  }
  int listItemCount() const { return families_.empty() ? 0 : static_cast<int>(families_.size()) + actionCount(); }
  size_t totalUninstalledSize() const;
  size_t totalUpdateSize() const;
  void syncSelectedIndexForNewActionCount();

  std::string confirmButtonLabel() const;
  void promptDeleteFamily(int familyIndex);
  void deleteFamilyAtIndex(int familyIndex);

  static std::string formatSize(size_t bytes);
};
