# Integer maths

The firmware moves off floating point, one area per PR. The open areas are one file each in
[integer-math/](integer-math/). A PR that finishes an area deletes its file, adds the area's sources to `FLOAT_FREE`
in `scripts/check_no_float.py`, and leaves this page and the README row alone.

**Why.** The C3 boards (X3, X4) have no FPU: every float or double operation is a call into a libgcc soft-float helper,
tens of cycles each, and float rounding can land a pixel short of a whole result (`340.0f / 1020 * 1020` is
339.99997). The S3 boards have a single-precision FPU, but share the code.

**What it buys, measured.** Little flash: newlib's `printf` and `strtod`, ArduinoJson, libstdc++'s hash tables and
ESP-IDF call the same helpers, so a float-free firmware keeps all but about 0.7 KB of the 18.8 KB of helpers. The
rest is the call sites' own code: area 1 (image scaling) took 1,292 bytes off the X3/X4 image. Speed matters where
float runs per pixel or per glyph (areas 1 and 2); the other areas follow the rule so that the check can cover whole
files.

**How.**

- Images scale by exact ratios: `ImageScale` (`lib/GfxRenderer/ImageScale.h`), `num` destination pixels per `den`
  source pixels, floored as `v * num / den`. `cutToBox` fills a box exactly.
- Text sizes, CSS lengths and progress use fixed point (an integer in a stated unit: 1/1024 of a scale, hundredths of
  a percent, and so on), chosen per area and named in its types.
- A float that arrives from outside (KOReader's JSON, the weather API, the IDF temperature sensor) is turned into an
  integer once, where it enters. ArduinoJson and newlib keep their own float code.
- A changed on-card format (the CSS cache, section files, settings) bumps its version constant.
- `scripts/check_no_float.py` fails the build when an object built from a listed source calls a soft-float, libm or
  float-parsing routine. It reads objects, so a header's inline code counts in the files that include it.

**Measuring.** `python scripts/float_inventory.py .pio/build/default/firmware.elf --helpers` lists every function
of ours that calls a float helper, with call counts, and which helpers only our code uses. Compare it before and
after an area, with the `Flash:` line of `pio run -e default` from the same worktree. On 2026-11-21 master had 1,393
call sites in 195 functions; after area 1, 1,100 in 170. (Code inlined from a library header, such as a lambda
inside `std::function`, is listed under that header; `--all` shows it.)

**Areas.** The work was split into six areas on 2026-11-21: 1 image scaling, 2 text scale, 3 CSS lengths and
layout, 4 progress and sync, 5 clock and sensors, 6 UI screens. Area 1 landed with this page; the folder holds the
others that are still open. Areas 2 and 3 share the font-size multiplier that CSS hands to text scaling: whichever
goes first defines its fixed-point type, and the other adopts it.
