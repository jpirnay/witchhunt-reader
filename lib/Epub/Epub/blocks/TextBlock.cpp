#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u)\n", (uint32_t)words.size(),
            (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size());
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  // Heading blocks may render with a taller real font (headingFontId) instead of scaling the
  // body font. In that case fontSizeMultiplier is a small residual (usually 1.0). Resolve the
  // effective (fontId, scale) pair once and use it for every measure/draw below.
  const int effFontId = blockStyle.headingFontId != 0 ? blockStyle.headingFontId : fontId;
  const float scale = blockStyle.fontSizeMultiplier;
  const int ascender =
      (scale == 1.0f) ? renderer.getFontAscenderSize(effFontId) : renderer.getFontAscenderSizeScaled(effFontId, scale);
  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender:
    //   SUP: raise by 40% of ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of ascender — descends below baseline without clashing with ascenders below
    int wordY = y;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }
    if (scale == 1.0f) {
      renderer.drawText(effFontId, wordX, wordY, words[i].c_str(), true, currentStyle);
    } else {
      renderer.drawTextScaled(effFontId, wordX, wordY, words[i].c_str(), true, currentStyle, scale);
    }

    const bool hasDecoration =
        !scanning && (currentStyle & (EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH)) != 0;
    if (hasDecoration) {
      const std::string& w = words[i];
      const int lineWidth = (scale == 1.0f) ? renderer.getTextWidth(effFontId, w.c_str(), currentStyle)
                                            : renderer.getTextWidthScaled(effFontId, w.c_str(), currentStyle, scale);

      if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
        const int underlineY = y + ascender + 3;
        renderer.drawLine(wordX, underlineY, wordX + lineWidth, underlineY, 2, true);
      }

      if ((currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
        const int strikeY = y + ascender / 2 + 4;
        renderer.drawLine(wordX, strikeY, wordX + lineWidth, strikeY, 2, true);
      }
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u)\n", words.size(),
            wordXpos.size(), wordStyles.size());
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.fontSizeMultiplier);
  serialization::writePod(file, blockStyle.headingFontId);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.fontSizeMultiplier);
  serialization::readPod(file, blockStyle.headingFontId);

  return std::unique_ptr<TextBlock>(
      new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), blockStyle));
}
