#include <ExpectedShaping.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The engine (lib/OtShaper) against the HarfBuzz output stored in
// test/complex_shaper/ExpectedShaping.h, converted the way ComplexShaper's
// appendGlyphs stored it.

static_assert(ot::MAX_NESTING_LEVEL == 8);
static_assert(ot::MAX_CONTEXT_LENGTH == 64);
static_assert(ot::MAX_PLANNED_LOOKUPS == 4096);

namespace {

struct StoredGlyph {
  uint32_t gid;
  int advance12_4;
  int dx;
  int dy;
  bool operator==(const StoredGlyph&) const = default;
};

std::ostream& operator<<(std::ostream& os, const StoredGlyph& g) {
  return os << "{" << g.gid << "," << g.advance12_4 << "," << g.dx << "," << g.dy << "}";
}

int roundPixels(const int v) { return v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64); }

std::vector<uint32_t> utf32(const char* utf8) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    uint32_t cp = *p;
    const int n = cp < 0x80 ? 1 : (cp >> 5) == 6 ? 2 : (cp >> 4) == 14 ? 3 : 4;
    cp = n == 1 ? cp : n == 2 ? cp & 0x1F : n == 3 ? cp & 0x0F : cp & 0x07;
    for (int k = 1; k < n && p[k]; k++) cp = (cp << 6) | (p[k] & 0x3F);
    out.push_back(cp);
    p += n;
  }
  return out;
}

std::vector<uint8_t> readFixture(const char* name) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen((std::string(SHAPING_FIXTURE_DIR) + "/" + name).c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

// Shapes `utf8` and converts each glyph to what ExpectedShaping.h stores.
// Empty when ot::shape fails.
std::vector<StoredGlyph> shapeAsStored(const ot::Face& face, const ot::Plan& plan, const char* utf8) {
  ot::Scale scale;
  scale.set(kFixturePpem26_6, (kFixturePpem26_6 + 32) >> 6, face.upem());
  ot::Buffer buffer;
  const std::vector<uint32_t> cps = utf32(utf8);
  std::vector<StoredGlyph> out;
  if (!ot::shape(face, scale, plan, cps.data(), static_cast<unsigned>(cps.size()), buffer)) return out;
  for (unsigned i = 0; i < buffer.len(); i++) {
    out.push_back({buffer.info[i].codepoint, (buffer.pos[i].xAdvance + 2) >> 2, roundPixels(buffer.pos[i].xOffset),
                   -roundPixels(buffer.pos[i].yOffset)});
  }
  return out;
}

bool scriptOf(const std::string& name, ot::Script* out) {
  static const struct {
    const char* name;
    ot::Script script = ot::Script::Devanagari;
  } all[] = {{"Bengali", ot::Script::Bengali},
             {"Devanagari", ot::Script::Devanagari},
             {"Tamil", ot::Script::Tamil},
             {"Sinhala", ot::Script::Sinhala}};
  for (const auto& e : all) {
    if (name == e.name) {
      *out = e.script;
      return true;
    }
  }
  return false;
}

// Every fixture word, built plan against the stored output. `withFilters`
// keeps CPac (the stored-filters route); CPpl is always dropped.
void checkAllWords(const bool withFilters) {
  size_t words = 0;
  for (const ShapingFixture& fx : kShapingFixtures) {
    ot::Script script = ot::Script::Devanagari;
    ASSERT_TRUE(scriptOf(fx.script, &script)) << fx.script;
    const std::vector<uint8_t> font = readFixture(fx.layoutFile);
    ASSERT_FALSE(font.empty()) << fx.layoutFile;
    ot::FaceTables tables;
    ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
    if (!withFilters) tables[ot::FaceTables::CPAC] = ot::Table();
    tables[ot::FaceTables::CPPL] = ot::Table();
    ot::Face face;
    ASSERT_TRUE(face.init(tables));

    for (size_t w = 0; w < fx.count; w++) {
      const ExpectedShaping& word = fx.words[w];
      ot::Plan plan;
      ASSERT_TRUE(plan.build(face, script, ot::languageTagsFor(word.language))) << fx.script << " " << word.utf8;
      std::vector<StoredGlyph> expected;
      for (unsigned i = 0; i < word.count; i++) {
        const ExpectedGlyph& g = word.glyphs[i];
        expected.push_back({g.gid, g.advance12_4, g.dx, g.dy});
      }
      EXPECT_EQ(shapeAsStored(face, plan, word.utf8), expected) << fx.script << " word " << w << ": " << word.utf8;
      words++;
    }
  }
  EXPECT_EQ(words, 56u);
}

}  // namespace

TEST(OtShaperReference, BuiltPlansMatchHarfBuzz) { checkAllWords(false); }

TEST(OtShaperReference, StoredFiltersMatchHarfBuzz) { checkAllWords(true); }
