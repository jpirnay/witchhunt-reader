#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <FlashFontPartition.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstdlib>

SdCardFontManager::~SdCardFontManager() {
  if (renderer_) {
    unloadAll(*renderer_);
  } else {
    for (auto& lf : loaded_) {
      delete lf.font;
    }
    loaded_.clear();
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

// Try to write the whole family (all sizes) to the flash partition.
// If the total size exceeds the available space, falls back to writing only
// the single requested file. Returns true if at least the requested size
// was written successfully.
static bool writeFamily(const SdCardFontFamilyInfo& family, uint8_t requestedPointSize) {
  // Collect available sizes and sort ascending so we write small→large.
  std::vector<uint8_t> sizes = family.availableSizes();
  std::sort(sizes.begin(), sizes.end());

  // Check whether the whole family fits.
  // FlashFontPartition::beginWrite erases the full partition, so we compute
  // the total first without writing.
  {
    // Rough total: sum all file sizes + HEADER_BYTES overhead.
    size_t total = FlashFontPartition::HEADER_BYTES;
    bool allFit = true;
    // We can't read partition size here directly, but 3.47 MB is the known
    // value. Use a conservative 3.3 MB cap to leave room for alignment padding.
    static constexpr size_t PARTITION_CAP = 3300 * 1024;
    for (uint8_t sz : sizes) {
      const SdCardFontFileInfo* fi = family.findFile(sz);
      if (!fi) continue;
      HalFile f;
      if (!Storage.openFileForRead("FFP", fi->path.c_str(), f) || !f) continue;
      total += (f.fileSize() + 3) & ~static_cast<size_t>(3);  // 4-byte align
      f.close();
      if (total > PARTITION_CAP || static_cast<uint8_t>(sizes.size()) > FlashFontPartition::MAX_ENTRIES) {
        allFit = false;
        break;
      }
    }

    if (allFit && sizes.size() > 1) {
      // Write the whole family.
      if (!FlashFontPartition::beginWrite(family.name.c_str())) return false;
      bool requestedWritten = false;
      for (uint8_t sz : sizes) {
        const SdCardFontFileInfo* fi = family.findFile(sz);
        if (!fi) continue;
        if (FlashFontPartition::appendFile(fi->path.c_str(), family.name.c_str(), sz)) {
          if (sz == requestedPointSize) requestedWritten = true;
        } else {
          LOG_ERR("FFP", "appendFile failed for %s@%u; aborting family write", family.name.c_str(), sz);
          // Fall through to single-file fallback below.
          FlashFontPartition::finaliseWrite();  // attempt to commit what we have
          return requestedWritten;
        }
      }
      if (!FlashFontPartition::finaliseWrite()) return false;
      LOG_INF("SDMGR", "Cached full family %s (%zu sizes) to flash", family.name.c_str(), sizes.size());
      return requestedWritten;
    }
  }

  // Single-file fallback: only write the requested size.
  const SdCardFontFileInfo* fi = family.findFile(requestedPointSize);
  if (!fi) return false;
  if (!FlashFontPartition::beginWrite(family.name.c_str())) return false;
  if (!FlashFontPartition::appendFile(fi->path.c_str(), family.name.c_str(), requestedPointSize)) {
    FlashFontPartition::finaliseWrite();
    return false;
  }
  if (!FlashFontPartition::finaliseWrite()) return false;
  LOG_INF("SDMGR", "Cached single file %s@%u to flash", family.name.c_str(), requestedPointSize);
  return true;
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
                                   const std::function<void()>& onColdLoad) {
  if (!renderer_) renderer_ = &renderer;
  if (!loadedFamilyName_.empty()) unloadAll(renderer);

  const SdCardFontFileInfo* selected = family.pickClosestSize(targetPtSize);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }

  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "OOM allocating SdCardFont for %s", selected->path.c_str());
    return false;
  }

  // --- Flash partition cache ---
  // If the partition already has this exact entry, mmap directly — no write
  // needed. Otherwise write the family (or single file) and then mmap.
  bool loadedFromMmap = false;
  {
    if (FlashFontPartition::isMapped()) FlashFontPartition::unmap();

    const bool alreadyCached = FlashFontPartition::hasEntry(family.name.c_str(), selected->pointSize);

    bool readyToMmap = alreadyCached;
    if (!alreadyCached) {
      // Genuine first load: writing the family into the flash partition takes
      // a noticeable moment — let the caller advertise it before we start.
      if (onColdLoad) onColdLoad();
      readyToMmap = writeFamily(family, selected->pointSize);
      if (!readyToMmap) {
        LOG_ERR("SDMGR", "Flash write failed for %s, falling back to SD load", family.name.c_str());
      }
    } else {
      LOG_DBG("SDMGR", "Flash cache hit for %s@%u", family.name.c_str(), selected->pointSize);
    }

    if (readyToMmap) {
      const uint8_t* mmapPtr = nullptr;
      size_t mmapSize = 0;
      if (FlashFontPartition::mmap(family.name.c_str(), selected->pointSize, &mmapPtr, &mmapSize)) {
        if (font->loadFromMmap(mmapPtr, mmapSize, selected->path.c_str())) {
          loadedFromMmap = true;
          LOG_INF("SDMGR", "Font loaded from flash: %s@%u", family.name.c_str(), selected->pointSize);
        } else {
          LOG_ERR("SDMGR", "loadFromMmap failed, falling back to SD");
          FlashFontPartition::unmap();
        }
      } else {
        LOG_ERR("SDMGR", "mmap failed for %s@%u, falling back to SD", family.name.c_str(), selected->pointSize);
      }
    }
  }

  if (!loadedFromMmap) {
    if (!font->load(selected->path.c_str())) {
      LOG_ERR("SDMGR", "Failed to load %s", selected->path.c_str());
      delete font;
      return false;
    }
  }

  int fontId = computeFontId(font->contentHash(), family.name.c_str(), selected->pointSize);
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, selected->path.c_str());
    delete font;
    return false;
  }
  renderer.registerSdCardFont(fontId, font);
  loaded_.push_back({font, fontId, selected->pointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u (target=%u)", selected->path.c_str(), selected->pointSize, fontId,
          font->styleCount(), targetPtSize);

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily);

  loadedFamilyName_ = family.name;
  loadedPointSize_ = selected->pointSize;
  return true;
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  if (!renderer_) renderer_ = &renderer;
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
  if (FlashFontPartition::isMapped()) FlashFontPartition::unmap();
}

int SdCardFontManager::getFontId(const std::string& familyName) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  return loaded_.front().fontId;
}
