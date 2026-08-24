#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "HyphenationCommon.h"

// Returns true when the current boundary matches a DANTE-learned German
// compound boundary signature. This layer is positive-only: it may ADD a
// break, but it never removes an existing break.
bool germanMorphologyShouldAdd(const std::vector<CodepointInfo>& cps, size_t boundary);
