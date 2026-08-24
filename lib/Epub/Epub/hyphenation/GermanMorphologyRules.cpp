#include "GermanMorphologyRules.h"

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
    case 0x00E4:
      return 26;  // ä
    case 0x00F6:
      return 27;  // ö
    case 0x00FC:
      return 28;  // ü
    case 0x00DF:
      return 29;  // ß
    default:
      return kSymbolOther;
  }
}

bool matchesComponent(const std::vector<CodepointInfo>& cps, const size_t start, const size_t length,
                      const size_t componentIndex) {
  if (start + length > cps.size()) {
    return false;
  }

  const size_t offset = kGermanMorphologyComponentOffsets[componentIndex];

  for (size_t i = 0; i < length; ++i) {
    const uint8_t symbol = germanSymbol(cps[start + i].value);

    if (symbol == kSymbolOther || symbol != kGermanMorphologyComponentBlob[offset + i]) {
      return false;
    }
  }

  return true;
}

bool hasComponentStartingAt(const std::vector<CodepointInfo>& cps, const size_t start) {
  if (start >= cps.size()) {
    return false;
  }

  for (size_t i = 0; i < kGermanMorphologyComponentCount; ++i) {
    const size_t length = kGermanMorphologyComponentLengths[i];

    if (matchesComponent(cps, start, length, i)) {
      return true;
    }
  }

  return false;
}

}  // namespace

bool germanMorphologyShouldBlock(const std::vector<CodepointInfo>& cps, const uint8_t* breaks, const size_t boundary) {
  if (breaks == nullptr || boundary >= cps.size()) {
    return false;
  }

  // We only intervene when there is already another accepted break at a
  // nearby learned compound-component boundary. Thus the morphology layer
  // chooses between two existing layout alternatives instead of inventing a
  // new hard linguistic prohibition.
  constexpr int kMaxDistance = 2;

  for (int distance = 1; distance <= kMaxDistance; ++distance) {
    if (boundary + static_cast<size_t>(distance) < cps.size()) {
      const size_t candidateComponentStart = boundary + static_cast<size_t>(distance);

      if (breaks[candidateComponentStart] != 0 && hasComponentStartingAt(cps, candidateComponentStart)) {
        return true;
      }
    }

    if (boundary >= static_cast<size_t>(distance)) {
      const size_t candidateComponentStart = boundary - static_cast<size_t>(distance);

      if (breaks[candidateComponentStart] != 0 && hasComponentStartingAt(cps, candidateComponentStart)) {
        return true;
      }
    }
  }

  return false;
}
