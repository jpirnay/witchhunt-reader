#include "FlashArea.h"

#include "EspPartitionFlash.h"

namespace FlashArea {

namespace {
EspPartitionFlash partitionArea;
NorFlash* chosen = nullptr;
bool picked = false;
}  // namespace

NorFlash* get() {
  if (!picked) {
    chosen = partitionArea.begin() ? &partitionArea : nullptr;
    picked = true;
  }
  return chosen;
}

}  // namespace FlashArea
