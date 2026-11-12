---
name: scope-discipline
description: Feature-scope discipline for a dedicated e-reader (not a Swiss Army knife). Use when adding a feature, a new activity, a new lib, a setting, or a dependency, or when a request would grow the firmware's surface. Covers the SCOPE.md test, the RAM-cost vs reading-benefit gate, preferring no-code or existing-mechanism solutions, awareness of the existing activity surface, and how to push back on out-of-scope asks.
---

# Scope Discipline

The mission: do one thing exceptionally well, focused reading on constrained
hardware. Read `SCOPE.md` before adding surface. This is the gate to run before
writing a new feature.

`SCOPE.md` is inherited from upstream CrossPoint and is not a complete record of
this fork's decisions. The fork already ships things it calls out of reach or
does not list: a clock on boards with an RTC, weather, Dark Mode, and SD-card
web plugins. Treat `SCOPE.md` as the default test. When a request sits at its
edge, ask the user instead of deciding from the text alone. Earlier decisions
live in `docs/design/` and `docs/future_work/`; check there first.

## The gate

Before adding a feature, activity, lib, setting, or dependency, answer in order:

1. **Is it in `SCOPE.md`?** Explicitly out: interactive apps (notepad,
   calculator, games), active connectivity (RSS, news, browser), media/audio
   playback. If it is out, say so and stop.
2. **Does it materially improve focused reading?** If the benefit is
   "nice to have" or serves a different use case, it is out. This is not a PDA.
3. **What does it cost on the X3?** Steady-state RAM, the largest transient
   block, and flash. On the C3, IRAM comes out of the heap, and each 64 KB of
   app growth costs the font cache an MMU map page. The partition table cannot
   change over OTA, so the app slot does not grow. A feature that adds cost
   needs a reading benefit that clearly outweighs it. Measure (the `RAM:` and
   `Flash:` lines of `pio run` against master, `scripts/firmware_size_history.py`,
   `scripts/script_profile_mem.sh`, a device `STATE` heap reading) rather than
   guess.
4. **Can it be done with no new firmware code?** Prefer an existing activity,
   an existing setting, a doc, or, for a web-side feature, an SD-card plugin
   (`docs/sd-plugins.md`), which needs no firmware build. The cheapest feature
   is the one already built.

If a request fails the gate, push back with the specific reason and the
`SCOPE.md` basis, and offer the in-scope alternative. Make the call and say why;
do not just hand over a menu.

## Surface awareness

The firmware already carries dozens of activities. Each new one is permanent
RAM, permanent maintenance, and another thing every future refactor must not
break. Default to extending an existing activity or setting before adding a new
screen. New top-level surface needs a real justification, not "it would be
convenient."

## Settings are not free

A new setting is a field to persist, migrate, validate, translate into every
UI language, and render on the device and the web page, plus combinatorial
test burden. Add one only when users genuinely need the choice; otherwise pick a
sensible fixed default. A setting that only some boards can use is gated on a
capability, not shown and ignored.

## Self-review

- [ ] Checked against `SCOPE.md`; not on the out-of-scope list.
- [ ] Stated the concrete reading benefit, not a generic "useful."
- [ ] Named the RAM, largest-block and flash cost on the X3 (measured, not
      guessed) and why the benefit wins.
- [ ] Checked whether an existing activity/setting/doc already covers it.
- [ ] New setting (if any) is justified by a real user need, not added
      "just in case."
