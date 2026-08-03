#include "NetworkMemoryTrim.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

void trimMemoryForNetworkSession(const GfxRenderer& renderer, const char* logTag) {
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->clearCache();
  }
  if (renderer.hasSecondaryBuffer()) {
    // Seed the controller baseline while frameBufferActive is still valid.
    if (!renderer.isX3()) renderer.syncRedRamFromFrameBuffer();
    if (renderer.releaseSecondaryBuffer()) {
      LOG_DBG(logTag, "Released secondary framebuffer before network session (~52 KB contiguous)");
      renderer.setSingleBufferFastDiff(true);
    }
  }
}
