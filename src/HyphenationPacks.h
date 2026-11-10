#pragma once

#include <functional>
#include <string>

// The hyphenation packs on the SD card (HyphenationPack.h), and where the open book's comes from while it is read.
// English is in the image; every other language hyphenates by its pack, and a book whose language has none is laid
// out without pattern breaks, as a language the firmware does not know.
//
// A pack is hyph-<code>.cphyph in /.crosspoint/hyphenation/, where the Font Manager installs it, or in CrossInk's
// /.crossink/hyphenation/: the files are the same, so a card that served CrossInk serves this firmware too. The card
// decides: a language whose pack is gone from it is not hyphenated, whatever the flash store still holds.
//
// - Without PSRAM (X3, X4): a pack's trie is copied into the flash store (HyphenationStore.h, the region below the UI
//   language slot) when the Font Manager installs it, or at the first book that needs it, and read in place through a
//   mapping, as the image's own tries were: the same bytes, the same flash cache, no RAM. German's copy takes about a
//   second, once.
// - With PSRAM (Murphy, T5S3, X4 Pro): the book's trie is read from its pack into PSRAM at book open.
namespace HyphenationPacks {

static constexpr const char* DIR = "/.crosspoint/hyphenation";
static constexpr const char* CROSSINK_DIR = "/.crossink/hyphenation";

// Mounts the store and registers the registry's provider. After I18N, before the first book opens.
void begin();

// At book open, before any section build or background work: makes the patterns of the book's language available
// when its pack is on the card, and takes back those of every other language. When the pack must first be copied into
// the flash store (it is new, or changed on the card), `beforeCopy` runs first, so the reader can say why it waits.
void prepareForBook(const std::string& languageTag, const std::function<void()>& beforeCopy = nullptr);

// The pack file of `code` the Font Manager installs into DIR: "hyph-de.cphyph" (and a pack's language from it, false
// for a name of another shape).
bool codeOfPackFile(const char* fileName, char (&code)[3]);

// After the Font Manager installed the pack of `code` into DIR: the language is available again, and on a board with
// a flash store its trie is copied in now, so the next book in it opens without the copy. Only into room the store
// has: no other language is dropped for it here (a book open will, if it must).
void packInstalled(const char* code);

// After the Font Manager deleted the pack of `code`: its books are laid out without its patterns from now on.
void packRemoved(const char* code);

// Whether the card holds the pack of `code`, in our folder or CrossInk's.
bool packOnCard(const char* code);

// The pack the reader offers to download, once a boot: that of the interface language, when it is not English, the
// firmware hyphenates it from a pack, the card has none, and the offer was not declined for it before. True with its
// code in `code` the first time a boot asks and the offer is due; false after that, and when it is not. A reader
// upgraded from a firmware that had its language's patterns built in is asked at its first boot.
bool takeOffer(char (&code)[3]);

// The offer of `code` was declined: it is not made again (CrossPointState::hyphenationOfferDeclined).
void declineOffer(const char* code);

}  // namespace HyphenationPacks
