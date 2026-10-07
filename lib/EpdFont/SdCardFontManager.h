#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;
class BuildArena;
class SdCardFont;
struct SdCardFontFamilyInfo;

// Whether a load may populate the flash font partition on a cache miss.
// Writing it erases the whole partition and copies the family back from SD, so
// only a load that the reader will actually use should pay that price.
enum class FlashCachePolicy : uint8_t {
  ReadWrite,  // mmap on a hit, write-then-mmap on a miss
  ReadOnly,   // mmap on a hit, plain SD read on a miss
};

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the single size closest to targetPtSize for a discovered family.
  // Only one .cpfont file is loaded; other sizes remain on disk. This keeps
  // resident interval + kern/ligature tables to one size's worth of memory
  // (see PR #1327 discussion re: Literata OOM).
  // Returns true on success.
  // onColdLoad (if set) is invoked once, just before the slow path that writes the
  // font into the flash partition (i.e. only on a genuine first load, not on a
  // flash-cache mmap hit) — callers use it to show a "loading font" popup.
  // Under FlashCachePolicy::ReadOnly that slow path is skipped entirely and
  // onColdLoad never fires.
  // `arena`, when set, is handed to the new SdCardFont (SdCardFont::useArena): the caller owns the
  // block and must unload this family before rewinding it. Only font previews pass one.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
                  const std::function<void()>& onColdLoad = {}, FlashCachePolicy policy = FlashCachePolicy::ReadWrite,
                  BuildArena* arena = nullptr);

  // Serve targetPtSize from the face that is loaded. A size the family ships is that face; any
  // other size is an ALIAS: a second font ID bound to the same face with a base scale of
  // target / loaded (GfxRenderer::insertScaledFont), so the reader renders the chosen typeface at
  // the chosen size instead of a built-in family. No .cpfont in existence ships 20-26 pt, so
  // every existing card reaches the larger sizes this way.
  //
  // One alias at a time: the reader wants one body size, and the ID is derived from
  // (content hash, family, target size) so a paginated section keyed on it is valid across
  // sessions. Returns false when nothing is loaded or the derived ID collides with a registered
  // font, in which case that size resolves to 0 and the caller falls back.
  bool ensureSizeAlias(GfxRenderer& renderer, uint8_t targetPtSize);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Font ID that renders the loaded family AT pointSize: the face itself when that is the loaded
  // size, the alias when one was made for it, else 0 -- never a nearby size. The setting says
  // "24pt" and the page has to be 24pt; a quiet 18pt is indistinguishable from the feature not
  // working for exactly the reader it exists for.
  int getFontId(const std::string& familyName, uint8_t pointSize) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded (closest match to targetPtSize).
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);
  void dropSizeAlias(GfxRenderer& renderer);

  GfxRenderer* renderer_ = nullptr;
  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
  // The scaled alias for the current target size (see ensureSizeAlias); 0 when the loaded face
  // serves the target size itself.
  int aliasFontId_ = 0;
  uint8_t aliasPointSize_ = 0;
};
