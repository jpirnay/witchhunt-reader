#include "FontCacheManager.h"

#include <BuildArena.h>
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
  // Deliberately does NOT clear existing page slots: a page that mixes fonts (body +
  // heading) is prewarmed with one call per font, and clearing here would wipe the
  // previous font's slots — the render would then transiently re-decompress a glyph
  // group per body glyph (~10 ms each, multi-second page turns). The batch clear
  // happens once in endScanAndPrewarm() before the per-font loop.

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
  // Accumulate per fontId AND per base style so a page that mixes fonts (heading + body)
  // prewarms each, and each style is later warmed with only its own glyphs.
  scanByFont_[fontId].textByStyle[static_cast<uint8_t>(style) & 0x03] += text;
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

  // One batch clear for the whole page; the per-(font,style) prewarmCache calls below
  // append into the freed slots (FontDecompressor dedupes across slots between calls).
  manager_->clearCache();

  // Prewarm every (font, style) pair that appeared during the scan, each with only the text
  // drawn in that style. The page slots are limited (MAX_PAGE_SLOTS), so warm the pair with
  // the MOST text first: the body font's dominant style (~hundreds of glyphs) must win the
  // slots over a short heading or a few styled words. Losers fall back cheaply per glyph —
  // far better than the body thrashing.
  struct PrewarmItem {
    int fontId;
    uint8_t style;
    const std::string* text;
  };
  std::vector<PrewarmItem> order;
  order.reserve(manager_->scanByFont_.size() * 4);  // worst case: every style seen in every font
  for (const auto& kv : manager_->scanByFont_) {
    for (uint8_t i = 0; i < 4; i++) {
      const std::string& text = kv.second.textByStyle[i];
      if (!text.empty()) order.push_back({kv.first, i, &text});
    }
  }
  std::sort(order.begin(), order.end(),
            [](const PrewarmItem& a, const PrewarmItem& b) { return a.text->size() > b.text->size(); });

  for (const auto& item : order) {
    manager_->prewarmCache(item.fontId, item.text->c_str(), static_cast<uint8_t>(1u << item.style));
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

FontCacheManager::ScopedSlotArena::ScopedSlotArena(FontCacheManager& manager, BuildArena* arena) : manager_(manager) {
  if (!arena || !arena->valid() || !manager_.fontDecompressor_) return;
  // One reserved block for the whole page. Every slot bump-allocates inside it and the lot is
  // rewound in the destructor, so the arena cursor ends exactly where it started -- important
  // because the region is on loan from a live section build, which resumes using it afterwards.
  block_ = arena->reserveBlock();
  if (!block_.valid()) return;
  // Start from a clean slot table: a heap-backed slot left over from the previous page would
  // otherwise survive into this scope and be indistinguishable from an arena-backed one at
  // teardown. Prewarm rebuilds slot buffers on every call, so nothing useful is discarded.
  manager_.clearCache();
  manager_.fontDecompressor_->setSlotArena(arena);
  arena_ = arena;
  installed_ = true;
}

FontCacheManager::ScopedSlotArena::~ScopedSlotArena() {
  if (!installed_) return;
  // Clearing must precede uninstalling: freePageBuffer() decides whether to free() a slot by
  // reading the arena-backed flag, and with the arena already gone it would call free() on
  // interior pointers into a region this code does not own. That ordering is the reason this
  // class exists rather than two calls at each site.
  manager_.clearCache();
  manager_.fontDecompressor_->setSlotArena(nullptr);
  if (!arena_->release(block_)) {
    // Only reachable if something else allocated from the arena inside this scope and did not
    // release -- i.e. the render lock stopped excluding the build. Not fatal (the arena is reset
    // at the next build), but it means the invariant this class relies on has been broken.
    LOG_ERR("FCM", "Slot arena release refused: another owner allocated inside the page scope");
  }
}
