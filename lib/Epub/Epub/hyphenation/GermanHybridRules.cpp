#include "GermanHybridRules.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "generated/de_hybrid_rules.h"

// Older / baseline generated headers do not contain any 3+3 tables.
// A generator that emits 3+3 data must also emit:
//
//   #define GERMAN_HYBRID_HAS_3X3 1
//
// before the table declarations.
//
// This keeps the runtime source compatible with both generated formats.
#ifndef GERMAN_HYBRID_HAS_3X3
#define GERMAN_HYBRID_HAS_3X3 0
#endif

namespace {

constexpr uint8_t kSymbolOther = 30;
#if GERMAN_HYBRID_HAS_3X3
constexpr uint8_t kSymbolPad = 31;
#endif

uint8_t germanSymbol(uint32_t cp) {
  cp = toLowerLatin(cp);

  if (cp >= 'a' && cp <= 'z') {
    return static_cast<uint8_t>(cp - 'a');
  }

  switch (cp) {
    case 0x00E4:
      return 26;  // ä
    case 0x00F6:
      return 27;  // ö
    case 0x00FC:
      return 28;  // ü
    case 0x00DF:
      return 29;  // ß

    case 0x00E0:
    case 0x00E1:
    case 0x00E2:
    case 0x00E3:
    case 0x00E5:
      return static_cast<uint8_t>('a' - 'a');

    case 0x00E8:
    case 0x00E9:
    case 0x00EA:
    case 0x00EB:
      return static_cast<uint8_t>('e' - 'a');

    case 0x00EC:
    case 0x00ED:
    case 0x00EE:
    case 0x00EF:
      return static_cast<uint8_t>('i' - 'a');

    case 0x00F2:
    case 0x00F3:
    case 0x00F4:
    case 0x00F5:
      return static_cast<uint8_t>('o' - 'a');

    case 0x00F9:
    case 0x00FA:
    case 0x00FB:
      return static_cast<uint8_t>('u' - 'a');

    case 0x00FD:
    case 0x00FF:
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

#if GERMAN_HYBRID_HAS_3X3

uint8_t contextSymbol3(const std::vector<CodepointInfo>& cps, const int index) {
  if (index < 0 || index >= static_cast<int>(cps.size())) {
    return kSymbolPad;
  }
  return germanSymbol(cps[static_cast<size_t>(index)].value);
}

bool contextKey3(const std::vector<CodepointInfo>& cps, const size_t boundary, uint32_t& outKey) {
  if (boundary == 0 || boundary >= cps.size()) {
    return false;
  }

  const int b = static_cast<int>(boundary);
  const uint8_t symbols[6] = {
      contextSymbol3(cps, b - 3), contextSymbol3(cps, b - 2), contextSymbol3(cps, b - 1),
      contextSymbol3(cps, b),     contextSymbol3(cps, b + 1), contextSymbol3(cps, b + 2),
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

uint32_t packed30At(const uint8_t* data, const size_t index) {
  const uint8_t* p = data + index * 4;
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3] & 0x3F) << 24);
}

template <size_t N>
bool containsPacked30(const std::array<uint8_t, N>& data, const size_t count, const uint32_t key) {
  size_t first = 0;
  size_t last = count;

  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const uint32_t value = packed30At(data.data(), middle);

    if (value < key) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }

  return first < count && packed30At(data.data(), first) == key;
}

#endif

}  // namespace

void applyGermanHybridOverrides(const std::vector<CodepointInfo>& cps, uint8_t* breaks) {
  if (breaks == nullptr || cps.size() < 4) {
    return;
  }

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
    } else if (hasContext && containsPacked20(kGermanAddContexts, kGermanAddContextCount, context)) {
      breaks[boundary] = 1;
    }

#if GERMAN_HYBRID_HAS_3X3
    uint32_t context3 = 0;
    if (!contextKey3(cps, boundary, context3)) {
      continue;
    }

    // BLOCK always wins.
    if (containsPacked30(kGermanBlockContexts3, kGermanBlockContext3Count, context3)) {
      breaks[boundary] = 0;
      continue;
    }

    if (breaks[boundary] == 0 && containsPacked30(kGermanAddContexts3, kGermanAddContext3Count, context3)) {
      breaks[boundary] = 1;
    }
#endif
  }
}