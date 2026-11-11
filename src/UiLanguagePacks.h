#pragma once

#include <cstddef>

#include "PackOffer.h"

class FontCatalog;

// What the interface language would have the reader download (asked with PackOffer::ask): its own pack, the strings
// of every language but English (LangPack.h), and its hyphenation pack (German, French, ... HyphenationPacks) or, for
// Chinese, Japanese and Korean, which have none, its CJK interface pack (CjkUiPacks.h), which draws book titles in its
// script outside a book. The language pack with either in one question, never hyphenation and an interface pack.
//
// Asked when the language is picked in Settings, and by Home once per start and language, so a language set any other
// way (a reader upgraded from a firmware with its patterns built in, a restored settings file, a card without the
// language's pack) is asked too.
namespace UiLanguagePacks {

// The offer for the interface language: each pack it needs that is not on the card and whose offer was not declined.
// A hyphenation pack only with hyphenation on. `chosen`: the reader just picked the language, so its own pack is
// offered even when an earlier offer of it was declined -- they cannot read the language without it.
PackOffer::Offer due(bool chosen = false);

// due(), once per start and interface language: what Home asks.
PackOffer::Offer takeHomeOffer();

// What a declined language pack offer is remembered by in CrossPointState::hyphenationOffersDeclined, in its one list
// of two-letter keys: the language's code capitalised ("De" from "de" or "DE"), where a hyphenation pack's is lower
// case ("de") and a CJK interface pack's upper case ("ZH"). Written into `key`, which it returns.
const char* languageDeclineKey(const char* code, char (&key)[3]);

// Whether the pack `family` of `families` is one the interface language uses: its strings, its hyphenation pack or its
// CJK interface pack. The web Font Manager says what deleting such a pack does before it deletes it.
bool usedByInterface(const FontCatalog& families, size_t family);

}  // namespace UiLanguagePacks
