#pragma once

// witchhunt: our file. The ShapingFace test double of writing modes step 2, PR 3: the shaping tests of TtfFontTest,
// TextShapingTest, WordSizeLayoutTest and EpubPipelineTest shape through it, so a face is a font in memory and the
// glyph store is not needed.

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
