# Interface languages and keyboards

Status: open decisions as of 2026-11-15, from the upstream triage of that day. How languages and their card packs work
is in [../i18n.md](../i18n.md).

## Estonian as an interface language (CrossInk 8ba743e6): decide

**Open.** The interface has 30 languages (`lib/I18n/translations/`), Estonian not among them. CrossInk added one. Its
keys differ from ours and must be mapped onto `english.yaml`; keys left out fall back to English. Latin script, so no
font work; it would ship as a card pack like the others (`gen_i18n.py --packs`). Check whether Estonian hyphenation
patterns exist for a pack. Size M. Translation by Paavo Pihelgas (paavoph), with Julia Nguyen (@uxjulia).

## A Vietnamese keyboard (Free-Ink #90, draft): decide

**Open.** We ship a Vietnamese interface (`lib/I18n/translations/vietnamese.yaml`) but no Vietnamese layout: the pin's
`KeyboardLayoutId` ends at `GreekEl` (`freeink-sdk/libs/ui/FreeInkUI/include/components/keyboard/keyboard.h:22-47`).
Choice: take a layout from this draft now, or wait for an upstream input method.

Taking it is a port, not a merge. The draft adds to `builtinKeyboardLayout()`, which the firmware must not call
(it brings back ~15 KB of expanded tables, `platformio.ini:178-183`); it needs a compact-form layout and an enum value
appended after `GreekEl`. It also needs a choice between combining marks (the draft, in VNI order, puts decomposed NFD
text into passwords and search fields) and precomposed letters. Free-Ink #90, brianhuster (@brianhuster).
