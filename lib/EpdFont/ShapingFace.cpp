#include "ShapingFace.h"

const ot::Face* ShapingFace::current() {
  if (state_ == State::Refused) return nullptr;
  const uint8_t* const base = map();
  if (!base) return nullptr;
  if (state_ == State::Unbuilt) {
    ot::FaceTables found;
    state_ = tables(base, &found) && face_.init(found) ? State::Built : State::Refused;
    if (state_ == State::Refused) return nullptr;
    base_ = base;
  } else if (base != base_) {
    face_.rebase(base_, base);
    base_ = base;
  }
  return &face_;
}
