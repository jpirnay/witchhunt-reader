#pragma once

#include <OtFace.h>

#include <cstdint>

/// One TrueType file's shaping tables for a section build (writing modes step 2, deviations 3 and 12): an ot::Face
/// over the tables where the glyph store maps them, nothing copied. Made by a font's provider on first ask
/// (GlyphProvider::shapingFace) and kept until the font set's releaseShapingFaces(); plans built on it go first.
class ShapingFace {
 public:
  virtual ~ShapingFace() = default;

  /// The face as mapped now. Fetches the mapping first and renders no glyph. The first call builds the face
  /// (Face::init, CPac validated); a later one whose mapping moved re-points it (Face::rebase). nullptr when the
  /// block cannot be mapped now (asked again next time) or init() refused the tables (never again). Call it
  /// before each Plan::build and each ot::shape: a glyph lookup in another flash-mode style moves the mapping.
  const ot::Face* current();

  /// Whether the last current() re-pointed the face because its mapping had moved.
  bool repointed() const { return repointed_; }
  /// The length of the CPac table the face uses, 0 when it has no filters; known once current() built the face.
  uint32_t cpacBytes() const { return cpacBytes_; }

 protected:
  virtual const uint8_t* map() = 0;                                   // the block's base now; nullptr: unmapped
  virtual bool tables(const uint8_t* base, ot::FaceTables* out) = 0;  // the views over the block at `base`

 private:
  ot::Face face_;
  const uint8_t* base_ = nullptr;
  uint32_t cpacBytes_ = 0;
  enum class State : uint8_t { Unbuilt, Built, Refused } state_ = State::Unbuilt;
  bool repointed_ = false;
};
