#pragma once

#include "NorFlash.h"

// FlashArea::get() for the host tests of the area's consumers (SimFlashArea.cpp stands in for lib/hal/FlashArea.cpp):
// whichever area the test hands it, nullptr for a board without one.
namespace sim_area {
void use(NorFlash* area);
}
