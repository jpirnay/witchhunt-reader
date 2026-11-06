#pragma once

#include <OtFace.h>
#include <OtPlan.h>

#include <cstdint>
#include <memory>

/// One TrueType file's shaping tables for a section build (writing modes step 2, deviations 3 and 12): an ot::Face
/// over the tables where the glyph store maps them, nothing copied. Made by a font's provider on first ask
/// (GlyphProvider::shapingFace) and kept until the font set's releaseShapingFaces() or its next load (a reload or an
/// install opens the styles again, which frees every face). The plans built on it (plan()) are its own: they go first.
class ShapingFace {
 public:
  virtual ~ShapingFace() = default;

  /// The face as mapped now. Fetches the mapping first and renders no glyph. The first call builds the face
  /// (Face::init, CPac validated); a later one whose mapping moved re-points it (Face::rebase). nullptr when the
  /// block cannot be mapped now (asked again next time) or init() refused the tables (never again). Call it
  /// before each Plan::build and each ot::shape: a glyph lookup in another flash-mode style moves the mapping.
  /// Only under RenderLock, as it maps the glyph store as a glyph lookup does. The face lives until the font set's
  /// releaseShapingFaces() or its next load (TtfFontSet::load()); its plans (plan()) go with it.
  const ot::Face* current();

  /// The plan for `script` in the language system `languageTags` (ot::languageTagsFor's array, compared by address;
  /// null: the script's default), built on first ask with makeUniqueNoThrow (about 1-4 KB) and kept with the face, by
  /// unique_ptr, never moved: no plan outlives the face it points into. nullptr when the face is not mapped now, the
  /// plan does not build, or there is no memory; asked again, it tries again. Under RenderLock, as current() is.
  /// The plan is valid until the next plan() on this face (a new key past kMaxPlans frees one) or until the face is
  /// freed: ask again before each ot::shape rather than holding the pointer.
  const ot::Plan* plan(ot::Script script, const uint32_t* languageTags);
  /// Plans a face keeps; a new one past them frees the one asked for least recently.
  static constexpr uint8_t kMaxPlans = 2;
  /// Plans built since the face was made: for tests and the build's log.
  uint16_t planBuilds() const { return planBuilds_; }

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

  // Declared after face_: the plans point into it, so they are destroyed first.
  struct PlanSlot {
    std::unique_ptr<ot::Plan> plan;
    const uint32_t* languageTags = nullptr;
    ot::Script script = ot::Script::Devanagari;
    uint32_t lastAsked = 0;
  };
  PlanSlot slots_[kMaxPlans];
  uint32_t asks_ = 0;
  uint16_t planBuilds_ = 0;
};
