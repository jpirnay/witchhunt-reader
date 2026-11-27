#include "UiLanguage.h"

#include "CjkUiFonts.h"
#include "activities/RenderLock.h"

void UiLanguage::apply(const Language language) {
  RenderLock lock;
  I18N.setLanguage(language);
  CjkUiFonts::setLanguage(I18N.getLanguage());
}

// What setup() does: I18N.loadSettings(), then setupDisplayAndFonts() hands the language to the CJK UI faces.
void UiLanguage::reload() {
  I18N.loadSettings();
  CjkUiFonts::setLanguage(I18N.getLanguage());
}
