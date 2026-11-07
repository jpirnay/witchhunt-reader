#pragma once

#include <IndicScripts.h>

#include <cstdint>
#include <string>

// What a book's language tag means, read in one place. dc:language is free text in the wild: case and '-'/'_' vary,
// and books carry tags no standard has. Layout (TypographyProfile) and the reader's font (SdCardFontSystem) both ask,
// and a tag one of them does not know gives a book the right font laid out by the wrong rules.
namespace language_tag {

// The tag lowercased, with '_' read as '-' (zh_TW is common in OPF files).
inline std::string normalised(const std::string& tag) {
  std::string out;
  out.reserve(tag.size());
  for (char c : tag) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '_') c = '-';
    out.push_back(c);
  }
  return out;
}

// Both take a normalised tag.
inline bool hasPrimary(const std::string& tag, const char* primary) {
  const std::string p = primary;
  return tag.compare(0, p.size(), p) == 0 && (tag.size() == p.size() || tag[p.size()] == '-');
}

inline bool hasSubtag(const std::string& tag, const char* subtag) {
  const std::string needle = std::string("-") + subtag;
  for (size_t at = tag.find(needle); at != std::string::npos; at = tag.find(needle, at + 1)) {
    const size_t end = at + needle.size();
    if (end == tag.size() || tag[end] == '-') return true;
  }
  return false;
}

// Which CJK writing a tag names. ChineseAmbiguous is Chinese that gives neither a script nor a region.
enum class CjkRegion : uint8_t { None, Japanese, Korean, SimplifiedChinese, TraditionalChinese, ChineseAmbiguous };

// An explicit script decides Chinese, else the region (Taiwan, Hong Kong, Macao are traditional; China and Singapore
// simplified), as KOReader's TextLangCfg. Classical Chinese (lzh) is traditional. Two tags no standard has are taken
// too: "cn", China's country code standing in for the language, and the old .NET script tags zh-CHS and zh-CHT.
inline CjkRegion cjkRegionOf(const std::string& rawTag) {
  const std::string tag = normalised(rawTag);
  if (hasPrimary(tag, "ja")) return CjkRegion::Japanese;
  if (hasPrimary(tag, "ko")) return CjkRegion::Korean;
  if (tag == "cn") return CjkRegion::SimplifiedChinese;  // whole tag only: "cnr" is Montenegrin
  if (hasPrimary(tag, "lzh")) return CjkRegion::TraditionalChinese;
  if (!hasPrimary(tag, "zh")) return CjkRegion::None;

  if (hasSubtag(tag, "hant") || hasSubtag(tag, "cht")) return CjkRegion::TraditionalChinese;
  if (hasSubtag(tag, "hans") || hasSubtag(tag, "chs")) return CjkRegion::SimplifiedChinese;
  if (hasSubtag(tag, "tw") || hasSubtag(tag, "hk") || hasSubtag(tag, "mo")) return CjkRegion::TraditionalChinese;
  if (hasSubtag(tag, "cn") || hasSubtag(tag, "sg")) return CjkRegion::SimplifiedChinese;
  return CjkRegion::ChineseAmbiguous;
}

namespace detail {
struct IndicLanguageRow {
  char primary[4];
  uint8_t script;  // an index into indic::SCRIPTS
};
inline constexpr IndicLanguageRow kIndicLanguages[] = {
    {"hi", 0},  {"mr", 0},  {"ne", 0},  {"sa", 0},  {"kok", 0}, {"mai", 0}, {"bho", 0}, {"awa", 0},
    {"mag", 0}, {"raj", 0}, {"doi", 0}, {"brx", 0}, {"new", 0}, {"bn", 1},  {"as", 1},  {"mni", 1},
    {"pa", 2},  {"gu", 3},  {"or", 4},  {"ta", 5},  {"te", 6},  {"kn", 7},  {"ml", 8},  {"si", 9},
};
}  // namespace detail

// The Indic script a tag is written in, or null for a language not written in one. A tag that names another script
// ("sa-Latn", "mni-Mtei") is not Indic here: the Noto family of the language's own script would draw it wrongly.
// `name` is the Noto family suffix (NotoSans/NotoSerif + name), `probe` a consonant every font of the script maps.
inline const indic::ScriptInfo* indicScriptOf(const std::string& rawTag) {
  const std::string tag = normalised(rawTag);
  for (const auto& language : detail::kIndicLanguages) {
    if (!hasPrimary(tag, language.primary)) continue;
    const indic::ScriptInfo& script = indic::SCRIPTS[language.script];
    // A script subtag is four letters; a region is two, a variant four with a digit first. A singleton ends the
    // scan: extensions and private use follow it, and name no script of the language.
    for (size_t at = tag.find('-'); at != std::string::npos; at = tag.find('-', at + 1)) {
      const size_t end = tag.find('-', at + 1);
      const size_t len = (end == std::string::npos ? tag.size() : end) - at - 1;
      if (len == 1) break;
      if (len != 4 || tag[at + 1] < 'a' || tag[at + 1] > 'z') continue;
      uint32_t code = 0;
      for (size_t i = 0; i < 4; ++i) code = (code << 8) | static_cast<uint8_t>(tag[at + 1 + i]);
      if (code != (script.isoTag | 0x20202020u)) return nullptr;
    }
    return &script;
  }
  return nullptr;
}

}  // namespace language_tag
