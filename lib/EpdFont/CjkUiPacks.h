#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

/// The CJK UI packs (lib/EpdFont/scripts/build_ui_script_pack.py writes them; src/CjkUiPack.cpp opens them): the CJK
/// characters a book title may hold, at the CJK UI faces' sizes (10 and 12 pt, regular), one .cpfont file per region
/// in that region's Noto Sans CJK forms, installed by the Font Manager under /.crosspoint/fonts/<family>/. The
/// cjk_ui_* faces in flash hold only the translations' characters; these draw the rest. No reading font: the registry
/// leaves them out.
namespace cjk_ui_pack {

enum class Region : uint8_t { SimplifiedChinese, TraditionalChinese, Japanese, Korean };
constexpr size_t REGION_COUNT = 4;

struct Range {
  uint32_t first;
  uint32_t last;
};

// What each pack holds, exactly (the host test cjk_ui_pack checks the files against these): the CJK punctuation and
// the full-width forms, and the Han packs the CJK Unified Ideographs Noto draws (U+4E00-9FEF), Japanese with the kana
// and half-width forms, Korean every Hangul syllable and the compatibility jamo instead of Han.
constexpr Range HAN[] = {{0x3000, 0x303F}, {0x4E00, 0x9FEF}, {0xFF01, 0xFF60}};
constexpr Range JAPANESE[] = {{0x3000, 0x303F}, {0x3041, 0x3096}, {0x3099, 0x30FF}, {0x31F0, 0x31FF},
                              {0x4E00, 0x9FEF}, {0xFF01, 0xFF60}, {0xFF61, 0xFF9F}};
constexpr Range KOREAN[] = {{0x3000, 0x303F}, {0x3131, 0x318E}, {0xAC00, 0xD7A3}, {0xFF01, 0xFF60}};

struct Pack {
  const char* family;
  Region region;
  const Range* ranges;
  size_t rangeCount;
};

constexpr Pack PACKS[REGION_COUNT] = {
    {"ChineseUI", Region::SimplifiedChinese, HAN, sizeof(HAN) / sizeof(HAN[0])},
    {"ChineseTradUI", Region::TraditionalChinese, HAN, sizeof(HAN) / sizeof(HAN[0])},
    {"JapaneseUI", Region::Japanese, JAPANESE, sizeof(JAPANESE) / sizeof(JAPANESE[0])},
    {"KoreanUI", Region::Korean, KOREAN, sizeof(KOREAN) / sizeof(KOREAN[0])},
};

/// The CJK UI faces' sizes (font-sizes.sh CJK_UI_FONT_SIZES), the packs'. A pack is one file, <family>_10.cpfont
/// (named for its first size), whose styles are these sizes in this order: style 0 holds the 10 pt glyphs, style 1
/// the 12 pt ones. One SdCardFont then serves both, where a file per size would hold two on the heap.
constexpr uint8_t SIZES[] = {10, 12};
constexpr size_t SIZE_COUNT = sizeof(SIZES) / sizeof(SIZES[0]);
constexpr uint8_t FILE_POINTS = SIZES[0];

/// The style of a pack's file that holds `points`, or SIZE_COUNT for a size it has none of.
constexpr size_t styleFor(const uint8_t points) {
  for (size_t i = 0; i < SIZE_COUNT; ++i) {
    if (SIZES[i] == points) return i;
  }
  return SIZE_COUNT;
}

/// Whether the pack of `region` holds `codepoint`.
constexpr bool covers(const Region region, const uint32_t codepoint) {
  const Pack& pack = PACKS[static_cast<size_t>(region)];
  for (size_t i = 0; i < pack.rangeCount; ++i) {
    if (codepoint >= pack.ranges[i].first && codepoint <= pack.ranges[i].last) return true;
  }
  return false;
}

/// Whether a pack of any region may hold `codepoint` (Japanese holds every Han pack's characters).
constexpr bool anyCovers(const uint32_t codepoint) {
  return codepoint >= 0x3000 && (covers(Region::Japanese, codepoint) || covers(Region::Korean, codepoint));
}

/// Whether `name` is a CJK UI pack's family.
inline bool isFamily(const char* name) {
  for (const Pack& pack : PACKS) {
    if (std::strcmp(name, pack.family) == 0) return true;
  }
  return false;
}

/// The order the packs are searched in for a string: kana makes it Japanese, Hangul Korean, and that pack goes first;
/// then the UI language's region (`uiRegion`, REGION_COUNT for a UI language of none), then simplified Chinese,
/// traditional Chinese, Japanese, Korean. A title's Han characters then take one region's forms, its own when its kana
/// or Hangul tell it, whatever packs are installed. `hasKana` and `hasHangul` describe the string (isKana, isHangul).
inline size_t order(const bool hasKana, const bool hasHangul, const size_t uiRegion, Region (&out)[REGION_COUNT]) {
  size_t count = 0;
  bool taken[REGION_COUNT] = {};
  const auto add = [&](const size_t r) {
    if (r < REGION_COUNT && !taken[r]) {
      taken[r] = true;
      out[count++] = static_cast<Region>(r);
    }
  };
  if (hasKana) add(static_cast<size_t>(Region::Japanese));
  if (hasHangul) add(static_cast<size_t>(Region::Korean));
  add(uiRegion);
  for (size_t r = 0; r < REGION_COUNT; ++r) add(r);
  return count;
}

/// Kana letters, which only Japanese writes (not the middle dot or the prolonged sound mark, which Chinese borrows).
constexpr bool isKana(const uint32_t cp) {
  return (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) || (cp >= 0x31F0 && cp <= 0x31FF) ||
         (cp >= 0xFF66 && cp <= 0xFF9D);
}
/// Hangul syllables and compatibility jamo.
constexpr bool isHangul(const uint32_t cp) { return (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x3131 && cp <= 0x318E); }

}  // namespace cjk_ui_pack
