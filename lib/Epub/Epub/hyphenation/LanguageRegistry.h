#pragma once

#include <cstddef>
#include <string>

#include "LanguageHyphenator.h"

// The languages the firmware hyphenates, and where each one's Liang trie lives.
//
// English's trie is compiled into the image. Every other language's comes from a hyphenation pack on the SD card
// (HyphenationPack.h): the board's provider (setHyphenationPatternProvider) maps it from the flash store on the C3
// and loads it into PSRAM on the S3. The language table itself -- how a language's letters are told and lowered, and
// its minima -- stays here: a pack carries data only.
//
// Host tests that lay out books define HYPHENATION_ALL_BUILTIN and get every trie compiled in, as before packs.
struct LanguageEntry {
  const char* cliName;
  const char* primaryTag;
  LanguageHyphenator* hyphenator;
  bool builtIn;  // its patterns are compiled into the image
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
// not one the firmware hyphenates or its pack is not available.
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

// Takes every pack language's patterns back: call before whatever the provider handed out is unmapped or freed. A
// hyphenator a caller still holds then finds no breaks rather than reading freed memory.
void forgetPackPatterns();

// What a section's cache key holds of the patterns a book in this language is laid out by (Section::BuildParams): 0
// for the patterns the image carried before packs -- English's, or a pack of exactly the bytes the image had -- so the
// layouts cached with them stay valid; the trie's CRC32 for any other pack; kNoPatternsLayoutKey when the language's
// pack is not available, as its layout then has no pattern breaks. 0 for a language the firmware does not hyphenate.
static constexpr uint32_t kNoPatternsLayoutKey = 1;
uint32_t hyphenationLayoutKey(const char* primaryTag);
