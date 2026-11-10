#include "BookPackOffer.h"

#include <Logging.h>

#include <cstdio>

#include "CrossPointState.h"
#include "HyphenationPacks.h"
#include "ReadingFontScript.h"
#include "SdCardFontGlobals.h"

namespace BookPackOffer {

PackOffer::Offer due(const std::string& languageTag, const bool hyphenation) {
  PackOffer::Offer offer;
  if (hyphenation) HyphenationPacks::bookOfferDue(languageTag, offer.hyphenation);
  const char* family = sdFontSystem.missingLanguageFamily();
  if (family[0] != '\0' && !ReadingFontScript::listed(APP_STATE.fontOffersDeclined, ReadingFontScript::of(family))) {
    snprintf(offer.family, sizeof(offer.family), "%s", family);
    LOG_INF("BPO", "%s: the book's script needs %s, offering it", languageTag.c_str(), family);
  }
  return offer;
}

}  // namespace BookPackOffer
