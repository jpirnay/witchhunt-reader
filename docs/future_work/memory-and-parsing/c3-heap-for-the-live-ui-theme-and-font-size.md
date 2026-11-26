# The live UI theme and font size on the C3s

Part of [Memory and parsing](../memory-and-parsing.md).

**Open, needs an X4 measurement.** A new UI theme or UI font size takes effect in Settings at once on the PSRAM boards
(`SettingsActivity::applyUiLookIfChanged`). The C3s still apply it when Settings closes, as before, behind
`HalCapabilities::defersUiLookChange()` (`lib/hal/HalCapabilities.h`).

What the live change does: under the exclusive render lock it binds the UI fonts again (`applyUiFontScale()`), replaces
the theme object (`UITheme::reload()`, a `std::make_unique` of the new theme) and lays Settings out again, while the
screens below Settings stay resident. Home's carousel frame cache and cover buffers are not touched by it.

On the T5S3 the internal heap in Settings stayed at about 109 KB free across a Normal → Large → Normal and a
Lyra Tiles → Classic → Lyra Tiles round trip (2026-11-26). Nobody has measured it on a C3.

**Next step:** on the X4 (and the X3 when it is back), open Settings from Home and from the reader, change the theme
and the font size there several times with the predicate forced to false, and read `CMD:STATE`'s `heap` and
`maxblock` before and after each change, then go back to Home and open a book. If the free heap and the largest block
come back, make `defersUiLookChange()` false everywhere and delete this file.
