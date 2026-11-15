#include "LanguageRegistry.h"

#include <cstring>

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
#endif

namespace {

// Each language's rules: how its letters are told and lowered, its minima, and whether its breaks show a hyphen.
#define LATIN isLatinLetter, toLowerLatin
#define CYRILLIC isCyrillicLetter, toLowerCyrillic
#define INDIC isIndicLetter, toLowerIndic

#ifdef HYPHENATION_ALL_BUILTIN
LanguageHyphenator frenchHyphenator(fr_patterns, LATIN);
LanguageHyphenator germanHyphenator(de_patterns, LATIN);
LanguageHyphenator russianHyphenator(ru_patterns, CYRILLIC);
LanguageHyphenator spanishHyphenator(es_patterns, LATIN);
LanguageHyphenator italianHyphenator(it_patterns, LATIN);
LanguageHyphenator swedishHyphenator(sv_patterns, LATIN);
LanguageHyphenator ukrainianHyphenator(uk_patterns, CYRILLIC);
LanguageHyphenator polishHyphenator(pl_patterns, LATIN);
LanguageHyphenator portugueseHyphenator(pt_patterns, LATIN);
#define BUILT_IN_OR_PACK(hyphenator) &hyphenator
#else
#define BUILT_IN_OR_PACK(hyphenator) nullptr
#endif

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, LATIN, 3, 3);

// The CRCs are scripts/build_hyphenation_packs.py's of the generated headers: the packs it builds carry them. The
// languages after Portuguese never had a trie in the image. Their minima are hypher's (Lang::bounds); Hungarian's, from
// patterns hypher does not carry, are hyph-utf8's for the language.
const LanguageEntry kEntries[] = {
    {"english", "en", LATIN, 3, 3, true, &englishHyphenator, 0},
    {"french", "fr", LATIN, 2, 2, true, BUILT_IN_OR_PACK(frenchHyphenator), 0x00caa580u},
    {"german", "de", LATIN, 2, 2, true, BUILT_IN_OR_PACK(germanHyphenator), 0x095df494u},
    {"russian", "ru", CYRILLIC, 2, 2, true, BUILT_IN_OR_PACK(russianHyphenator), 0x28d49bcau},
    {"spanish", "es", LATIN, 2, 2, true, BUILT_IN_OR_PACK(spanishHyphenator), 0x615551eeu},
    {"italian", "it", LATIN, 2, 2, true, BUILT_IN_OR_PACK(italianHyphenator), 0xb9b3a903u},
    {"swedish", "sv", LATIN, 2, 2, true, BUILT_IN_OR_PACK(swedishHyphenator), 0xe62a5c73u},
    {"polish", "pl", LATIN, 2, 2, true, BUILT_IN_OR_PACK(polishHyphenator), 0x294a1f9du},
    {"ukrainian", "uk", CYRILLIC, 2, 2, true, BUILT_IN_OR_PACK(ukrainianHyphenator), 0x314316eau},
    {"portuguese", "pt", LATIN, 2, 2, true, BUILT_IN_OR_PACK(portugueseHyphenator), 0x544b09ffu},
    {"dutch", "nl", LATIN, 2, 2, true, nullptr, 0},
    {"czech", "cs", LATIN, 2, 2, true, nullptr, 0},
    {"danish", "da", LATIN, 2, 2, true, nullptr, 0},
    {"slovenian", "sl", LATIN, 2, 2, true, nullptr, 0},
    {"lithuanian", "lt", LATIN, 2, 2, true, nullptr, 0},
    {"catalan", "ca", LATIN, 2, 2, true, nullptr, 0},
    {"belarusian", "be", CYRILLIC, 2, 2, true, nullptr, 0},
    {"finnish", "fi", LATIN, 2, 2, true, nullptr, 0},
    {"turkish", "tr", LATIN, 2, 2, true, nullptr, 0},
    {"hungarian", "hu", LATIN, 2, 2, true, nullptr, 0},
    {"assamese", "as", INDIC, 2, 2, false, nullptr, 0},
    {"bengali", "bn", INDIC, 2, 2, false, nullptr, 0},
    {"gujarati", "gu", INDIC, 2, 2, false, nullptr, 0},
    {"hindi", "hi", INDIC, 2, 2, false, nullptr, 0},
    {"kannada", "kn", INDIC, 2, 2, false, nullptr, 0},
    {"malayalam", "ml", INDIC, 2, 2, false, nullptr, 0},
    {"marathi", "mr", INDIC, 2, 2, false, nullptr, 0},
    {"odia", "or", INDIC, 2, 2, false, nullptr, 0},
    {"punjabi", "pa", INDIC, 2, 2, false, nullptr, 0},
    {"sanskrit", "sa", INDIC, 2, 2, false, nullptr, 0},
    {"tamil", "ta", INDIC, 2, 2, false, nullptr, 0},
    {"telugu", "te", INDIC, 2, 2, false, nullptr, 0},
};
constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

#undef LATIN
#undef CYRILLIC
#undef INDIC
#undef BUILT_IN_OR_PACK

[[maybe_unused]] constexpr SerializedHyphenationPatterns kNoPatterns{0, nullptr, 0};

// The pack slots: a pack language's hyphenator, its rules copied from its entry, its patterns from the provider.
constexpr size_t kPackSlots = 2;
LanguageHyphenator packSlots[kPackSlots];
const LanguageEntry* slotOwner[kPackSlots] = {};

HyphenationPatternProvider packProvider = nullptr;
// The pack languages the provider had nothing for since the last forgetPackPatterns(): asked once rather than on
// every lookup, as a miss reads the store's header from flash. A bit per kEntries index.
uint32_t packMissing = 0;
static_assert(kEntryCount <= 32, "a bit per language");  // 32 now: the next language widens packMissing

// A pack language's patterns and their CRC, from the provider. False when its pack is not available.
bool askProvider(const LanguageEntry& entry, SerializedHyphenationPatterns& out, uint32_t& crc) {
  const uint32_t bit = 1u << static_cast<uint32_t>(&entry - kEntries);
  if (!packProvider || (packMissing & bit) != 0) return false;
  if (packProvider(entry.primaryTag, out, crc) && out.available()) return true;
  packMissing |= bit;
  return false;
}

// The slot holding `entry`'s patterns, null when none does.
LanguageHyphenator* slotOf(const LanguageEntry& entry) {
  for (size_t i = 0; i < kPackSlots; ++i) {
    if (slotOwner[i] == &entry) return &packSlots[i];
  }
  return nullptr;
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
  if (entry->builtIn) return entry->builtIn;
  if (LanguageHyphenator* held = slotOf(*entry)) return held;
  size_t free = kPackSlots;
  for (size_t i = 0; i < kPackSlots && free == kPackSlots; ++i) {
    if (!slotOwner[i]) free = i;
  }
  // Both slots hold other languages: this one is not hyphenated until the next book open frees them. A slot is never
  // taken from a language, so a hyphenator a caller holds keeps the language it was given.
  if (free == kPackSlots) return nullptr;
  SerializedHyphenationPatterns found{0, nullptr, 0};
  uint32_t crc = 0;
  if (!askProvider(*entry, found, crc)) return nullptr;
  packSlots[free] = LanguageHyphenator(found, entry->isLetter, entry->toLower, entry->minPrefix, entry->minSuffix,
                                       entry->insertsHyphen);
  slotOwner[free] = entry;
  return &packSlots[free];
}

LanguageEntryView getLanguageEntries() { return LanguageEntryView{kEntries, kEntryCount}; }

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
  for (size_t i = 0; i < kPackSlots; ++i) {
    packSlots[i].setPatterns(kNoPatterns);
    slotOwner[i] = nullptr;
  }
}
