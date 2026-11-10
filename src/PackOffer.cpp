#include "PackOffer.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <string>

#include "CrossPointState.h"
#include "HyphenationPacks.h"
#include "ReadingFontScript.h"
#include "UiLanguagePacks.h"
#include "activities/Activity.h"
#include "activities/util/ConfirmationActivity.h"

namespace PackOffer {

namespace {

ConfirmationActivity::Question question(const Offer& offer) {
  ConfirmationActivity::Question question;
  char message[400];
  if (offer.language[0] != '\0') {
    // The interface language's own pack, and what else it needs in the same question.
    const char* name = I18N.getLanguageName(I18N.getLanguage());
    question.headline = name;
    const StrId text = offer.family[0] != '\0'        ? StrId::STR_LANGUAGE_INTERFACE_PACK_OFFER
                       : offer.hyphenation[0] != '\0' ? StrId::STR_LANGUAGE_HYPHENATION_OFFER
                                                      : StrId::STR_LANGUAGE_PACK_OFFER;
    snprintf(message, sizeof(message), I18N.get(text), name);
  } else if (offer.interfaceLanguage[0] != '\0') {
    // A CJK interface pack, offered alone: its languages hyphenate nothing.
    const char* name = I18N.getLanguageName(I18N.getLanguage());
    question.headline = name;
    snprintf(message, sizeof(message), tr(STR_INTERFACE_PACK_OFFER), name);
  } else if (offer.family[0] == '\0') {
    question.headline = tr(STR_HYPHENATION);
    snprintf(message, sizeof(message), tr(STR_HYPHENATION_PACK_OFFER),
             HyphenationPacks::languageName(offer.hyphenation));
  } else if (offer.hyphenation[0] == '\0') {
    question.headline = offer.family;
    snprintf(message, sizeof(message), tr(STR_READING_FONT_OFFER), offer.family);
  } else {
    question.headline = offer.family;
    snprintf(message, sizeof(message), tr(STR_READING_FONT_HYPHENATION_OFFER), offer.family,
             HyphenationPacks::languageName(offer.hyphenation));
  }
  question.message = message;
  question.messageMaxLines = 10;
  question.cancelLabel = StrId::STR_NO;
  question.acceptLabel = StrId::STR_DOWNLOAD;
  return question;
}

void decline(const Offer& offer) {
  if (offer.language[0] != '\0') {
    char key[3];
    HyphenationPacks::declineOffer(UiLanguagePacks::languageDeclineKey(offer.language, key));
  }
  if (offer.hyphenation[0] != '\0') HyphenationPacks::declineOffer(offer.hyphenation);
  if (offer.family[0] == '\0') return;
  if (offer.interfaceLanguage[0] != '\0') {
    HyphenationPacks::declineOffer(offer.interfaceLanguage);
    return;
  }
  const std::string script = ReadingFontScript::of(offer.family);
  if (ReadingFontScript::listed(APP_STATE.fontOffersDeclined, script)) return;
  ReadingFontScript::add(APP_STATE.fontOffersDeclined, script);
  APP_STATE.saveToFile();
}

}  // namespace

bool ask(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput, const Offer& offer,
         const FontDownloadActivity::ReturnTo returnTo, void (*const carryOn)(Activity& host)) {
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, question(offer));
  if (!confirmation) {
    LOG_ERR("OFR", "OOM: pack offer (%zu bytes)", sizeof(ConfirmationActivity));
    return false;
  }
  host.startActivityForResult(std::move(confirmation), [&host, &renderer, &mappedInput, offer, returnTo,
                                                        carryOn](const ActivityResult& result) {
    if (result.isCancelled) {
      decline(offer);
      carryOn(host);
      return;
    }
    FontDownloadActivity::Wanted wanted;
    snprintf(wanted.language, sizeof(wanted.language), "%s", offer.language);
    snprintf(wanted.pack, sizeof(wanted.pack), "%s", offer.hyphenation);
    snprintf(wanted.family, sizeof(wanted.family), "%s", offer.family);
    // In the asking screen's place, not over it: the download's TLS session needs the heap that screen holds.
    auto download = makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInput, wanted, returnTo);
    if (!download) {
      LOG_ERR("OFR", "OOM: Font Manager for the pack offer");
      carryOn(host);
      return;
    }
    activityManager.replaceActivity(std::move(download));
  });
  return true;
}

}  // namespace PackOffer
