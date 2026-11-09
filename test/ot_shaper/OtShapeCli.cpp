// Shapes lines of text with lib/OtShaper and prints glyphs and positions, for
// test/ot_shaper/compare_harfbuzz.py to diff against HarfBuzz.
//
//   ot_shape_cli <font.ttf|layout blob> <scale> <ppem> <bcp47 or -> [cpac] < lines
//
// With `cpac` the font's CPac is written with ot::FilterTableWriter, as the install writes it into a style block, and
// the lines are shaped on those filters; without it the filters are computed.
//
// Each output line: "gid+xAdvance+xOffset+yOffset" per glyph, space separated,
// or "FAIL".

#include <IndicScripts.h>
#include <OtFilterWriter.h>
#include <OtShaper.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint32_t> decodeUtf8(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<uint8_t>(s[i]);
    uint32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 6) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c >> 4) == 14) {
      cp = c & 0x0F;
      n = 3;
    } else {
      cp = c & 0x07;
      n = 4;
    }
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

// The Arabic blocks the engine's tables cover: Arabic, Arabic Supplement, Arabic Extended-B and Extended-A.
bool isArabic(const uint32_t cp) {
  return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x0870 && cp <= 0x08FF);
}

// The Myanmar blocks the engine's tables cover: Myanmar, Myanmar Extended-B and Extended-A.
bool isMyanmar(const uint32_t cp) {
  return (cp >= 0x1000 && cp <= 0x109F) || (cp >= 0xA9E0 && cp <= 0xA9FF) || (cp >= 0xAA60 && cp <= 0xAA7F);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s font scale ppem lang [cpac] < lines\n", argv[0]);
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  const std::vector<uint8_t> font((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  ot::FaceTables tables;
  if (!ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) {
    fprintf(stderr, "bad font\n");
    return 1;
  }
  ot::Face face;
  if (!face.init(tables)) {
    fprintf(stderr, "bad font\n");
    return 1;
  }
  std::vector<uint8_t> cpac;
  if (argc > 5 && std::string(argv[5]) == "cpac") {
    ot::FilterTableWriter writer(face);
    cpac.resize(writer.bytes());
    if (cpac.empty() || writer.write(cpac.data(), static_cast<uint32_t>(cpac.size())) != cpac.size()) {
      fprintf(stderr, "no CPac\n");
      return 1;
    }
    tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
    if (!face.init(tables) || !face.filters(ot::GSUB) || !face.filters(ot::GPOS)) {
      fprintf(stderr, "CPac refused\n");
      return 1;
    }
  }
  ot::Scale scale;
  scale.set(std::atoi(argv[2]), static_cast<unsigned>(std::atoi(argv[3])), face.upem());
  const uint32_t* languages = ot::languageTagsFor(argv[4][0] == '-' ? "" : argv[4]);

  constexpr size_t SCRIPTS = static_cast<size_t>(ot::Script::Myanmar) + 1;
  ot::Plan plans[SCRIPTS];
  bool built[SCRIPTS] = {};
  ot::Buffer buffer;
  static uint8_t storage[ot::Buffer::bytesFor(4096)];  // witchhunt: deviation 1 - past 2040 code points: FAIL
  buffer.setStorage(storage, sizeof(storage));         // witchhunt: deviation 1
  std::string line;
  while (std::getline(std::cin, line)) {
    const std::vector<uint32_t> cps = decodeUtf8(line);
    ot::Script script = ot::Script::Devanagari;
    // Shared codepoints (dandas, joiners) belong to the surrounding script, as in ComplexShaper.
    bool arabic = false;
    for (const uint32_t cp : cps) {
      if (isArabic(cp)) {
        arabic = true;
        script = ot::Script::Arabic;
        break;
      }
    }
    bool myanmar = false;
    for (size_t k = 0; !arabic && k < cps.size(); k++) {
      if (isMyanmar(cps[k])) {
        myanmar = true;
        script = ot::Script::Myanmar;
        break;
      }
    }
    for (size_t k = 0; !arabic && !myanmar && k < cps.size(); k++) {
      if (const indic::ScriptInfo* info = indic::scriptOf(cps[k])) {
        script = static_cast<ot::Script>(indic::indexOf(*info));
        break;
      }
    }
    const auto si = static_cast<size_t>(script);
    if (!built[si]) {
      // .cpfont layout fonts carry compiled plans; TTF/OTF files are planned here.
      if (!plans[si].load(face, script, languages)) plans[si].build(face, script, languages);
      built[si] = true;
    }
    if (!ot::shape(face, scale, plans[si], cps.data(), static_cast<unsigned>(cps.size()), buffer)) {
      printf("FAIL\n");
      continue;
    }
    for (unsigned i = 0; i < buffer.len(); i++) {
      printf("%s%u+%d+%d+%d", i ? " " : "", buffer.info[i].codepoint, buffer.pos[i].xAdvance, buffer.pos[i].xOffset,
             buffer.pos[i].yOffset);
    }
    printf("\n");
  }
  return 0;
}
