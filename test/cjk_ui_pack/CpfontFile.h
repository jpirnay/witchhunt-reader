#pragma once
// A v4 .cpfont read straight from its file, as the generators wrote it: for the CJK UI pack test, which checks the
// shipped files against what the UI looks up there and what it draws from them.

#include <EpdFontData.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpfont_file {

using Bytes = std::vector<uint8_t>;

inline Bytes readFile(const std::string& path) {
  Bytes bytes;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    uint8_t buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    std::fclose(f);
  }
  return bytes;
}

inline uint32_t u32(const Bytes& b, const size_t at) {
  return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t{b[at + 3]} << 24;
}
inline uint16_t u16(const Bytes& b, const size_t at) { return static_cast<uint16_t>(b[at] | b[at + 1] << 8); }

// One style of the file.
struct FileStyle {
  std::vector<uint32_t> codepoints;  // in glyph order
  std::vector<EpdGlyph> glyphs;
  Bytes bitmaps;
  std::unordered_map<uint32_t, size_t> index;  // code point -> glyph

  Bytes bitmapOf(const uint32_t cp, EpdGlyph* record = nullptr) const {
    const auto it = index.find(cp);
    if (it == index.end()) return {};
    const EpdGlyph& g = glyphs[it->second];
    if (record) *record = g;
    return Bytes(bitmaps.begin() + g.dataOffset, bitmaps.begin() + g.dataOffset + g.dataLength);
  }
};

struct FileFont {
  uint16_t flags = 0;
  std::vector<uint8_t> styleIds;
  std::vector<FileStyle> styles;
};

inline FileFont readCpfont(const std::string& path) {
  const Bytes b = readFile(path);
  FileFont font;
  if (b.size() < 32) return font;
  font.flags = u16(b, 10);
  const uint8_t count = b[12];
  for (uint8_t s = 0; s < count; ++s) {
    const size_t toc = 32 + s * 32;
    const uint32_t intervals = u32(b, toc + 4);
    const uint32_t glyphs = u32(b, toc + 8);
    const uint16_t kernLeft = u16(b, toc + 17), kernRight = u16(b, toc + 19);
    const uint8_t leftClasses = b[toc + 21], rightClasses = b[toc + 22], ligatures = b[toc + 23];
    const uint32_t data = u32(b, toc + 24);
    FileStyle style;
    for (uint32_t i = 0; i < intervals; ++i) {
      const uint32_t first = u32(b, data + i * 12), last = u32(b, data + i * 12 + 4);
      for (uint32_t cp = first; cp <= last; ++cp) {
        style.index[cp] = style.codepoints.size();
        style.codepoints.push_back(cp);
      }
    }
    const size_t glyphAt = data + intervals * 12;
    for (uint32_t i = 0; i < glyphs; ++i) {
      EpdGlyph g{};
      std::memcpy(&g, &b[glyphAt + i * 16], sizeof(EpdGlyph));
      style.glyphs.push_back(g);
    }
    const size_t bitmapAt =
        glyphAt + glyphs * 16 + (kernLeft + kernRight) * 3 + leftClasses * rightClasses + ligatures * 8;
    style.bitmaps.assign(b.begin() + bitmapAt, b.end());
    font.styleIds.push_back(b[toc]);
    font.styles.push_back(std::move(style));
  }
  return font;
}

}  // namespace cpfont_file
