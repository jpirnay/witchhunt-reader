#include <array>

#include "GermanHybridHyphenator.h"
#include "GermanHybridRules.h"
#include "HyphenationCommon.h"

namespace {

constexpr size_t kMaxGermanWordChars = 70;

uint32_t lower(const uint32_t cp) { return toLowerLatin(cp); }

bool isGermanVowel(const uint32_t cpIn) {
  const uint32_t cp = lower(cpIn);

  switch (cp) {
    case 'a':
    case 'e':
    case 'i':
    case 'o':
    case 'u':
    case 'y':
    case 0x00E4:  // ä
    case 0x00F6:  // ö
    case 0x00FC:  // ü
      return true;

    // Common foreign-word vowels. Treat these as their vowel class rather
    // than rejecting otherwise perfectly usable German text.
    case 0x00E0:  // à
    case 0x00E1:  // á
    case 0x00E2:  // â
    case 0x00E3:  // ã
    case 0x00E5:  // å
    case 0x00E8:  // è
    case 0x00E9:  // é
    case 0x00EA:  // ê
    case 0x00EB:  // ë
    case 0x00EC:  // ì
    case 0x00ED:  // í
    case 0x00EE:  // î
    case 0x00EF:  // ï
    case 0x00F2:  // ò
    case 0x00F3:  // ó
    case 0x00F4:  // ô
    case 0x00F5:  // õ
    case 0x00F9:  // ù
    case 0x00FA:  // ú
    case 0x00FB:  // û
    case 0x00FD:  // ý
    case 0x00FF:  // ÿ
      return true;

    default:
      return false;
  }
}

bool isPair(const std::vector<CodepointInfo>& cps, const size_t pos, const char first, const char second) {
  return pos + 1 < cps.size() && lower(cps[pos].value) == static_cast<uint32_t>(first) &&
         lower(cps[pos + 1].value) == static_cast<uint32_t>(second);
}

bool isProtectedDiphthong(const std::vector<CodepointInfo>& cps, const size_t pos) {
  if (pos + 1 >= cps.size()) {
    return false;
  }

  const uint32_t a = lower(cps[pos].value);
  const uint32_t b = lower(cps[pos + 1].value);

  if (a == 'a' && b == 'i') return true;
  if (a == 'a' && b == 'u') return true;
  if (a == 0x00E4 && b == 'u') return true;  // äu
  if (a == 'e' && b == 'i') return true;
  if (a == 'e' && b == 'u') return true;
  if (a == 'o' && b == 'i') return true;

  // Usually the silent/stretching 'i': Wie-se.
  // Foreign-word exceptions such as Fo-li-en are handled by residual rules.
  if (a == 'i' && b == 'e') return true;

  return false;
}

// Number of codepoints making one inseparable consonant unit.
size_t consonantUnitLength(const std::vector<CodepointInfo>& cps, const size_t pos, const size_t end) {
  if (pos + 2 < end && lower(cps[pos].value) == 's' && lower(cps[pos + 1].value) == 'c' &&
      lower(cps[pos + 2].value) == 'h') {
    return 3;
  }

  if (pos + 1 < end) {
    const uint32_t a = lower(cps[pos].value);
    const uint32_t b = lower(cps[pos + 1].value);

    // Duden D165
    if ((a == 'c' && b == 'h') || (a == 'c' && b == 'k') || (a == 'p' && b == 'h') || (a == 'r' && b == 'h') ||
        (a == 's' && b == 'h') || (a == 't' && b == 'h')) {
      return 2;
    }
  }

  return 1;
}

// Returns index immediately following the vowel nucleus starting at pos.
size_t vowelNucleusEnd(const std::vector<CodepointInfo>& cps, const size_t pos) {
  if (!isGermanVowel(cps[pos].value)) {
    return pos;
  }

  if (isProtectedDiphthong(cps, pos)) {
    return pos + 2;
  }

  return pos + 1;
}

void markBaseSyllableBreaks(const std::vector<CodepointInfo>& cps,
                            std::array<uint8_t, kMaxGermanWordChars + 1>& breaks) {
  const size_t n = cps.size();

  size_t firstVowel = 0;
  while (firstVowel < n && !isGermanVowel(cps[firstVowel].value)) {
    ++firstVowel;
  }

  if (firstVowel == n) {
    return;
  }

  size_t leftNucleusEnd = vowelNucleusEnd(cps, firstVowel);
  size_t scan = leftNucleusEnd;

  while (scan < n) {
    size_t nextVowel = scan;
    while (nextVowel < n && !isGermanVowel(cps[nextVowel].value)) {
      ++nextVowel;
    }

    if (nextVowel == n) {
      break;
    }

    size_t candidate = 0;

    if (nextVowel == leftNucleusEnd) {
      // Two independently pronounced vowel nuclei:
      // e.g. Trau|ung, Brau|e|rei.
      candidate = nextVowel;
    } else {
      // One or more consonant units between the vowel nuclei.
      //
      // Duden:
      //  - one consonant -> it moves to the following syllable
      //  - multiple consonants -> only the final consonant/unit moves
      size_t pos = leftNucleusEnd;
      size_t lastUnitStart = pos;

      while (pos < nextVowel) {
        lastUnitStart = pos;
        pos += consonantUnitLength(cps, pos, nextVowel);
      }

      candidate = lastUnitStart;
    }

    if (candidate >= GermanHybridHyphenator::kMinPrefix && n - candidate >= GermanHybridHyphenator::kMinSuffix) {
      breaks[candidate] = 1;
    }

    leftNucleusEnd = vowelNucleusEnd(cps, nextVowel);
    scan = leftNucleusEnd;
  }
}

}  // namespace

std::vector<size_t> GermanHybridHyphenator::breakIndexes(const std::vector<CodepointInfo>& cps) const {
  if (cps.size() < kMinPrefix + kMinSuffix || cps.size() > kMaxGermanWordChars) {
    return {};
  }

  for (const auto& cp : cps) {
    if (!isLatinLetter(cp.value)) {
      return {};
    }
  }

  std::array<uint8_t, kMaxGermanWordChars + 1> breaks{};
  markBaseSyllableBreaks(cps, breaks);

  // Generated high-confidence corrections:
  //   + add missed morphology/foreign-word breaks
  //   - suppress unsafe base-rule breaks
  applyGermanHybridOverrides(cps, breaks.data(), cps.size());

  std::vector<size_t> result;

  for (size_t i = kMinPrefix; i + kMinSuffix <= cps.size(); ++i) {
    if (breaks[i]) {
      result.push_back(i);
    }
  }

  return result;
}