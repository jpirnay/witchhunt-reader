#include "ShapingFace.h"

const ot::Face* ShapingFace::current() {
  if (state_ == State::Refused) return nullptr;
  repointed_ = false;
  const uint8_t* const base = map();
  if (!base) return nullptr;
  if (state_ == State::Unbuilt) {
    ot::FaceTables found;
    state_ = tables(base, &found) && face_.init(found) ? State::Built : State::Refused;
    if (state_ == State::Refused) return nullptr;
    base_ = base;
    // The CPac the face took its filters from; a table that failed validation left it without.
    cpacBytes_ = face_.filters(ot::GSUB) || face_.filters(ot::GPOS) ? found[ot::FaceTables::CPAC].length() : 0;
  } else if (base != base_) {
    face_.rebase(base_, base);
    base_ = base;
    repointed_ = true;
  }
  return &face_;
}
