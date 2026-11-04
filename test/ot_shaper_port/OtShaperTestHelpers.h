#pragma once

// What OtShaperPortTest and OtShaperAllocTest share: the fixture fonts of
// test/complex_shaper (SHAPING_FIXTURE_DIR), a UTF-8 decoder that allocates
// nothing, and shaping a run at the size ExpectedShaping.h was stored at.

#include <ExpectedShaping.h>
#include <OtShaper.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ottest {

// Decodes `utf8` into `out`, writing at most `capacity` code points, and
// returns how many the string holds: more than `capacity` when it does not
// fit. Allocates nothing.
inline size_t decodeUtf8(const char* utf8, uint32_t* out, const size_t capacity) {
  size_t count = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    uint32_t cp = *p;
    const int n = cp < 0x80 ? 1 : (cp >> 5) == 6 ? 2 : (cp >> 4) == 14 ? 3 : 4;
    cp = n == 1 ? cp : n == 2 ? cp & 0x1F : n == 3 ? cp & 0x0F : cp & 0x07;
    for (int k = 1; k < n && p[k]; k++) cp = (cp << 6) | (p[k] & 0x3F);
    if (count < capacity) out[count] = cp;
    count++;
    p += n;
  }
  return count;
}

// The same, into a vector.
inline std::vector<uint32_t> utf32(const char* utf8) {
  std::vector<uint32_t> out(decodeUtf8(utf8, nullptr, 0));
  decodeUtf8(utf8, out.data(), out.size());
  return out;
}

inline std::vector<uint8_t> readFixture(const char* name) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen((std::string(SHAPING_FIXTURE_DIR) + "/" + name).c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

// The ot::Script of a ShapingFixture's script name; false when unknown.
inline bool scriptOf(const char* name, ot::Script* out) {
  static constexpr struct {
    const char* name;
    ot::Script script;
  } ALL[] = {{"Bengali", ot::Script::Bengali},
             {"Devanagari", ot::Script::Devanagari},
             {"Tamil", ot::Script::Tamil},
             {"Sinhala", ot::Script::Sinhala}};
  for (const auto& e : ALL) {
    if (std::strcmp(name, e.name) == 0) {
      *out = e.script;
      return true;
    }
  }
  return false;
}

// Shapes `count` code points at the fixtures' size (kFixturePpem26_6).
inline bool shapeRun(const ot::Face& face, const ot::Plan& plan, const uint32_t* cps, const unsigned count,
                     ot::Buffer& buffer) {
  ot::Scale scale;
  scale.set(kFixturePpem26_6, (kFixturePpem26_6 + 32) >> 6, face.upem());
  return ot::shape(face, scale, plan, cps, count, buffer);
}

// The shaped run: glyph IDs, advances and offsets in scaled units.
inline std::vector<int32_t> glyphsOf(const ot::Buffer& buffer) {
  std::vector<int32_t> out;
  for (unsigned i = 0; i < buffer.len(); i++) {
    const ot::GlyphPosition& p = buffer.pos[i];
    out.insert(out.end(),
               {static_cast<int32_t>(buffer.info[i].codepoint), p.xAdvance, p.yAdvance, p.xOffset, p.yOffset});
  }
  return out;
}

}  // namespace ottest
