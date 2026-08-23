#pragma once

#include <cstdint>
#include <vector>

#include "HyphenationCommon.h"

// Takes the candidate break mask produced by GermanHybridHyphenator's small
// rule engine, removes candidates that are not known-safe, and adds a small
// number of high-confidence residual breaks learned from DANTE.
//
// breaks[i] corresponds to a break before cps[i].
void applyGermanHybridOverrides(const std::vector<CodepointInfo>& cps, uint8_t* breaks);
