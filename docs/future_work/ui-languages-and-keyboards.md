# Interface languages and keyboards

Status: open as of 2026-11-23. How languages and their card packs work is in [../i18n.md](../i18n.md). Which layouts a
keyboard offers is `src/activities/util/KeyboardLayoutSet.h`; the layouts themselves are the SDK's
(`freeink-sdk/libs/ui/FreeInkUI/include/components/keyboard/keyboard.h`, `KeyboardLayoutId`).

## Estonian as an interface language (CrossInk 8ba743e6): decide

**Open.** The interface has 30 languages, English included (`lib/I18n/translations/`), Estonian not among them.
CrossInk added one. Its keys differ from ours and must be mapped onto `english.yaml`; keys left out fall back to
English. Latin script, so no font work; it would ship as a card pack like the others (`gen_i18n.py --packs`).
Hyphenation patterns exist: typst/hypher, the source `scripts/update_hyphenation.sh` downloads tries from, has
`tries/et.bin`, so an Estonian hyphenation pack can be built as the others are (`scripts/build_hyphenation_packs.py`).
Size M. Translation by Paavo Pihelgas (paavoph), with Julia Nguyen (@uxjulia).

## A Vietnamese keyboard (Free-Ink #90, draft): decide

**Open.** We ship a Vietnamese interface (`lib/I18n/translations/vietnamese.yaml`) but no Vietnamese layout: the
pin's `KeyboardLayoutId` ends at `GreekEl`. Free-Ink #90 is still an open draft. Choice: take a layout from this draft
now, or wait for an upstream input method.

Taking it is a port, not a merge:
- **The SDK.** The draft adds to `builtinKeyboardLayout()`, which the firmware must not call: it brings back ~15 KB of
  expanded tables (`platformio.ini`, the comment on the FreeInkUI library entry). It needs a compact-form layout for
  `buildKeyboardLayout()` and an enum value appended after `GreekEl`, landed on the SDK's `witchhunt` branch first.
- **The firmware.** A row appended to `keyboard_layouts::ALL` (`KeyboardLayoutSet.h`), never inserted, since a row's
  position is its bit in the saved `keyboardLayouts` mask (11 of 16 bits used): `{KeyboardLayoutId::<the new value>,
  Language::VI, nullptr}`, so a Vietnamese interface opens its keyboard on it. Whether it joins `LATIN_BITS` (the
  layouts that can type a URL or a Wi-Fi password) follows from the next choice.
- **Combining or precomposed.** The draft, in VNI order, types combining marks, which puts decomposed (NFD) text into
  passwords and search fields; precomposed letters would not.

Free-Ink #90, brianhuster (@brianhuster).

## A CJK keyboard

**Open.** None of Chinese, Japanese and Korean has a layout (`KeyboardLayoutId`), so text entry stays Latin under a
CJK interface. No design or size yet. Next step: decide whether it is in scope ([SCOPE.md](../../SCOPE.md)), then
what each language would type with.
