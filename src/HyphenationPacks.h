#pragma once

#include <string>

// The hyphenation packs on the SD card, /.crosspoint/hyphenation/hyph-<code>.cphyph (HyphenationPack.h), and where
// the open book's comes from while it is read. English is in the image; every other language hyphenates by its pack,
// and a book whose language has none is laid out without pattern breaks, as a language the firmware does not know.
//
// - Without PSRAM (X3, X4): the packs' tries are copied into the flash store (HyphenationStore.h, the region below
//   the UI language slot) the first time a book needs them, and read in place through a mapping, as the image's own
//   tries were: the same bytes, the same flash cache, no RAM. A language is copied once (German: 206 KB).
// - With PSRAM (Murphy, T5S3, X4 Pro): the book's trie is read from its pack into PSRAM at book open.
namespace HyphenationPacks {

static constexpr const char* DIR = "/.crosspoint/hyphenation";

// Mounts the store and registers the registry's provider. After I18N, before the first book opens.
void begin();

// At book open, before any section build or background work: makes the patterns of the book's language available
// when its pack is on the card (copying a new or changed pack into the store first), and takes back those of every
// other language.
void prepareForBook(const std::string& languageTag);

}  // namespace HyphenationPacks
