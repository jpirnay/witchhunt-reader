#pragma once

#include "PackOffer.h"

// What the interface language would have the reader download (asked with PackOffer::ask): its hyphenation pack
// (German, French, ... HyphenationPacks) or, for Chinese, Japanese and Korean, which have none, its CJK interface pack
// (CjkUiPacks.h), which draws book titles in its script outside a book. One or the other, never both.
//
// Asked when the language is picked in Settings, and by Home once per start and language, so a language set any other
// way (a reader upgraded from a firmware with its patterns built in, a restored settings file) is asked too.
namespace UiLanguagePacks {

// The offer for the interface language, when its pack is not on the card and the offer was not declined. A
// hyphenation pack only with hyphenation on.
PackOffer::Offer due();

// due(), once per start and interface language: what Home asks.
PackOffer::Offer takeHomeOffer();

}  // namespace UiLanguagePacks
