#pragma once

#include <BuildArena.h>
#include <GfxRenderer.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "FontPreviewCache.h"
#include "activities/UiListActivity.h"
#include "activities/settings/SettingInfo.h"

class MappedInputManager;
struct SdCardFontFamilyInfo;

/// Full-screen list of all reader fonts (built-in + SD card families).
/// Replaces in-place enum cycling for the Reader Font Family setting.
class FontSelectionActivity final : public UiListActivity {
 public:
  enum class Target { EPUB, TXT };

  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Target target = Target::EPUB)
      : UiListActivity("FontSelect", renderer, mappedInput), target(target) {}
  FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SettingInfo& overrideSetting)
      : UiListActivity("FontSelect", renderer, mappedInput), overrideSetting(overrideSetting) {}

  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onSelectionChanged(int index) override;
  void afterUiRender() override;
  // Blit or capture the strip for whatever the preview is currently showing.
  void handlePreviewStrip();
  bool handleCustomInput() override;
  // Queue every SD family whose strip is missing, once the rectangle is known.
  void scanForMissingPreviews();
  // Render the next queued family, one per loop() tick so the watchdog stays fed.
  void advanceWarmup();
  // Leave the warm-up and restore the cursor's preview in one locked step.
  void finishWarmup();
  void updatePreviewFont(int index);
  void updatePreviewFontLocked(int index);
  int previewOptionIndex(int index) const;
  uint8_t selectedFontSize() const;
  // Decide how the next render draws this family: from a cached strip when one
  // matches, otherwise by loading the font. Returns the font id to draw with,
  // or 0 when the strip will be blitted instead.
  int prepareSdPreview(const SdCardFontFamilyInfo& family);
  bool previewKey(FontPreviewCache::Key& key) const;
  // Load the sample sentence's glyphs up front; see the note at the definition.
  void prewarmPreviewGlyphs(int fontId) const;
  void drawWarmupNotice() const;
  void drawPreviewFrame() const;
  // The secondary framebuffer, lent for as long as the selector is open. Preview fonts load into it
  // (loadPreviewFont) instead of the heap, which is at its lowest here: ~31 KB free with the Settings
  // screens underneath, and a preview font takes ~12 KB plus transients (X3, 2026-10-07). Null when
  // nothing could be borrowed -- a reader's background build holds it, or the board has none -- and
  // previews then load on the heap as before.
  void lendPreviewArena();
  void returnPreviewArena();
  // The only way the selector loads or drops a preview font. Unloading before the block is rewound
  // is what makes the arena safe, and dropping first keeps one family in it at a time.
  void loadPreviewFont(const SdCardFontFamilyInfo& family);
  void dropPreviewFont();
  std::unique_ptr<BuildArena> previewArena_;
  BuildArena::Block previewBlock_;

  std::vector<std::string> rowLabels;
  std::vector<freeink::ui::ListItem> rowItems;
  Target target = Target::EPUB;
  std::optional<SettingInfo> overrideSetting;

  // The preview strip's rectangle, in screen coordinates, as of the last layout
  // pass. Only the layout knows it, so the first preview after entering always
  // renders rather than probing the cache -- and then stores what it drew.
  int16_t previewX = 0;
  int16_t previewY = 0;
  int16_t previewW = 0;
  int16_t previewH = 0;
  // Which SD family the strip belongs to. Empty for a built-in font, which is
  // resident in flash and needs no cache.
  std::string previewFamily;
  uint32_t previewSourceBytes = 0;
  uint8_t previewPointSize = 0;
  bool previewFromCache = false;
  // The sample is drawn in the regular weight and nothing else, so exactly one
  // style is ever warmed. See the note on prewarmPreviewGlyphs().
  static constexpr uint8_t PREVIEW_STYLE_MASK = 0x01;
  // Floor for the largest contiguous block before the warm-up gives up, measured
  // with no SD font resident (~22.5 KB on an X3) against the ~15 KB a kern-heavy
  // family needs to load and warm. An emergency brake, not a routine limiter.
  static constexpr uint32_t WARMUP_MIN_CONTIGUOUS = 18 * 1024;
  // Set when a strip has been rendered from the real font but not yet written
  // out. Without it every repaint of an uncached preview would rewrite the file.
  // It doubles as the warm-up's "current item is not on screen yet" gate.
  bool previewNeedsStore = false;

  // Warm-up. Rows whose strip is not on the card yet, rendered one per tick on
  // entry with a count on screen. The work is the same either way; doing it up
  // front means one announced wait instead of a stall on whichever cursor move
  // happens to land on a cold font.
  std::vector<int> warmupQueue;
  size_t warmupDone = 0;
  bool warmupActive = false;
  bool warmupScanned = false;
};
