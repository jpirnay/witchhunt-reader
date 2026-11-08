#pragma once

// witchhunt: our file. The ShapingFace test double of writing modes step 2, PR 3: the shaping tests of TtfFontTest,
// TextShapingTest, WordSizeLayoutTest and EpubPipelineTest shape through it, so a face is a font in memory and the
// glyph store is not needed. With it, GsubFace: a face over a font whose GSUB a test writes (deletingGsub).

#include <OtFace.h>
#include <ShapingFace.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

/// The bytes of `dir`/`name`; empty when it cannot be read.
inline std::vector<uint8_t> readFont(const char* dir, const char* name) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen((std::string(dir) + "/" + name).c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

/// What a FixtureShapingFace drops from its font's tables (outside the class: a nested type with default member
/// initializers cannot be the class's own default argument).
struct FixtureFaceOptions {
  bool keepCpac = true;
  bool keepLayout = true;
};

/// A ShapingFace over font bytes held in memory: map() returns them (the same address every time, so the face is never
/// re-pointed), and tables() is ot::tablesFromSfnt over them with CPpl cleared (the face computes its plans, as the
/// firmware's does), less what the options drop.
///
/// Constructor: FixtureShapingFace(std::vector<uint8_t> font, Options options = {}). The face keeps `font`; the bytes
/// must be a whole sfnt (a .layout fixture or a TrueType file). Options (all default to the firmware's face):
///   keepCpac   false: CPac is cleared too, so the face computes its filters.
///   keepLayout false: GSUB and GPOS are cleared, a face without layout (it still has head, hhea, maxp, hmtx, cmap,
///   GDEF).
/// Public state, changeable between calls:
///   mapped     false: map() returns nullptr, as a block that cannot be mapped now (current() is nullptr; true again,
///              it builds).
///   mapCalls   how many times map() was asked.
class FixtureShapingFace final : public ShapingFace {
 public:
  using Options = FixtureFaceOptions;

  explicit FixtureShapingFace(std::vector<uint8_t> font, const Options options = {})
      : bytes_(std::move(font)), options_(options) {}

  bool mapped = true;
  int mapCalls = 0;

 protected:
  const uint8_t* map() override {
    ++mapCalls;
    return mapped && !bytes_.empty() ? bytes_.data() : nullptr;
  }
  bool tables(const uint8_t* base, ot::FaceTables* out) override {
    if (!ot::tablesFromSfnt(base, static_cast<uint32_t>(bytes_.size()), out)) return false;
    (*out)[ot::FaceTables::CPPL] = ot::Table();
    if (!options_.keepCpac) (*out)[ot::FaceTables::CPAC] = ot::Table();
    if (!options_.keepLayout) {
      (*out)[ot::FaceTables::GSUB_TABLE] = ot::Table();
      (*out)[ot::FaceTables::GPOS_TABLE] = ot::Table();
    }
    return true;
  }

 private:
  std::vector<uint8_t> bytes_;
  Options options_;
};

// A GSUB of one lookup, ccmp in the default language system of the script `tag` (dev2 unless named): a MultipleSubst
// of `glyph` to no glyph, which deletes it. Big-endian fields, each table at the offset its parent names.
inline std::vector<uint8_t> deletingGsub(const uint16_t glyph, const char (&tag)[5] = "dev2") {
  const auto tagHalf = [&](const int at) { return static_cast<uint16_t>(tag[at] << 8 | tag[at + 1]); };
  const uint16_t fields[] = {
      1, 0,          10,         30, 44,  // the header (version 1.0) and its script, feature and lookup lists
      1, tagHalf(0), tagHalf(2), 8,       // the script list (at 10): one script, `tag`
      4, 0,                               // the script (at 18): its default language system, no other
      0, 0xFFFF,     1,          0,       // the language system (at 22): no required feature, feature 0
      1, 0x6363,     0x6D70,     8,       // the feature list (at 30): one feature, "ccmp"
      0, 1,          0,                   // the feature (at 38): lookup 0
      1, 4,                               // the lookup list (at 44): one lookup
      2, 0,          1,          8,       // the lookup (at 48): a MultipleSubst of one subtable
      1, 10,         1,          8,       // the subtable (at 56): its coverage at 66, its one sequence at 64
      0,                                  // the sequence (at 64): no glyph
      1, 1,          glyph,               // the coverage (at 66): the glyph
  };
  std::vector<uint8_t> out;
  for (const uint16_t field : fields) {
    out.push_back(static_cast<uint8_t>(field >> 8));
    out.push_back(static_cast<uint8_t>(field & 0xFF));
  }
  return out;
}

// A face over a font with its GSUB replaced by `gsub`, and no GPOS, CPac or CPpl: it computes its filters.
class GsubFace final : public ShapingFace {
 public:
  GsubFace(std::vector<uint8_t> font, std::vector<uint8_t> gsub) : font_(std::move(font)), gsub_(std::move(gsub)) {}

 protected:
  const uint8_t* map() override { return font_.empty() ? nullptr : font_.data(); }
  bool tables(const uint8_t* base, ot::FaceTables* out) override {
    if (!ot::tablesFromSfnt(base, static_cast<uint32_t>(font_.size()), out)) return false;
    (*out)[ot::FaceTables::GSUB_TABLE] = ot::Table(gsub_.data(), static_cast<uint32_t>(gsub_.size()));
    (*out)[ot::FaceTables::GPOS_TABLE] = ot::Table();
    (*out)[ot::FaceTables::CPAC] = ot::Table();
    (*out)[ot::FaceTables::CPPL] = ot::Table();
    return true;
  }

 private:
  std::vector<uint8_t> font_;
  std::vector<uint8_t> gsub_;
};
