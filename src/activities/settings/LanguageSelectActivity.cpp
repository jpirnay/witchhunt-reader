#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <iterator>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "UiLanguage.h"
#include "UiLanguagePacks.h"
#include "activities/ActivityManager.h"

namespace fui = freeink::ui;

LanguageSelectActivity::LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("LanguageSelect", renderer, mappedInput) {}

void LanguageSelectActivity::onEnter() {
  UiListActivity::onEnter();

  // Set current selection based on current language
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  nav.selected = (it != end) ? static_cast<int>(std::distance(begin, it)) : 0;

  for (int index = 0; index < totalItems; ++index) {
    fui::ListItem item;
    item.label = I18N.getLanguageName(static_cast<Language>(SORTED_LANGUAGE_INDICES[index]));
    if (SORTED_LANGUAGE_INDICES[index] == currentLang) item.value = tr(STR_SELECTED);
    item.actionValue = static_cast<int16_t>(index);
    rowItems[index] = item;
  }
}

const char* LanguageSelectActivity::headerTitle() const { return tr(STR_LANGUAGE); }

void LanguageSelectActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected = index;
  UiLanguage::apply(static_cast<Language>(SORTED_LANGUAGE_INDICES[index]));
  // The new language's pack, when the card lacks it: its hyphenation, or its CJK interface pack (UiLanguagePacks).
  const UiLanguagePacks::Offer offer = UiLanguagePacks::due();
  if (!offer) {
    finish();
    return;
  }
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, UiLanguagePacks::question(offer));
  if (!confirmation) {
    LOG_ERR("LNG", "OOM: language pack offer (%zu bytes)", sizeof(ConfirmationActivity));
    finish();
    return;
  }
  startActivityForResult(std::move(confirmation), [this, offer](const ActivityResult& result) {
    if (result.isCancelled) {
      UiLanguagePacks::decline(offer);
      finish();
      return;
    }
    // In Settings' place: leaving the Font Manager restarts into Settings.
    auto download = UiLanguagePacks::download(renderer, mappedInput, offer, /*toSettings=*/true);
    if (!download) {
      LOG_ERR("LNG", "OOM: Font Manager for the language pack offer");
      finish();
      return;
    }
    activityManager.replaceActivity(std::move(download));
  });
}

void LanguageSelectActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);
  auto props = listProps(screen);
  props.items = rowItems;
  props.count = totalItems;
  addList(screen, props);
}
