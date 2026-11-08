#include "TypographyProfile.h"

#include <OtShaper.h>

#include "LanguageTag.h"
#include "hyphenation/Hyphenator.h"

TypographyProfile TypographyProfile::forLanguage(const std::string& tag) {
  using language_tag::CjkRegion;
  using language_tag::hasPrimary;
  const std::string t = language_tag::normalised(tag);
  const CjkRegion region = language_tag::cjkRegionOf(tag);
  TypographyProfile p;
  p.hyphenator = Hyphenator::forLanguage(tag);
  p.shapingLanguage = ot::languageTagsFor(tag.c_str());
  const bool arabicScript = language_tag::isArabicScript(tag);
  p.shapedScript = language_tag::indicScriptOf(tag) != nullptr || arabicScript;

  if (region != CjkRegion::None && region != CjkRegion::Korean) {
    p.breaking = Breaking::BetweenCharacters;
    p.justify = Justify::BetweenCharacters;
    p.punctuation = region == CjkRegion::Japanese             ? Punctuation::Japanese
                    : region == CjkRegion::TraditionalChinese ? Punctuation::TraditionalChinese
                                                              : Punctuation::SimplifiedChinese;
    p.cjkLatinGap = true;
    p.wholeEmGrid = true;
    p.verticalAllowed = true;
    p.cjkRepertoire = true;
  } else if (region == CjkRegion::Korean) {
    // Korean spaces its words and uses Western punctuation (KLREQ): the Simplified Chinese table only meets
    // the rare CJK mark, and there is no grid and no CJK-Latin gap.
    p.breaking = Breaking::KoreanWords;
    p.verticalAllowed = true;
    p.cjkRepertoire = true;
  } else if (hasPrimary(t, "he") || hasPrimary(t, "iw") || hasPrimary(t, "yi") || arabicScript) {
    p.direction = Direction::RightToLeft;
    p.hyphenator = nullptr;
  }
  return p;
}
