#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <functional>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts only. Call once during setup.
  ///
  /// SD font payloads are loaded lazily on reader entry (ensureLoaded*) and
  /// should be unloaded when leaving reader activities.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  void ensureLoaded(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for an explicit family + size.
  /// Used when the reader type determines which settings field to consult.
  /// onColdLoad (if set) fires only when the font has to be written to the flash
  /// partition (genuine first load) — callers use it to show a "loading font" popup.
  void ensureLoaded(GfxRenderer& renderer, const char* familyName, uint8_t fontSizeEnum,
                    const std::function<void()>& onColdLoad = {});

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// Unload any currently loaded SD font family, freeing its heap (intervals,
  /// kern/ligature tables, glyph cache — typically 24-60KB). The registry is
  /// preserved so the font can be reloaded on next ensureLoaded() call.
  void unload(GfxRenderer& renderer) { manager_.unloadAll(renderer); }

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  SdCardFontRegistry& registry() { return registry_; }
  const SdCardFontRegistry& registry() const { return registry_; }

 private:
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
};
