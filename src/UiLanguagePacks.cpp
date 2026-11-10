#include "UiLanguagePacks.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SdCardFontRegistry.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "HyphenationPacks.h"

namespace UiLanguagePacks {

namespace {

// Each interface language's pack. The hyphenation pack of the languages whose patterns come from one (Romanian and
// Kazakh have no patterns; English is built in; Vietnamese is not hyphenated), the CJK interface pack of Chinese,
// Japanese and Korean, which hyphenate none.
struct LanguagePack {
  const char* uiCode;  // the translation's _language_code
  const char* hyphenation;
  const char* interfaceFamily;
};
constexpr LanguagePack kPacks[] = {
    {"DE", "de", nullptr},        {"FR", "fr", nullptr},         {"ES", "es", nullptr},      {"IT", "it", nullptr},
    {"PT", "pt", nullptr},        {"PO", "pt", nullptr},         {"PL", "pl", nullptr},      {"RU", "ru", nullptr},
    {"UK", "uk", nullptr},        {"SV", "sv", nullptr},         {"NL", "nl", nullptr},      {"CS", "cs", nullptr},
    {"DA", "da", nullptr},        {"SI", "sl", nullptr},         {"LT", "lt", nullptr},      {"CA", "ca", nullptr},
    {"BE", "be", nullptr},        {"FI", "fi", nullptr},         {"TR", "tr", nullptr},      {"HU", "hu", nullptr},
    {"ZH", nullptr, "ChineseUI"}, {"JA", nullptr, "JapaneseUI"}, {"KO", nullptr, "KoreanUI"}};

const char* uiCode() { return I18N.getLanguageCode(I18N.getLanguage()); }

bool interfacePackOnCard(const char* family) {
  char path[64];
  snprintf(path, sizeof(path), "%s/%s", SdCardFontRegistry::FONTS_DIR, family);
  return Storage.exists(path);
}

Language homeAsked = Language::_COUNT;

}  // namespace

PackOffer::Offer due() {
  PackOffer::Offer offer;
  const char* ui = uiCode();
  for (const auto& pack : kPacks) {
    if (!ui || std::strcmp(ui, pack.uiCode) != 0) continue;
    if (pack.hyphenation && SETTINGS.hyphenationEnabled && HyphenationPacks::offerDue(pack.hyphenation)) {
      snprintf(offer.hyphenation, sizeof(offer.hyphenation), "%s", pack.hyphenation);
    }
    // An interface pack's offer is declined under the interface language's code, upper case: "ZH".
    if (pack.interfaceFamily && !HyphenationPacks::declined(pack.uiCode) &&
        !interfacePackOnCard(pack.interfaceFamily)) {
      snprintf(offer.family, sizeof(offer.family), "%s", pack.interfaceFamily);
      snprintf(offer.interfaceLanguage, sizeof(offer.interfaceLanguage), "%s", pack.uiCode);
    }
  }
  if (offer) {
    LOG_INF("LNG", "%s: offering %s", ui, offer.family[0] != '\0' ? offer.family : offer.hyphenation);
  }
  return offer;
}

PackOffer::Offer takeHomeOffer() {
  if (I18N.getLanguage() == homeAsked) return {};
  homeAsked = I18N.getLanguage();
  return due();
}

}  // namespace UiLanguagePacks
