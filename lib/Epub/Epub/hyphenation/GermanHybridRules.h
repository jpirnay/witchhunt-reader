#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "HyphenationCommon.h"

void applyGermanHybridOverrides(const std::vector<CodepointInfo>& cps, uint8_t* breaks, size_t breakCount);