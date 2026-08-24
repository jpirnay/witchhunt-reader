#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "HyphenationCommon.h"

// Returns true when a currently accepted break is suspicious because it sits
// immediately next to a frequently observed German compound component.
//
// The rule is deliberately conservative: it only blocks a candidate when an
// already accepted alternative break exists exactly at the learned component
// boundary one or two characters away. This avoids globally blacklisting
// common character sequences.
bool germanMorphologyShouldBlock(const std::vector<CodepointInfo>& cps, const uint8_t* breaks, size_t boundary);
