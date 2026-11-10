#include "BookPackOffer.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointState.h"
#include "HyphenationPacks.h"
#include "ReadingFontScript.h"
#include "SdCardFontGlobals.h"
#include "activities/settings/FontDownloadActivity.h"

namespace BookPackOffer {

Offer due(const std::string& languageTag, const bool hyphenation) {
  Offer offer;
  if (hyphenation) HyphenationPacks::bookOfferDue(languageTag, offer.hyphenation);
  const char* family = sdFontSystem.missingLanguageFamily();
  if (family[0] != '\0' && !ReadingFontScript::listed(APP_STATE.fontOffersDeclined, ReadingFontScript::of(family))) {
    snprintf(offer.family, sizeof(offer.family), "%s", family);
    LOG_INF("BPO", "%s: the book's script needs %s, offering it", languageTag.c_str(), family);
  }
  return offer;
}

ConfirmationActivity::Question question(const Offer& offer) {
  ConfirmationActivity::Question question;
  char message[400];
  const char* language = HyphenationPacks::languageName(offer.hyphenation);
  if (offer.family[0] == '\0') {
    question.headline = tr(STR_HYPHENATION);
    snprintf(message, sizeof(message), tr(STR_HYPHENATION_PACK_OFFER), language);
  } else if (offer.hyphenation[0] == '\0') {
    question.headline = offer.family;
    snprintf(message, sizeof(message), tr(STR_READING_FONT_OFFER), offer.family);
  } else {
    question.headline = offer.family;
    snprintf(message, sizeof(message), tr(STR_READING_FONT_HYPHENATION_OFFER), offer.family, language);
  }
  question.message = message;
  question.messageMaxLines = 10;
  question.cancelLabel = StrId::STR_NO;
  question.acceptLabel = StrId::STR_DOWNLOAD;
  return question;
}

void decline(const Offer& offer) {
  if (offer.hyphenation[0] != '\0') HyphenationPacks::declineOffer(offer.hyphenation);
  if (offer.family[0] == '\0') return;
  const std::string script = ReadingFontScript::of(offer.family);
  if (ReadingFontScript::listed(APP_STATE.fontOffersDeclined, script)) return;
  ReadingFontScript::add(APP_STATE.fontOffersDeclined, script);
  APP_STATE.saveToFile();
}

std::unique_ptr<Activity> download(GfxRenderer& renderer, MappedInputManager& mappedInput, const Offer& offer) {
  FontDownloadActivity::Wanted wanted;
  snprintf(wanted.pack, sizeof(wanted.pack), "%s", offer.hyphenation);
  snprintf(wanted.family, sizeof(wanted.family), "%s", offer.family);
  return std::make_unique<FontDownloadActivity>(renderer, mappedInput, wanted, FontDownloadActivity::ReturnTo::Book);
}

}  // namespace BookPackOffer
