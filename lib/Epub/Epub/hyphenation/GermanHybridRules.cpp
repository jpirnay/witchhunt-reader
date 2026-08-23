#include "GermanHybridRules.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "generated/de_hybrid_rules.h"

namespace {

constexpr uint8_t kSymbolOther = 30;

uint8_t germanSymbol(uint32_t cp) {
  cp = toLowerLatin(cp);

  if (cp >= 'a' && cp <= 'z') {
    return static_cast<uint8_t>(cp - 'a');
  }

  switch (cp) {
    case 0x00E4:  // ä
      return 26;
    case 0x00F6:  // ö
      return 27;
    case 0x00FC:  // ü
      return 28;
    case 0x00DF:  // ß
      return 29;

    // Fold common accented vowels used in German foreign words to their
    // ASCII base letter. Keep this mapping identical to the generator.
    case 0x00E0:  // à
    case 0x00E1:  // á
    case 0x00E2:  // â
    case 0x00E3:  // ã
    case 0x00E5:  // å
      return static_cast<uint8_t>('a' - 'a');

    case 0x00E8:  // è
    case 0x00E9:  // é
    case 0x00EA:  // ê
    case 0x00EB:  // ë
      return static_cast<uint8_t>('e' - 'a');

    case 0x00EC:  // ì
    case 0x00ED:  // í
    case 0x00EE:  // î
    case 0x00EF:  // ï
      return static_cast<uint8_t>('i' - 'a');

    case 0x00F2:  // ò
    case 0x00F3:  // ó
    case 0x00F4:  // ô
    case 0x00F5:  // õ
      return static_cast<uint8_t>('o' - 'a');

    case 0x00F9:  // ù
    case 0x00FA:  // ú
    case 0x00FB:  // û
      return static_cast<uint8_t>('u' - 'a');

    case 0x00FD:  // ý
    case 0x00FF:  // ÿ
      return static_cast<uint8_t>('y' - 'a');

    default:
      return kSymbolOther;
  }
}

bool pairKey(const std::vector<CodepointInfo>& cps, const size_t boundary, uint16_t& outKey) {
  if (boundary == 0 || boundary >= cps.size()) {
    return false;
  }

  const uint8_t left = germanSymbol(cps[boundary - 1].value);
  const uint8_t right = germanSymbol(cps[boundary].value);
  if (left == kSymbolOther || right == kSymbolOther) {
    return false;
  }

  outKey = static_cast<uint16_t>((static_cast<uint16_t>(left) << 5) | right);
  return true;
}

bool contextKey(const std::vector<CodepointInfo>& cps, const size_t boundary, uint32_t& outKey) {
  // minPrefix/minSuffix are both 2, so every legal runtime boundary has two
  // codepoints available on each side. Still guard here to keep the helper
  // safe if the caller changes those limits later.
  if (boundary < 2 || boundary + 1 >= cps.size()) {
    return false;
  }

  const uint8_t symbols[4] = {
      germanSymbol(cps[boundary - 2].value),
      germanSymbol(cps[boundary - 1].value),
      germanSymbol(cps[boundary].value),
      germanSymbol(cps[boundary + 1].value),
  };

  uint32_t key = 0;
  for (const uint8_t symbol : symbols) {
    if (symbol == kSymbolOther) {
      return false;
    }
    key = (key << 5) | symbol;
  }

  outKey = key;
  return true;
}

bool isSafePair(const uint16_t key) {
  const size_t byteIndex = key >> 3;
  const uint8_t mask = static_cast<uint8_t>(1u << (key & 7u));
  return byteIndex < kGermanSafePairBits.size() && (kGermanSafePairBits[byteIndex] & mask) != 0;
}

uint32_t packed20At(const uint8_t* data, const size_t index) {
  const uint8_t* p = data + index * 3;
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2] & 0x0F) << 16);
}

template <size_t N>
bool containsPacked20(const std::array<uint8_t, N>& data, const size_t count, const uint32_t key) {
  size_t first = 0;
  size_t last = count;

  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const uint32_t value = packed20At(data.data(), middle);
    if (value < key) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }

  return first < count && packed20At(data.data(), first) == key;
}

}  // namespace

void applyGermanHybridOverrides(const std::vector<CodepointInfo>& cps, uint8_t* breaks) {
  if (breaks == nullptr || cps.size() < 4) {
    return;
  }

  // The base engine is deliberately permissive: it implements the cheap
  // Sprechsilben rules and proposes candidate boundaries. The generated
  // tables turn that into a conservative e-reader policy:
  //   1. keep candidates from globally safe immediate boundary classes;
  //   2. otherwise keep only candidates with a safe 2+2 context;
  //   3. add a high-confidence 2+2 residual break when the base engine missed it.
  for (size_t boundary = 2; boundary + 2 <= cps.size(); ++boundary) {
    uint32_t context = 0;
    const bool hasContext = contextKey(cps, boundary, context);

    if (breaks[boundary] != 0) {
      uint16_t pair = 0;
      const bool safeByPair = pairKey(cps, boundary, pair) && isSafePair(pair);
      const bool safeByContext = hasContext && containsPacked20(kGermanSafeContexts, kGermanSafeContextCount, context);

      if (!safeByPair && !safeByContext) {
        breaks[boundary] = 0;
      }
      continue;
    }

    if (hasContext && containsPacked20(kGermanAddContexts, kGermanAddContextCount, context)) {
      breaks[boundary] = 1;
    }
  }
}
