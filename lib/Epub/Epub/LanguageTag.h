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

}  // namespace language_tag
