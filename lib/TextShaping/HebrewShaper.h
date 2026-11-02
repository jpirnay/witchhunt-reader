#pragma once

#include "TextShaping.h"

// The divine-name rule is crosspoint-reader PR #3834 (Uri Tauber); upstream matches it in visual order inside
// BidiUtils::applyBidiVisual, here it is matched in logical order as a shaping module (writing-modes step 1
// design, decision 3).
namespace HebrewShaper {

// Same contract as TextShaping::shape. Replaces each yod-he-vav-he (non-spacing marks may sit between the
// letters and after the last he) with he + geresh (U+05F3; upstream draws U+0027), both carrying the cluster of
// the yod.
bool shape(const char* text, size_t len, TextShaping::Unit* units, size_t capacity, size_t& count);

}  // namespace HebrewShaper
