#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "GermanHybridHyphenator.h"
#include "HyphenationCommon.h"
#include "LiangLanguageHyphenator.h"
#include "generated/hyph-en.trie.h"
#include "generated/hyph-es.trie.h"
#include "generated/hyph-fr.trie.h"
#include "generated/hyph-it.trie.h"
#include "generated/hyph-pl.trie.h"
#include "generated/hyph-ru.trie.h"
#include "generated/hyph-sv.trie.h"
#include "generated/hyph-uk.trie.h"

namespace {

LiangLanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);

LiangLanguageHyphenator frenchHyphenator(fr_patterns, isLatinLetter, toLowerLatin);

GermanHybridHyphenator germanHyphenator;

LiangLanguageHyphenator russianHyphenator(ru_patterns, isCyrillicLetter, toLowerCyrillic);

LiangLanguageHyphenator spanishHyphenator(es_patterns, isLatinLetter, toLowerLatin);

LiangLanguageHyphenator italianHyphenator(it_patterns, isLatinLetter, toLowerLatin);

LiangLanguageHyphenator swedishHyphenator(sv_patterns, isLatinLetter, toLowerLatin);

LiangLanguageHyphenator ukrainianHyphenator(uk_patterns, isCyrillicLetter, toLowerCyrillic);

LiangLanguageHyphenator polishHyphenator(pl_patterns, isLatinLetter, toLowerLatin);

using EntryArray = std::array<LanguageEntry, 9>;

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator},
                                       {"french", "fr", &frenchHyphenator},
                                       {"german", "de", &germanHyphenator},
                                       {"russian", "ru", &russianHyphenator},
                                       {"spanish", "es", &spanishHyphenator},
                                       {"italian", "it", &italianHyphenator},
                                       {"swedish", "sv", &swedishHyphenator},
                                       {"polish", "pl", &polishHyphenator},
                                       {"ukrainian", "uk", &ukrainianHyphenator}}};
  return kEntries;
}

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != allEntries.end()) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
