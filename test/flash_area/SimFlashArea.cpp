#include "SimFlashArea.h"

#include "FlashArea.h"

namespace {
NorFlash* current = nullptr;
}

void sim_area::use(NorFlash* area) { current = area; }

NorFlash* FlashArea::get() { return current; }
