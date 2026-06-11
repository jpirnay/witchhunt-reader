#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    if (!font) {
      LOG_ERR("FCM", "clearCache: null SdCardFont pointer for fontId=%d", id);
      continue;
    }
    font->clearCache();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  clearCache();

  // SD card font prewarm path: prewarm all requested styles in one call
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end()) {
    SdCardFont* sdFont = sdIt->second;
    if (!sdFont) {
      LOG_ERR("FCM", "prewarmCache(SD): null SdCardFont pointer for fontId=%d", fontId);
      return;
    }
    int missed = sdFont->prewarm(utf8Text, styleMask);
    if (missed < 0) {
      LOG_ERR("FCM", "prewarmCache(SD): prewarm failed for fontId=%d (styleMask=0x%02X)", fontId, styleMask);
    } else if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed < 0) {
      LOG_DBG("FCM", "prewarmCache: Decompressor slots full during style %d!", i);
    } else if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    if (font) font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    if (font) font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  // Accumulate per fontId so a page that mixes fonts (e.g. heading + body) prewarms each.
  ScanEntry& entry = scanByFont_[fontId];
  entry.text += text;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    if ((*p & 0xC0) != 0x80) cpCount++;
    p++;
  }
  entry.styleCounts[baseStyle] += cpCount;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->resetStats();
  manager_->scanByFont_.clear();
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanByFont_.empty()) return;

  // Prewarm every font that appeared during the scan (typically 1, occasionally 2 for a
  // heading + body page). Without this, only one font is warmed and the render thrashes the
  // glyph cache on the others.
  //
  // The decompressor's page slots are limited (MAX_PAGE_SLOTS), so prewarm the font with the
  // MOST text first: the body font (the bulk of the glyphs, ~hundreds) must win the slots over
  // a short heading (a handful of glyphs). If the heading then can't get a slot, its few glyphs
  // fall back cheaply — far better than the body thrashing.
  std::vector<const std::pair<const int, ScanEntry>*> order;
  order.reserve(manager_->scanByFont_.size());
  for (const auto& kv : manager_->scanByFont_) {
    if (!kv.second.text.empty()) order.push_back(&kv);
  }
  std::sort(order.begin(), order.end(),
            [](const auto* a, const auto* b) { return a->second.text.size() > b->second.text.size(); });

  for (const auto* kv : order) {
    const ScanEntry& entry = kv->second;
    uint8_t styleMask = 0;
    for (uint8_t i = 0; i < 4; i++) {
      if (entry.styleCounts[i] > 0) styleMask |= (1 << i);
    }
    if (styleMask == 0) styleMask = 1;  // default to regular
    manager_->prewarmCache(kv->first, entry.text.c_str(), styleMask);
  }

  manager_->scanByFont_.clear();
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanByFont_ is empty)
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
