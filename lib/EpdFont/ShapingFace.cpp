#include "ShapingFace.h"

#include <Memory.h>

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

const ot::Plan* ShapingFace::plan(const ot::Script script, const uint32_t* languageTags) {
  const ot::Face* const face = current();
  if (!face) return nullptr;
  ++asks_;
  PlanSlot* victim = &slots_[0];
  for (PlanSlot& slot : slots_) {
    if (slot.plan && slot.script == script && slot.languageTags == languageTags) {
      slot.lastAsked = asks_;
      return slot.plan.get();
    }
    // The slot to fill: an empty one, else the one asked for least recently (the counter wraps after 4 billion asks).
    if (!slot.plan) {
      if (victim->plan) victim = &slot;
    } else if (victim->plan && static_cast<int32_t>(slot.lastAsked - victim->lastAsked) < 0) {
      victim = &slot;
    }
  }
  victim->plan.reset();  // its memory first: the new plan needs the room
  // One Plan (about 1-2 KB with its lookup lists), kept for the face's life.
  auto built = makeUniqueNoThrow<ot::Plan>();
  if (!built || !built->build(*face, script, languageTags)) return nullptr;
  ++planBuilds_;
  victim->script = script;
  victim->languageTags = languageTags;
  victim->lastAsked = asks_;
  victim->plan = std::move(built);
  return victim->plan.get();
}
