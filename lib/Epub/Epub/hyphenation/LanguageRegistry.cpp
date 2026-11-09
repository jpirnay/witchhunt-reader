#include "LanguageRegistry.h"

#include <array>
#include <cstring>
#include <tuple>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"
#ifdef HYPHENATION_ALL_BUILTIN
#include "generated/hyph-de.trie.h"
#include "generated/hyph-es.trie.h"
#include "generated/hyph-fr.trie.h"
#include "generated/hyph-it.trie.h"
#include "generated/hyph-pl.trie.h"
#include "generated/hyph-pt.trie.h"
#include "generated/hyph-ru.trie.h"
#include "generated/hyph-sv.trie.h"
#include "generated/hyph-uk.trie.h"
#define PACK_PATTERNS(code) code##_patterns
#define PACK_BUILT_IN true
#else
#define PACK_PATTERNS(code) kNoPatterns
#define PACK_BUILT_IN false
#endif

namespace {

[[maybe_unused]] constexpr SerializedHyphenationPatterns kNoPatterns{0, nullptr, 0};

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
LanguageHyphenator frenchHyphenator(PACK_PATTERNS(fr), isLatinLetter, toLowerLatin);
LanguageHyphenator germanHyphenator(PACK_PATTERNS(de), isLatinLetter, toLowerLatin);
LanguageHyphenator russianHyphenator(PACK_PATTERNS(ru), isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator spanishHyphenator(PACK_PATTERNS(es), isLatinLetter, toLowerLatin);
LanguageHyphenator italianHyphenator(PACK_PATTERNS(it), isLatinLetter, toLowerLatin);
LanguageHyphenator swedishHyphenator(PACK_PATTERNS(sv), isLatinLetter, toLowerLatin);
LanguageHyphenator ukrainianHyphenator(PACK_PATTERNS(uk), isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator polishHyphenator(PACK_PATTERNS(pl), isLatinLetter, toLowerLatin);
LanguageHyphenator portugueseHyphenator(PACK_PATTERNS(pt), isLatinLetter, toLowerLatin);

using EntryArray = std::array<LanguageEntry, 10>;

// The CRCs are scripts/build_hyphenation_packs.py's of the generated headers: the packs it builds carry them.
const EntryArray kEntries = {{{"english", "en", &englishHyphenator, true, 0},
                              {"french", "fr", &frenchHyphenator, PACK_BUILT_IN, 0x00caa580u},
                              {"german", "de", &germanHyphenator, PACK_BUILT_IN, 0x095df494u},
                              {"russian", "ru", &russianHyphenator, PACK_BUILT_IN, 0x28d49bcau},
                              {"spanish", "es", &spanishHyphenator, PACK_BUILT_IN, 0x615551eeu},
                              {"italian", "it", &italianHyphenator, PACK_BUILT_IN, 0xb9b3a903u},
                              {"swedish", "sv", &swedishHyphenator, PACK_BUILT_IN, 0xe62a5c73u},
                              {"polish", "pl", &polishHyphenator, PACK_BUILT_IN, 0x294a1f9du},
                              {"ukrainian", "uk", &ukrainianHyphenator, PACK_BUILT_IN, 0x314316eau},
                              {"portuguese", "pt", &portugueseHyphenator, PACK_BUILT_IN, 0x544b09ffu}}};

HyphenationPatternProvider packProvider = nullptr;
// The pack languages the provider had nothing for since the last forgetPackPatterns(): asked once rather than on
// every lookup, as a miss reads the store's header from flash. A bit per kEntries index.
uint16_t packMissing = 0;
static_assert(std::tuple_size<EntryArray>::value <= 16, "a bit per language");

// A pack language's patterns and their CRC, from the provider. False when its pack is not available.
bool askProvider(const LanguageEntry& entry, SerializedHyphenationPatterns& out, uint32_t& crc) {
  const auto bit = static_cast<uint16_t>(1u << (&entry - kEntries.data()));
  if (!packProvider || (packMissing & bit) != 0) return false;
  if (packProvider(entry.primaryTag, out, crc) && out.data && out.size > 0) return true;
  packMissing |= bit;
  return false;
}

}  // namespace

const LanguageEntry* findLanguageEntry(const char* primaryTag) {
  if (!primaryTag) return nullptr;
  for (const auto& entry : kEntries) {
    if (std::strcmp(primaryTag, entry.primaryTag) == 0) return &entry;
  }
  return nullptr;
}

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const LanguageEntry* entry = findLanguageEntry(primaryTag.c_str());
  if (!entry) return nullptr;
  LanguageHyphenator* hyphenator = entry->hyphenator;
  if (!hyphenator->hasPatterns() && !entry->builtIn) {
    SerializedHyphenationPatterns found{0, nullptr, 0};
    uint32_t crc = 0;
    if (askProvider(*entry, found, crc)) hyphenator->setPatterns(found);
  }
  return hyphenator->hasPatterns() ? hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() { return LanguageEntryView{kEntries.data(), kEntries.size()}; }

void setHyphenationPatternProvider(const HyphenationPatternProvider provider) {
  packProvider = provider;
  packMissing = 0;
}

uint32_t hyphenationLayoutKey(const char* primaryTag) {
  const LanguageEntry* entry = findLanguageEntry(primaryTag);
  if (!entry || entry->builtIn) return 0;
  SerializedHyphenationPatterns found{0, nullptr, 0};
  uint32_t crc = 0;
  if (!askProvider(*entry, found, crc)) return kNoPatternsLayoutKey;
  return crc == entry->imageTrieCrc ? 0 : crc;
}

void forgetPackPatterns() {
  packMissing = 0;
  for (const auto& entry : kEntries) {
    if (!entry.builtIn) entry.hyphenator->setPatterns(kNoPatterns);
  }
}
