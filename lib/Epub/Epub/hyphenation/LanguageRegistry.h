#pragma once

#include <cstddef>
#include <string>

#include "LanguageHyphenator.h"

// The languages the firmware hyphenates, and where each one's Liang trie lives.
//
// English's trie is compiled into the image. Every other language's comes from a hyphenation pack on the SD card
// (HyphenationPack.h): the board's provider (setHyphenationPatternProvider) maps it from the flash store on the C3
// and loads it into PSRAM on the S3. The language table itself -- how a language's letters are told and lowered, its
// minima, whether its breaks show a hyphen -- is const and stays here: a pack carries data only. A pack language is
// hyphenated by one of two pack slots, given to it when its patterns are first asked for: the open book's language,
// and one more (a page of another book), as the flash store maps two at once.
//
// Host tests that lay out books define HYPHENATION_ALL_BUILTIN and get the tries of the ten languages that were
// compiled in before packs compiled in again.
struct LanguageEntry {
  const char* cliName;
  const char* primaryTag;
  bool (*isLetter)(uint32_t);
  uint32_t (*toLower)(uint32_t);
  uint8_t minPrefix;
  uint8_t minSuffix;
  bool insertsHyphen;  // false for the Indic scripts: their lines break between syllables without a hyphen
  // The hyphenator of the patterns compiled into the image; null for a language whose patterns come from its pack.
  LanguageHyphenator* builtIn;
  // CRC32 of the trie the image carried for the language before packs, 0 for English (still carried) and for a
  // language that had none: a pack holding exactly those bytes lays books out as the image did
  // (hyphenationLayoutKey).
  uint32_t imageTrieCrc;
};

struct LanguageEntryView {
  const LanguageEntry* data;
  size_t size;

  const LanguageEntry* begin() const { return data; }
  const LanguageEntry* end() const { return data + size; }
};

// Returns the Liang-backed hyphenator for a given primary language tag (e.g., "en", "fr"), null when the language is
// not one the firmware hyphenates, its pack is not available, or both pack slots hold other languages.
const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag);

// The table entry for a primary tag ("de"), null when the firmware does not hyphenate the language.
const LanguageEntry* findLanguageEntry(const char* primaryTag);

// Exposes the list of supported languages primarily for tooling/tests.
LanguageEntryView getLanguageEntries();

// Where a pack language's patterns come from. Asked the first time a language without patterns is looked up, and
// again after forgetPackPatterns(); fills `out` and returns true when the pack is available. The patterns must stay
// valid until forgetPackPatterns().
// `crc32` is the trie's (the pack header's).
using HyphenationPatternProvider = bool (*)(const char* primaryTag, SerializedHyphenationPatterns& out,
                                            uint32_t& crc32);
void setHyphenationPatternProvider(HyphenationPatternProvider provider);

// Takes every pack language's patterns back and frees both pack slots: call before whatever the provider handed out
// is unmapped or freed. A hyphenator a caller still holds then finds no breaks rather than reading freed memory (until
// its slot is given to a language again, at the next book open).
void forgetPackPatterns();

// What a section's cache key holds of the patterns a book in this language is laid out by (Section::BuildParams): 0
// for the patterns the image carried before packs -- English's, or a pack of exactly the bytes the image had -- so the
// layouts cached with them stay valid; the trie's CRC32 for any other pack; kNoPatternsLayoutKey when the language's
// pack is not available, as its layout then has no pattern breaks. 0 for a language the firmware does not hyphenate.
static constexpr uint32_t kNoPatternsLayoutKey = 1;
uint32_t hyphenationLayoutKey(const char* primaryTag);
