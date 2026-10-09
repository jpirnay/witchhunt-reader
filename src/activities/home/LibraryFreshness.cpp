#include "LibraryFreshness.h"

#include <HalStorage.h>
#include <LibraryIndexReader.h>

#include "CrossPointSettings.h"

namespace {
bool builtThisBoot = false;
uint32_t generationOfLastBuild = 0;
}  // namespace

namespace LibraryFreshness {

bool stale(const LibraryIndexReader& index) {
  return !index.isOpen() || !builtThisBoot || generationOfLastBuild != Storage.contentGeneration() ||
         index.header().acceptRules != (SETTINGS.showHiddenFiles ? 1 : 0);
}

void built(const uint32_t generationAtStart) {
  builtThisBoot = true;
  generationOfLastBuild = generationAtStart;
}

}  // namespace LibraryFreshness
