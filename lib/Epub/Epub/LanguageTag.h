#pragma once

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

// An Indic script the reader has Noto families for: `name` is the family suffix (NotoSans/NotoSerif + name) and
// `probe` a consonant every font of the script maps.
struct IndicScript {
  const char* name;
  uint32_t probe;
};

namespace detail {
struct IndicScriptRow {
  const char* subtag;  // the ISO 15924 code, lowercase
  IndicScript script;
};
inline constexpr IndicScriptRow kIndicScripts[] = {
    {"deva", {"Devanagari", 0x0915}}, {"beng", {"Bengali", 0x0995}}, {"guru", {"Gurmukhi", 0x0A15}},
    {"gujr", {"Gujarati", 0x0A95}},   {"orya", {"Oriya", 0x0B15}},   {"taml", {"Tamil", 0x0B95}},
    {"telu", {"Telugu", 0x0C15}},     {"knda", {"Kannada", 0x0C95}}, {"mlym", {"Malayalam", 0x0D15}},
    {"sinh", {"Sinhala", 0x0D9A}},
};
struct IndicLanguageRow {
  const char* primary;
  uint8_t script;  // an index into kIndicScripts
};
inline constexpr IndicLanguageRow kIndicLanguages[] = {
    {"hi", 0},  {"mr", 0},  {"ne", 0},  {"sa", 0},  {"kok", 0}, {"mai", 0}, {"bho", 0}, {"awa", 0},
    {"mag", 0}, {"raj", 0}, {"doi", 0}, {"brx", 0}, {"new", 0}, {"bn", 1},  {"as", 1},  {"mni", 1},
    {"pa", 2},  {"gu", 3},  {"or", 4},  {"ta", 5},  {"te", 6},  {"kn", 7},  {"ml", 8},  {"si", 9},
};
}  // namespace detail

// The Indic script a tag is written in, or null for a language not written in one. A tag that names another script
// ("sa-Latn", "mni-Mtei") is not Indic here: the Noto family of the language's own script would draw it wrongly.
inline const IndicScript* indicScriptOf(const std::string& rawTag) {
  const std::string tag = normalised(rawTag);
  for (const auto& language : detail::kIndicLanguages) {
    if (!hasPrimary(tag, language.primary)) continue;
    const auto& row = detail::kIndicScripts[language.script];
    // A script subtag is four letters; a region is two, a variant four with a digit first.
    for (size_t at = tag.find('-'); at != std::string::npos; at = tag.find('-', at + 1)) {
      const size_t end = tag.find('-', at + 1);
      const std::string subtag = tag.substr(at + 1, end == std::string::npos ? end : end - at - 1);
      const bool letters = subtag.size() == 4 && subtag[0] >= 'a' && subtag[0] <= 'z';
      if (letters && subtag != row.subtag) return nullptr;
    }
    return &row.script;
  }
  return nullptr;
}

}  // namespace language_tag
