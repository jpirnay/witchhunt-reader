#pragma once

#include <cstdint>
#include <vector>

#include "HyphenationCommon.h"

// Validates candidate German syllable breaks and adds high-confidence
// residual breaks learned from the DANTE German word list.
//
// breaks[i] means that a break may occur before cps[i].
void applyGermanHybridOverrides(const std::vector<CodepointInfo>& cps, uint8_t* breaks);