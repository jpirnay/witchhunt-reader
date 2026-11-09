#include "UiScriptPack.h"

#include <ArabicUiPack.h>
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

// The pack's sizes: the UI faces' (build_ui_script_pack.py SIZES).
constexpr uint8_t SIZES[] = {6, 8, 10, 12, 14};
constexpr size_t SIZE_COUNT = sizeof(SIZES) / sizeof(SIZES[0]);

struct Slot {
  std::unique_ptr<SdCardFont> font;
  int8_t installed = -1;  // the file is on the card: -1 not asked this screen
  bool failed = false;    // the file did not load: not tried again this screen
};
Slot slots[SIZE_COUNT];

Slot* slotFor(const uint8_t points) {
  for (size_t i = 0; i < SIZE_COUNT; ++i) {
    if (SIZES[i] == points) return &slots[i];
  }
  return nullptr;
}

void pathFor(const uint8_t points, char* out, const size_t len) {
  snprintf(out, len, "%s/%s/%s_%u.cpfont", SdCardFontRegistry::FONTS_DIR, arabic_ui_pack::FAMILY,
           arabic_ui_pack::FAMILY, static_cast<unsigned>(points));
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

bool UiScriptPack::covers(const uint32_t cp) { return arabic_ui_pack::covers(cp); }

bool UiScriptPack::installed(const uint8_t points) {
  Slot* slot = slotFor(points);
  if (!slot) return false;
  if (slot->installed < 0) {
    char path[96];
    pathFor(points, path, sizeof(path));
    slot->installed = Storage.exists(path) ? 1 : 0;
  }
  return slot->installed == 1;
}

const EpdFontData* UiScriptPack::face(const uint8_t points, const bool bold) {
  if (!installed(points)) return nullptr;
  Slot* slot = slotFor(points);
  if (!slot->font && !slot->failed) {
    char path[96];
    pathFor(points, path, sizeof(path));
    slot->font = makeUniqueNoThrow<SdCardFont>();
    if (!slot->font || !slot->font->load(path)) {
      LOG_ERR("UIPACK", "%s did not load", path);
      slot->font.reset();
      slot->failed = true;
      return nullptr;
    }
    LOG_DBG("UIPACK", "%s open", path);
  }
  if (!slot->font) return nullptr;
  const uint8_t style = bold ? 1 : 0;
  if (!slot->font->hasStyle(style)) return nullptr;
  const EpdFont* font = slot->font->getEpdFont(style);
  return font ? font->data : nullptr;
}

void UiScriptPack::prepare(const uint8_t points, const bool bold, const char* utf8Text) {
  if (!utf8Text) return;
  // The string's pack characters, as UTF-8: what the prewarm reads. A title's worth; past it, the rest come one by
  // one from the font's miss ring.
  char wanted[192];
  size_t used = 0;
  bool any = false;
  const auto* p = reinterpret_cast<const uint8_t*>(utf8Text);
  while (*p != 0) {
    const uint8_t* const start = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!covers(cp)) continue;
    any = true;
    const auto bytes = static_cast<size_t>(p - start);
    if (used + bytes < sizeof(wanted)) {
      std::memcpy(wanted + used, start, bytes);
      used += bytes;
    }
  }
  if (!any) return;
  const EpdFontData* data = face(points, bold);
  if (!data) return;
  wanted[used] = '\0';
  // Measured, then drawn in visual order: the same characters, read once.
  bool held = true;
  for (const auto* q = reinterpret_cast<const uint8_t*>(wanted); held && *q != 0;) {
    held = inTable(data, utf8NextCodepoint(&q));
  }
  if (held) return;
  slotFor(points)->font->prewarm(wanted, static_cast<uint8_t>(1u << (bold ? 1 : 0)));
}

void UiScriptPack::release() {
  for (Slot& slot : slots) {
    slot.font.reset();
    slot.installed = -1;
    slot.failed = false;
  }
}
