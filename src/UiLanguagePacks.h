#pragma once

#include <memory>

#include "activities/util/ConfirmationActivity.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

// What the interface language would have the reader download, and the question that offers it: its hyphenation pack
// (German, French, ... HyphenationPacks) or, for Chinese, Japanese and Korean, which have none, its CJK interface pack
// (CjkUiPacks.h), which draws book titles in its script outside a book. One or the other, never both.
//
// Asked when the language is picked in Settings, and by Home once per start and language, so a language set any other
// way (a reader upgraded from a firmware with its patterns built in, a restored settings file) is asked too.
namespace UiLanguagePacks {

struct Offer {
  char hyphenation[3] = {};               // the hyphenation pack's language, "" for none
  const char* interfaceFamily = nullptr;  // the CJK interface pack's family, null for none
  explicit operator bool() const { return hyphenation[0] != '\0' || interfaceFamily != nullptr; }
};

// The offer for the interface language, when its pack is not on the card and the offer was not declined. A
// hyphenation pack only with hyphenation on.
Offer due();

// due(), once per start and interface language: what Home asks.
Offer takeHomeOffer();

// The question that makes `offer`.
ConfirmationActivity::Question question(const Offer& offer);

// No to `offer`: not offered again (CrossPointState::hyphenationOffersDeclined).
void decline(const Offer& offer);

// Yes to `offer`: the Font Manager, downloading it as soon as its list is loaded. Leaving it restarts into Settings
// when `toSettings`, else Home.
std::unique_ptr<Activity> download(GfxRenderer& renderer, MappedInputManager& mappedInput, const Offer& offer,
                                   bool toSettings);

}  // namespace UiLanguagePacks
