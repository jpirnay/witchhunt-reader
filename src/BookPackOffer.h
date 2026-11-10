#pragma once

#include <memory>
#include <string>

#include "FontInstaller.h"
#include "activities/util/ConfirmationActivity.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

// What a book's open offers to download before its first page: the hyphenation pack of its language
// (HyphenationPacks::bookOfferDue) and the reading font of its script, when the reader's font has none of it and no
// Noto family of it is installed (SdCardFontSystem::missingLanguageFamily: a Hindi book in Bookerly, neither
// NotoSerifDevanagari nor NotoSansDevanagari on the card). One question for both. No to it is remembered, for the pack
// by its language, for the font by its script, and neither is asked again; the Font Manager still has them.
namespace BookPackOffer {

struct Offer {
  char hyphenation[3] = {};                                  // the hyphenation pack's language, "" for none
  char family[FontInstaller::MAX_FAMILY_NAME_LEN + 1] = {};  // the reading font's family, "" for none
  explicit operator bool() const { return hyphenation[0] != '\0' || family[0] != '\0'; }
};

// At book open, once its fonts are chosen (ensureSdFontLoadedForPath): what the book in `languageTag` lacks. The pack
// only with `hyphenation` on for the book.
Offer due(const std::string& languageTag, bool hyphenation);

// The question that makes `offer`.
ConfirmationActivity::Question question(const Offer& offer);

// No to `offer`: neither is offered again (CrossPointState::hyphenationOffersDeclined, fontOffersDeclined).
void decline(const Offer& offer);

// Yes to `offer`: the Font Manager, downloading it as soon as its list is loaded. Leaving it restarts into the book
// (APP_STATE.openEpubPath). Null when the heap has no room for it.
std::unique_ptr<Activity> download(GfxRenderer& renderer, MappedInputManager& mappedInput, const Offer& offer);

}  // namespace BookPackOffer
