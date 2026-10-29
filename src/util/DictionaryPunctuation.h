#pragma once

#include <Utf8.h>

#include <cstdint>

namespace dictionary_punctuation {

// Punctuation, symbols and spaces beyond ASCII that Dictionary::cleanWord() strips from a
// word's edges. isWordByte() keeps every byte >= 0x80 so accented letters
// survive, which also left «Über», ¿Qué, （词） and 「言葉」 in their marks.
//
// Based on crosspoint-reader PR #3611 (Anton Lijcklama à Nijeholt,
// @alycklama), reworked as a short list of the marks books put against a word
// instead of Unicode's category data (~6 KB of tables there).
struct CodepointRange {
  uint16_t first;
  uint16_t last;
};
inline constexpr CodepointRange EDGE_PUNCTUATION[] = {
    {0x00A0, 0x00A9},  // no-break space ¡ ¢ £ ¤ ¥ ¦ § ¨ ©
    {0x00AB, 0x00B4},  // « ¬ soft hyphen ® ¯ ° ± ² ³ ´
    {0x00B6, 0x00B9},  // ¶ · ¸ ¹
    {0x00BB, 0x00BF},  // » ¼ ½ ¾ ¿ (ª µ º between them are letters)
    {0x037E, 0x037E},  // Greek question mark
    {0x0387, 0x0387},  // Greek ano teleia
    {0x2000, 0x207F},  // General Punctuation (quotes, dashes, ellipsis, ‹ ›) and superscripts
    {0x3000, 0x3004},  // ideographic space 、 。 〃 〄
    {0x3008, 0x3020},  // CJK brackets and quotes 〈 〉 《 》 「 」 『 』 【 】 〜 〝 〞
    {0xFF01, 0xFF0F},  // fullwidth ！ ＂ ＃ ＄ ％ ＆ ＇ （ ） ＊ ＋ ， － ． ／
    {0xFF1A, 0xFF20},  // fullwidth ： ； ＜ ＝ ＞ ？ ＠
    {0xFF3B, 0xFF40},  // fullwidth ［ ＼ ］ ＾ ＿ ｀
    {0xFF5B, 0xFF65},  // fullwidth ｛ ｜ ｝ ～ ｟ ｠, halfwidth ｡ ｢ ｣ ､ ･
};

}  // namespace dictionary_punctuation

// True for a codepoint cleanWord() strips from a word's edge: EDGE_PUNCTUATION or utf8IsDefaultIgnorable().
inline bool isDictionaryEdgePunctuation(const uint32_t cp) {
  if (utf8IsDefaultIgnorable(cp)) return true;
  for (const auto& range : dictionary_punctuation::EDGE_PUNCTUATION) {
    if (cp >= range.first && cp <= range.last) return true;
  }
  return false;
}
