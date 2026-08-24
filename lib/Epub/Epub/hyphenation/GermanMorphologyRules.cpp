#include "GermanMorphologyRules.h"

#include <array>
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

bool contextKey2(const std::vector<CodepointInfo>& cps, const size_t boundary, uint32_t& outKey) {
  if (boundary < 2 || boundary + 1 >= cps.size()) {
    return false;
  }

  uint32_t key = 0;
  for (size_t i = boundary - 2; i <= boundary + 1; ++i) {
    const uint8_t symbol = germanSymbol(cps[i].value);
    if (symbol == kSymbolOther) {
      return false;
    }
    key = (key << 5) | symbol;
  }

  outKey = key;
  return true;
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

bool germanMorphologyShouldAdd(const std::vector<CodepointInfo>& cps, const size_t boundary) {
  uint32_t key = 0;
  if (!contextKey2(cps, boundary, key)) {
    return false;
  }

  return containsPacked20(kGermanMorphologyAddContexts2, kGermanMorphologyAddContext2Count, key);
}
