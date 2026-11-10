#include "UiLanguagePacks.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SdCardFontRegistry.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "HyphenationPacks.h"
#include "activities/settings/FontDownloadActivity.h"

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

Offer due() {
  Offer offer;
  const char* ui = uiCode();
  for (const auto& pack : kPacks) {
    if (!ui || std::strcmp(ui, pack.uiCode) != 0) continue;
    if (pack.hyphenation && SETTINGS.hyphenationEnabled && HyphenationPacks::offerDue(pack.hyphenation)) {
      snprintf(offer.hyphenation, sizeof(offer.hyphenation), "%s", pack.hyphenation);
    }
    // An interface pack's offer is declined under the interface language's code, upper case: "ZH".
    if (pack.interfaceFamily && !HyphenationPacks::declined(pack.uiCode) &&
        !interfacePackOnCard(pack.interfaceFamily)) {
      offer.interfaceFamily = pack.interfaceFamily;
    }
  }
  if (offer) {
    LOG_INF("LNG", "%s: offering %s", ui, offer.interfaceFamily ? offer.interfaceFamily : offer.hyphenation);
  }
  return offer;
}

Offer takeHomeOffer() {
  if (I18N.getLanguage() == homeAsked) return {};
  homeAsked = I18N.getLanguage();
  return due();
}

ConfirmationActivity::Question question(const Offer& offer) {
  ConfirmationActivity::Question question;
  char message[320];
  if (offer.hyphenation[0] != '\0') {
    question.headline = tr(STR_HYPHENATION);
    snprintf(message, sizeof(message), tr(STR_HYPHENATION_PACK_OFFER),
             HyphenationPacks::languageName(offer.hyphenation));
  } else {
    question.headline = I18N.getLanguageName(I18N.getLanguage());
    snprintf(message, sizeof(message), tr(STR_INTERFACE_PACK_OFFER), I18N.getLanguageName(I18N.getLanguage()));
  }
  question.message = message;
  question.messageMaxLines = 6;
  question.cancelLabel = StrId::STR_NO;
  question.acceptLabel = StrId::STR_DOWNLOAD;
  return question;
}

void decline(const Offer& offer) {
  if (offer.hyphenation[0] != '\0') HyphenationPacks::declineOffer(offer.hyphenation);
  if (offer.interfaceFamily) HyphenationPacks::declineOffer(uiCode());
}

std::unique_ptr<Activity> download(GfxRenderer& renderer, MappedInputManager& mappedInput, const Offer& offer,
                                   const bool toSettings) {
  FontDownloadActivity::Wanted wanted;
  if (offer.hyphenation[0] != '\0') {
    snprintf(wanted.pack, sizeof(wanted.pack), "%s", offer.hyphenation);
  } else {
    wanted.family = offer.interfaceFamily;
  }
  return std::make_unique<FontDownloadActivity>(
      renderer, mappedInput, wanted,
      toSettings ? FontDownloadActivity::ReturnTo::Settings : FontDownloadActivity::ReturnTo::Home);
}

}  // namespace UiLanguagePacks
