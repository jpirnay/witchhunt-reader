#include "CjkUiPack.h"

#include <CjkUiPacks.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

using cjk_ui_pack::Region;
constexpr size_t REGIONS = cjk_ui_pack::REGION_COUNT;
constexpr size_t SIZE_COUNT = sizeof(cjk_ui_pack::SIZES) / sizeof(cjk_ui_pack::SIZES[0]);

struct Slot {
  std::unique_ptr<SdCardFont> font;
  int8_t installed = -1;  // the file is on the card: -1 not asked this screen
  bool failed = false;    // the file did not load: not tried again this screen
};
Slot slots[REGIONS][SIZE_COUNT];

size_t uiRegion = REGIONS;  // none
Region order[REGIONS] = {Region::SimplifiedChinese, Region::TraditionalChinese, Region::Japanese, Region::Korean};
size_t orderCount = REGIONS;

bool sizeIndex(const uint8_t points, size_t& out) {
  for (size_t i = 0; i < SIZE_COUNT; ++i) {
    if (cjk_ui_pack::SIZES[i] == points) {
      out = i;
      return true;
    }
  }
  return false;
}

bool installed(const Region region, const size_t size) {
  Slot& slot = slots[static_cast<size_t>(region)][size];
  if (slot.installed < 0) {
    const char* family = cjk_ui_pack::PACKS[static_cast<size_t>(region)].family;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s/%s_%u.cpfont", SdCardFontRegistry::FONTS_DIR, family, family,
             static_cast<unsigned>(cjk_ui_pack::SIZES[size]));
    slot.installed = Storage.exists(path) ? 1 : 0;
  }
  return slot.installed == 1;
}

SdCardFont* open(const Region region, const size_t size) {
  if (!installed(region, size)) return nullptr;
  Slot& slot = slots[static_cast<size_t>(region)][size];
  if (!slot.font && !slot.failed) {
    const char* family = cjk_ui_pack::PACKS[static_cast<size_t>(region)].family;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s/%s_%u.cpfont", SdCardFontRegistry::FONTS_DIR, family, family,
             static_cast<unsigned>(cjk_ui_pack::SIZES[size]));
    slot.font = makeUniqueNoThrow<SdCardFont>();
    if (!slot.font || !slot.font->load(path)) {
      LOG_ERR("CJKPACK", "%s did not load", path);
      slot.font.reset();
      slot.failed = true;
    }
  }
  return slot.font.get();
}

// Whether `face`'s own table holds `cp`, asking nothing of the card.
bool inTable(const EpdFontData* face, const uint32_t cp) {
  if (!face->intervals || face->intervalCount == 0) return false;
  const EpdUnicodeInterval* const end = face->intervals + face->intervalCount;
  const EpdUnicodeInterval* const after =
      std::upper_bound(face->intervals, end, cp,
                       [](const uint32_t value, const EpdUnicodeInterval& interval) { return value < interval.first; });
  return after != face->intervals && cp <= (after - 1)->last;
}

}  // namespace

void CjkUiPack::setLanguage(const Language language) {
  switch (language) {
    case Language::ZH:
      uiRegion = static_cast<size_t>(Region::SimplifiedChinese);
      break;
    case Language::JA:
      uiRegion = static_cast<size_t>(Region::Japanese);
      break;
    case Language::KO:
      uiRegion = static_cast<size_t>(Region::Korean);
      break;
    default:
      uiRegion = REGIONS;
      break;
  }
  orderCount = cjk_ui_pack::order(false, false, uiRegion, order);
}

bool CjkUiPack::covers(const uint8_t points, const uint32_t codepoint) {
  size_t size = 0;
  if (!cjk_ui_pack::anyCovers(codepoint) || !sizeIndex(points, size)) return false;
  for (size_t i = 0; i < orderCount; ++i) {
    if (cjk_ui_pack::covers(order[i], codepoint) && installed(order[i], size)) return true;
  }
  return false;
}

const EpdFontData* CjkUiPack::face(const uint8_t points, const uint32_t codepoint) {
  size_t size = 0;
  if (!cjk_ui_pack::anyCovers(codepoint) || !sizeIndex(points, size)) return nullptr;
  for (size_t i = 0; i < orderCount; ++i) {
    if (!cjk_ui_pack::covers(order[i], codepoint)) continue;
    if (SdCardFont* font = open(order[i], size)) {
      const EpdFont* regular = font->getEpdFont(0);
      if (regular) return regular->data;
    }
  }
  return nullptr;
}

void CjkUiPack::prepare(const uint8_t points, const char* utf8Text) {
  size_t size = 0;
  if (!utf8Text || !sizeIndex(points, size)) return;
  bool kana = false;
  bool hangul = false;
  for (const auto* p = reinterpret_cast<const uint8_t*>(utf8Text); *p != 0;) {
    const uint32_t cp = utf8NextCodepoint(&p);
    kana = kana || cjk_ui_pack::isKana(cp);
    hangul = hangul || cjk_ui_pack::isHangul(cp);
  }
  orderCount = cjk_ui_pack::order(kana, hangul, uiRegion, order);

  // Each character goes to the first installed pack in that order that holds it: that pack reads it, if its table
  // does not hold it already.
  for (size_t i = 0; i < orderCount; ++i) {
    const Region region = order[i];
    if (!installed(region, size)) continue;
    char wanted[96];
    size_t used = 0;
    bool held = true;
    SdCardFont* font = nullptr;
    for (const auto* p = reinterpret_cast<const uint8_t*>(utf8Text); *p != 0;) {
      const uint8_t* const start = p;
      const uint32_t cp = utf8NextCodepoint(&p);
      if (!cjk_ui_pack::covers(region, cp)) continue;
      bool earlier = false;
      for (size_t j = 0; j < i && !earlier; ++j) {
        earlier = cjk_ui_pack::covers(order[j], cp) && installed(order[j], size);
      }
      if (earlier) continue;
      if (!font && !(font = open(region, size))) break;
      held = held && inTable(font->getEpdFont(0)->data, cp);
      const auto bytes = static_cast<size_t>(p - start);
      if (used + bytes < sizeof(wanted)) {
        std::memcpy(wanted + used, start, bytes);
        used += bytes;
      }
    }
    if (!font || used == 0 || held) continue;
    wanted[used] = '\0';
    font->prewarm(wanted, 1);
  }
}

void CjkUiPack::release() {
  for (auto& region : slots) {
    for (Slot& slot : region) {
      slot.font.reset();
      slot.installed = -1;
      slot.failed = false;
    }
  }
}
