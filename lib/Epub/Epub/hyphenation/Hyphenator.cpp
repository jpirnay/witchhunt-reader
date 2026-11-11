#include "Hyphenator.h"

#include <ArabicScripts.h>
#include <IndicScripts.h>
#include <MyanmarScript.h>
#include <Utf8.h>

#include <algorithm>
#include <cassert>
#include <vector>

#include "HyphenationCommon.h"
#include "LanguageHyphenator.h"
#include "LanguageRegistry.h"

struct Iso639Mapping {
  const char* iso639_2;
  const char* iso639_1;
};
static constexpr Iso639Mapping kIso639Mappings[] = {
    {"eng", "en"}, {"fra", "fr"}, {"fre", "fr"}, {"deu", "de"}, {"ger", "de"}, {"rus", "ru"},
    {"spa", "es"}, {"ita", "it"}, {"ukr", "uk"}, {"swe", "sv"}, {"pol", "pl"}, {"por", "pt"},
    {"nld", "nl"}, {"dut", "nl"}, {"ces", "cs"}, {"cze", "cs"}, {"dan", "da"}, {"slv", "sl"},
    {"lit", "lt"}, {"cat", "ca"}, {"bel", "be"}, {"fin", "fi"}, {"tur", "tr"}, {"asm", "as"},
    {"ben", "bn"}, {"guj", "gu"}, {"hin", "hi"}, {"kan", "kn"}, {"mal", "ml"}, {"mar", "mr"},
    {"ori", "or"}, {"ory", "or"}, {"pan", "pa"}, {"san", "sa"}, {"tam", "ta"}, {"tel", "te"}};

const LanguageHyphenator* Hyphenator::cachedHyphenator_ = nullptr;

namespace {

// The registry's entry for a language tag: its primary subtag, lowered, with ISO 639-2 codes mapped to 639-1.
const LanguageEntry* languageEntryFor(const std::string& langTag) {
  if (langTag.empty()) return nullptr;

  std::string primary;
  primary.reserve(langTag.size());
  for (char c : langTag) {
    if (c == '-' || c == '_') break;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    primary.push_back(c);
  }
  if (primary.empty()) return nullptr;

  for (const auto& mapping : kIso639Mappings) {
    if (primary == mapping.iso639_2) {
      return findLanguageEntry(mapping.iso639_1);
    }
  }

  return findLanguageEntry(primary.c_str());
}

const LanguageHyphenator* hyphenatorForLanguage(const std::string& langTag) {
  const LanguageEntry* entry = languageEntryFor(langTag);
  return entry ? getLanguageHyphenatorForPrimaryTag(entry->primaryTag) : nullptr;
}

// Maps a codepoint index back to its byte offset inside the source word.
size_t byteOffsetForIndex(const std::vector<CodepointInfo>& cps, const size_t index) {
  return (index < cps.size()) ? cps[index].byteOffset : (cps.empty() ? 0 : cps.back().byteOffset);
}

// Builds a vector of break information from explicit hyphen markers in the given codepoints.
// Only hyphens that appear between two alphabetic characters are considered valid breaks.
//
// Example: "US-Satellitensystems" (cps: U, S, -, S, a, t, ...)
//   -> finds '-' at index 2 with alphabetic neighbors 'S' and 'S'
//   -> returns one BreakInfo at the byte offset of 'S' (the char after '-'),
//      with requiresInsertedHyphen=false because '-' is already visible.
//
// Example: "Satel\u00ADliten" (soft-hyphen between 'l' and 'l')
//   -> returns one BreakInfo with requiresInsertedHyphen=true (soft-hyphen
//      is invisible and needs a visible '-' when the break is used).
void appendExplicitBreakInfos(const std::vector<CodepointInfo>& cps, std::vector<Hyphenator::BreakInfo>& breaks) {
  for (size_t i = 1; i + 1 < cps.size(); ++i) {
    const uint32_t cp = cps[i].value;
    if (!isExplicitHyphen(cp) || !isAlphabetic(cps[i - 1].value) || !isAlphabetic(cps[i + 1].value)) {
      continue;
    }
    // Offset points to the next codepoint so rendering starts after the hyphen marker.
    breaks.push_back({cps[i + 1].byteOffset, isSoftHyphen(cp)});
  }
}

// The Arabic part of the fallback rule, and only where Arabic is: never before a mark, never before a joiner that
// follows an Arabic letter or mark, never after a ZWJ that follows one. A Latin or emoji joiner keeps its breaks.
bool arabicBreakAllowed(const std::vector<CodepointInfo>& cps, const size_t idx) {
  const uint32_t prev = cps[idx - 1].value;
  const uint32_t cur = cps[idx].value;
  if (arabic::isMark(cur)) return false;
  if ((cur == arabic::ZWJ || cur == arabic::ZWNJ) && arabic::takesJoiner(prev)) return false;
  if (prev == arabic::ZWJ && idx >= 2 && arabic::takesJoiner(cps[idx - 2].value)) return false;
  return true;
}

// The Myanmar part of the fallback rule (myanmar::cutAllowed): a Myanmar word is cut between syllables only, and its
// joiners stay with the letter before them. The rule reads the code point after cps[idx] past any dot below.
bool myanmarBreakAllowed(const std::vector<CodepointInfo>& cps, const size_t idx) {
  size_t next = idx + 1;
  while (next < cps.size() && cps[next].value == myanmar::DOT_BELOW) ++next;
  return myanmar::cutAllowed(cps[idx - 1].value, cps[idx].value, next < cps.size() ? cps[next].value : 0);
}

// Fallback break indexes for a word the language patterns could not split:
// every index leaving minPrefix/minSuffix codepoints on each side, except
// inside an Indic syllable, whose halves would each shape on their own, inside an Arabic cluster (before a mark
// or a joiner, after a ZWJ), which would start the next line with a mark, and inside a Myanmar syllable.
// Ported from crosspoint-reader PR #3787 ("feat: add Indic language support without HarfBuzz", SSafayet /
// @SSafayet).
void appendFallbackIndexes(const std::vector<CodepointInfo>& cps, const size_t minPrefix, const size_t minSuffix,
                           std::vector<size_t>& out) {
  for (size_t idx = minPrefix; idx + minSuffix <= cps.size(); ++idx) {
    if (idx == 0 || (indic::syllableBreakAllowed(cps[idx - 1].value, cps[idx].value) && arabicBreakAllowed(cps, idx) &&
                     myanmarBreakAllowed(cps, idx))) {
      out.push_back(idx);
    }
  }
}

bool isSegmentSeparator(const uint32_t cp) { return isExplicitHyphen(cp) || isApostrophe(cp); }

// Whether a break before cps[idx] shows a hyphen: not between two letters of a language whose lines break without one
// (the Indic scripts). A Latin word in such a book keeps its hyphen.
bool showsHyphen(const LanguageHyphenator* hyphenator, const std::vector<CodepointInfo>& cps, const size_t idx) {
  if (!hyphenator || hyphenator->insertsHyphen() || idx == 0 || idx >= cps.size()) return true;
  return !(hyphenator->isLetter(cps[idx - 1].value) && hyphenator->isLetter(cps[idx].value));
}

void appendSegmentPatternBreaks(const std::vector<CodepointInfo>& cps, const LanguageHyphenator& hyphenator,
                                const bool includeFallback, std::vector<Hyphenator::BreakInfo>& outBreaks) {
  size_t segStart = 0;

  for (size_t i = 0; i <= cps.size(); ++i) {
    const bool atEnd = i == cps.size();
    const bool atSeparator = !atEnd && isSegmentSeparator(cps[i].value);
    if (!atEnd && !atSeparator) {
      continue;
    }

    if (i > segStart) {
      std::vector<CodepointInfo> segment(cps.begin() + segStart, cps.begin() + i);
      auto segIndexes = hyphenator.breakIndexes(segment);

      if (includeFallback && segIndexes.empty()) {
        const size_t minPrefix = hyphenator.minPrefix();
        const size_t minSuffix = hyphenator.minSuffix();
        appendFallbackIndexes(segment, minPrefix, minSuffix, segIndexes);
      }

      for (const size_t idx : segIndexes) {
        assert(idx > 0 && idx < segment.size());
        if (idx == 0 || idx >= segment.size()) continue;
        const size_t cpIdx = segStart + idx;
        if (cpIdx < cps.size()) {
          outBreaks.push_back({cps[cpIdx].byteOffset, showsHyphen(&hyphenator, cps, cpIdx)});
        }
      }
    }

    segStart = i + 1;
  }
}

void appendApostropheContractionBreaks(const std::vector<CodepointInfo>& cps,
                                       std::vector<Hyphenator::BreakInfo>& outBreaks) {
  constexpr size_t kMinLeftSegmentLen = 3;
  constexpr size_t kMinRightSegmentLen = 3;
  size_t segmentStart = 0;

  for (size_t i = 0; i < cps.size(); ++i) {
    if (isSegmentSeparator(cps[i].value)) {
      if (isApostrophe(cps[i].value) && i > 0 && i + 1 < cps.size() && isAlphabetic(cps[i - 1].value) &&
          isAlphabetic(cps[i + 1].value)) {
        size_t leftPrefixLen = 0;
        for (size_t j = segmentStart; j < i; ++j) {
          if (isAlphabetic(cps[j].value)) {
            ++leftPrefixLen;
          }
        }

        size_t rightSuffixLen = 0;
        for (size_t j = i + 1; j < cps.size() && !isSegmentSeparator(cps[j].value); ++j) {
          if (isAlphabetic(cps[j].value)) {
            ++rightSuffixLen;
          }
        }

        // Avoid stranding short clitics like "l'"/"d'" or contraction tails like "'ve"/"'re"/"'ll".
        if (leftPrefixLen >= kMinLeftSegmentLen && rightSuffixLen >= kMinRightSegmentLen) {
          outBreaks.push_back({cps[i + 1].byteOffset, false});
        }
      }
      segmentStart = i + 1;
    }
  }
}

void sortAndDedupeBreakInfos(std::vector<Hyphenator::BreakInfo>& infos) {
  std::sort(infos.begin(), infos.end(), [](const Hyphenator::BreakInfo& a, const Hyphenator::BreakInfo& b) {
    if (a.byteOffset != b.byteOffset) {
      return a.byteOffset < b.byteOffset;
    }
    return a.requiresInsertedHyphen < b.requiresInsertedHyphen;
  });

  infos.erase(std::unique(infos.begin(), infos.end(),
                          [](const Hyphenator::BreakInfo& a, const Hyphenator::BreakInfo& b) {
                            return a.byteOffset == b.byteOffset;
                          }),
              infos.end());
}

}  // namespace

void Hyphenator::breakOffsets(const std::string& word, const bool includeFallback, Scratch& scratch) {
  std::vector<BreakInfo>& breaks = scratch.breaks;
  breaks.clear();
  if (word.empty()) {
    return;
  }

  // Convert to codepoints and normalize word boundaries.
  std::vector<CodepointInfo>& cps = scratch.cps;
  collectCodepoints(word, cps);
  trimSurroundingPunctuationAndFootnote(cps);
  const auto* hyphenator = cachedHyphenator_;

  // Detect apostrophe-like separators early; used by both branches below.
  bool hasApostropheLikeSeparator = false;
  for (const auto& cp : cps) {
    if (isApostrophe(cp.value)) {
      hasApostropheLikeSeparator = true;
      break;
    }
  }

  // Explicit hyphen markers (soft or hard) take precedence over language breaks.
  appendExplicitBreakInfos(cps, breaks);
  if (!breaks.empty()) {
    // When a word contains explicit hyphens we also run Liang patterns on each alphabetic
    // segment between them. Without this, "US-Satellitensystems" would only offer one split
    // point (after "US-"), making it impossible to break mid-"Satellitensystems" even when
    // "US-Satelliten-" would fit on the line.
    //
    // Example: "US-Satellitensystems"
    //   Segments: ["US", "Satellitensystems"]
    //   Explicit break: after "US-"           -> @3  (no inserted hyphen)
    //   Pattern breaks on "Satellitensystems" -> @5  Sa|tel  (+hyphen)
    //                                            @8  Satel|li  (+hyphen)
    //                                            @10 Satelli|ten  (+hyphen)
    //                                            @13 Satelliten|sys  (+hyphen)
    //                                            @16 Satellitensys|tems  (+hyphen)
    //   Result: 6 sorted break points; the line-breaker picks the widest prefix that fits.
    if (hyphenator) {
      appendSegmentPatternBreaks(cps, *hyphenator, /*includeFallback=*/false, breaks);
    }
    // Also add apostrophe contraction breaks when present (e.g. "l'état-major"
    // has both an explicit hyphen and an apostrophe that can independently break).
    if (hasApostropheLikeSeparator) {
      appendApostropheContractionBreaks(cps, breaks);
    }
    // Merge all break points into ascending byte-offset order.
    sortAndDedupeBreakInfos(breaks);
    return;
  }

  // Apostrophe-like separators split compounds into alphabetic segments; run Liang on each segment.
  // This allows words like "all'improvviso" to hyphenate within "improvviso" instead of becoming
  // completely unsplittable due to the apostrophe punctuation. Apostrophe contraction breaks are
  // applied regardless of whether a language hyphenator is available.
  if (hasApostropheLikeSeparator) {
    if (hyphenator) {
      appendSegmentPatternBreaks(cps, *hyphenator, includeFallback, breaks);
    }
    appendApostropheContractionBreaks(cps, breaks);
    sortAndDedupeBreakInfos(breaks);
    return;
  }

  // Ask language hyphenator for legal break points.
  std::vector<size_t>& indexes = scratch.indexes;
  indexes.clear();
  if (hyphenator) {
    hyphenator->breakIndexes(cps, indexes);
  }

  // Only add fallback breaks if needed
  if (includeFallback && indexes.empty()) {
    const size_t minPrefix = hyphenator ? hyphenator->minPrefix() : LiangWordConfig::kDefaultMinPrefix;
    const size_t minSuffix = hyphenator ? hyphenator->minSuffix() : LiangWordConfig::kDefaultMinSuffix;
    indexes.reserve(cps.size());  // nearly every position is one: no regrowth with the old block beside the new
    appendFallbackIndexes(cps, minPrefix, minSuffix, indexes);
  }

  if (indexes.empty()) {
    return;
  }

  breaks.reserve(indexes.size());
  for (const size_t idx : indexes) {
    // CJK scripts are not hyphenated: a break next to a CJK character draws no hyphen. Ported from
    // crosspoint-reader PR #2288 (Justin Mitchell). Nor is Myanmar: a line ends between its syllables as it ends
    // between its phrases.
    const auto unhyphenated = [](const uint32_t cp) { return utf8IsCjkBreakable(cp) || myanmar::isRunMember(cp); };
    const bool noHyphen =
        (idx < cps.size() && unhyphenated(cps[idx].value)) || (idx > 0 && unhyphenated(cps[idx - 1].value));
    breaks.push_back({byteOffsetForIndex(cps, idx), !noHyphen && showsHyphen(hyphenator, cps, idx)});
  }
}

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string& word, const bool includeFallback) {
  Scratch scratch;
  breakOffsets(word, includeFallback, scratch);
  return std::move(scratch.breaks);
}

void Hyphenator::Scratch::release() {
  std::vector<CodepointInfo>().swap(cps);
  std::vector<size_t>().swap(indexes);
  std::vector<BreakInfo>().swap(breaks);
}

std::vector<Hyphenator::BreakInfo> Hyphenator::anywhereBreaks(const std::string& word) {
  const std::vector<CodepointInfo> cps = collectCodepoints(word);
  std::vector<size_t> indexes;
  appendFallbackIndexes(cps, 1, 1, indexes);
  std::vector<BreakInfo> breaks;
  breaks.reserve(indexes.size());  // lives for one candidate search
  for (const size_t idx : indexes) breaks.push_back({byteOffsetForIndex(cps, idx), false});
  return breaks;
}

std::vector<Hyphenator::BreakInfo> Hyphenator::myanmarLineEndBreaks(const std::string& word) {
  // The byte scan first: most words are not Myanmar, and return here without a code point vector.
  if (!myanmar::holdsMyanmar(word.data(), word.size())) return {};
  const std::vector<CodepointInfo> cps = collectCodepoints(word);
  std::vector<BreakInfo> breaks;
  breaks.reserve(cps.size());  // at most a break a code point; the vector lives for one candidate search
  for (size_t idx = 1; idx < cps.size(); ++idx) {
    // Inside a run: a syllable's start after a member, a joiner or a dotted circle. Myanmar meeting another script
    // is no break of this rule's (a bracket or a quote must not be left at a line's end).
    const uint32_t prev = cps[idx - 1].value;
    if (myanmar::isRunMember(cps[idx].value) && (myanmar::isRunMember(prev) || myanmar::isShared(prev)) &&
        myanmarBreakAllowed(cps, idx)) {
      breaks.push_back({cps[idx].byteOffset, false});
    }
  }
  return breaks;
}

void Hyphenator::setPreferredLanguage(const std::string& lang) { cachedHyphenator_ = hyphenatorForLanguage(lang); }

const LanguageHyphenator* Hyphenator::forLanguage(const std::string& lang) { return hyphenatorForLanguage(lang); }

uint32_t Hyphenator::layoutKey(const std::string& lang, const bool hyphenationEnabled) {
  if (!hyphenationEnabled) return 0;
  const LanguageEntry* entry = languageEntryFor(lang);
  return entry ? hyphenationLayoutKey(entry->primaryTag) : 0;
}

const char* Hyphenator::languageCode(const std::string& lang) {
  const LanguageEntry* entry = languageEntryFor(lang);
  return entry ? entry->primaryTag : nullptr;
}
