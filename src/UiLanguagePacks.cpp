#include "UiLanguagePacks.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SdCardFontRegistry.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "HyphenationPacks.h"
#include "util/FontCatalog.h"

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

const char* languageDeclineKey(const char* code, char (&key)[3]) {
  key[0] = static_cast<char>(code && code[0] ? std::toupper(static_cast<unsigned char>(code[0])) : 0);
  key[1] = static_cast<char>(key[0] && code[1] ? std::tolower(static_cast<unsigned char>(code[1])) : 0);
  key[2] = '\0';
  return key;
}

PackOffer::Offer due(const bool chosen) {
  PackOffer::Offer offer;
  const char* ui = uiCode();
  // The language's own strings: without them the rest of the interface is English.
  char path[48];
  const Language language = I18N.getLanguage();
  if (I18n::packPath(language, path, sizeof(path)) && !I18n::packOnCard(language)) {
    // "/.crosspoint/lang/de.cplang" -> "de"
    const char* file = path + strlen(I18n::PACK_DIR) + 1;
    const size_t len = strcspn(file, ".");
    char key[3];
    if (len < sizeof(offer.language) && (chosen || !HyphenationPacks::declined(languageDeclineKey(file, key)))) {
      memcpy(offer.language, file, len);
      offer.language[len] = '\0';
    }
  }
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
    LOG_INF("LNG", "%s: offering language pack '%s', family '%s', hyphenation '%s'", ui, offer.language, offer.family,
            offer.hyphenation);
  }
  return offer;
}

bool usedByInterface(const FontCatalog& families, const size_t family) {
  const char* ui = uiCode();
  if (!ui) return false;
  if (families.languagePack(family)) {
    // The language's strings: a file of the pack where I18n reads the language from.
    char own[48];
    if (!I18n::packPath(I18N.getLanguage(), own, sizeof(own))) return false;
    for (size_t j = 0; j < families.fileCount(family); ++j) {
      char path[128];
      FontInstaller::buildPackPath(families, family, families.fileLocalName(family, j), path, sizeof(path));
      if (strcasecmp(path, own) == 0) return true;
    }
    return false;
  }
  for (const auto& pack : kPacks) {
    if (std::strcmp(ui, pack.uiCode) != 0) continue;
    if (pack.interfaceFamily) return std::strcmp(families.name(family), pack.interfaceFamily) == 0;
    if (!pack.hyphenation || !families.hyphenation(family)) return false;
    for (size_t j = 0; j < families.fileCount(family); ++j) {
      char code[3];
      if (HyphenationPacks::codeOfPackFile(families.fileLocalName(family, j), code) &&
          std::strcmp(code, pack.hyphenation) == 0) {
        return true;
      }
    }
    return false;
  }
  return false;
}

PackOffer::Offer takeHomeOffer() {
  if (I18N.getLanguage() == homeAsked) return {};
  homeAsked = I18N.getLanguage();
  return due();
}

}  // namespace UiLanguagePacks
