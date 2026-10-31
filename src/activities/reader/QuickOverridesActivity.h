#pragma once
#include <I18n.h>

#include <string>

#include "../MenuListActivity.h"
#include "ReaderFontSizes.h"

// Bottom-of-the-toolbox quick-overrides menu: shows only the per-book overrides
// (font family, size, Publisher Fonts, embedded style, image rendering, bionic, guide dots,
// paragraph alignment, line spacing, anti-aliasing, hyphenation). Optimised for snappy in-book toggling:
// renders in FAST_REFRESH mode and reports back through MenuResult; the parent
// reader applies the result through its existing applyBookReaderOverrides()
// path, which itself short-circuits when nothing changed.
class QuickOverridesActivity final : public MenuListActivity {
 public:
  QuickOverridesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int8_t initialEmbeddedStyleOverride,
                         int8_t initialImageRenderingOverride, int8_t initialFontFamilyOverride,
                         const std::string& initialSdFontFamilyOverride, int8_t initialFontSizeOverride,
                         int8_t initialBionicReadingOverride, int8_t initialGuideDotsOverride,
                         int8_t initialParagraphAlignmentOverride, int8_t initialTextAntiAliasingOverride,
                         int8_t initialHyphenationOverride, int8_t initialFontSizeNormalizationOverride,
                         int8_t initialInlineFootnotePreviewsOverride, int8_t initialLineSpacingOverride,
                         int8_t initialVerticalTextOverride, int8_t initialPublisherFontsOverride,
                         bool verticalAllowed);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  void buildMenuItems(bool verticalAllowed);
  void finishWithResult(bool cancelled);

  void onBackPressed() override;
  void onSettingToggled(int index) override;

  int8_t pendingEmbeddedStyleOverride = -1;
  int8_t pendingImageRenderingOverride = -1;
  int8_t pendingFontFamilyOverride = -1;
  std::string pendingSdFontFamilyOverride;
  int8_t pendingFontSizeOverride = -1;
  // The sizes the pending font family offers. The size row lists these, and a pending size
  // override is one of them (see ReaderSizeList).
  ReaderSizeList pendingSizeList() const;
  int8_t pendingBionicReadingOverride = -1;
  int8_t pendingGuideDotsOverride = -1;
  int8_t pendingParagraphAlignmentOverride = -1;
  int8_t pendingTextAntiAliasingOverride = -1;
  int8_t pendingHyphenationOverride = -1;
  int8_t pendingFontSizeNormalizationOverride = -1;
  int8_t pendingInlineFootnotePreviewsOverride = -1;
  int8_t pendingLineSpacingOverride = -1;
  int8_t pendingVerticalTextOverride = -1;
  int8_t pendingPublisherFontsOverride = -1;
};
