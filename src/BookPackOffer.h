#pragma once

#include <string>

#include "PackOffer.h"

// What a book's open offers to download before its first page: the hyphenation pack of its language
// (HyphenationPacks::bookOfferDue) and the reading font of its script, when the reader's font has none of it and no
// Noto family of it is installed (SdCardFontSystem::missingLanguageFamily: a Hindi book in Bookerly, neither
// NotoSerifDevanagari nor NotoSansDevanagari on the card). One question for both (PackOffer::ask). No to it is
// remembered, for the pack by its language, for the font by its script, and neither is asked again; the Font Manager
// still has them.
namespace BookPackOffer {

// At book open, once its fonts are chosen (ensureSdFontLoadedForPath): what the book in `languageTag` lacks. The pack
// only with `hyphenation` on for the book.
PackOffer::Offer due(const std::string& languageTag, bool hyphenation);

}  // namespace BookPackOffer
