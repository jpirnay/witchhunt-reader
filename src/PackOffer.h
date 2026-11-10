#pragma once

#include "FontInstaller.h"
#include "activities/settings/FontDownloadActivity.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

// The reader's offers to download what the card lacks, and the one way they are asked: Home's and the language
// picker's for the interface language (UiLanguagePacks), the reader's for a book (BookPackOffer). One question for
// what an offer holds; No is remembered and the offer is not made again (CrossPointState::hyphenationOffersDeclined,
// fontOffersDeclined), Yes opens the Font Manager in the asking screen's place, downloading it as soon as its list is
// loaded.
namespace PackOffer {

struct Offer {
  char hyphenation[3] = {};                                  // the hyphenation pack's language, "" for none
  char family[FontInstaller::MAX_FAMILY_NAME_LEN + 1] = {};  // the font family, "" for none
  // The interface language's code ("ZH") when `family` is its CJK interface pack, which No declines under that code;
  // "" when it is a book script's reading font, which No declines by its script (ReadingFontScript).
  char interfaceLanguage[3] = {};
  explicit operator bool() const { return hyphenation[0] != '\0' || family[0] != '\0'; }
};

// Asks `offer` over `host`, whose renderer and input the screens take. No declines it; Yes replaces the screens with
// the Font Manager, which restarts into `returnTo` when left. `carryOn(host)` runs when the reader goes on without the
// Font Manager: after No, or with no room for it on the heap. False, with nothing asked, when the question could not
// be made (no room for it either).
bool ask(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput, const Offer& offer,
         FontDownloadActivity::ReturnTo returnTo, void (*carryOn)(Activity& host));

}  // namespace PackOffer
