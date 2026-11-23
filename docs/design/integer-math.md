# Integer maths — design

> **Status.** Done in six areas (#862, #863, #864, #866, #867, #868). The X3/X4 build's own code makes no soft-float
> call (`scripts/float_inventory.py`: 0 call sites after #868). `scripts/check_no_float.py` keeps it so: it fails the
> build when an object built from a source in its `FLOAT_FREE` list calls a soft-float, libm or float-parsing routine.
> Host tests: `test/image_scale`, `test/text_scale`, `test/progress_ppb`, `test/byte_size`, `test/fixed_decimal`,
> `test/weather_units`, `test/clock_drift`, `test/imu_motion`, `CssNumberTest` in `test/epub_css`.

## Why

The C3 boards (X3, X4) have no FPU: every float or double operation is a call into a libgcc soft-float helper, tens of
cycles each. Float rounding can also land a pixel short of a whole result (`340.0f / 1020 * 1020` is 339.99997, the
off-by-one behind thumbnails of the wrong size). The S3 boards have a single-precision FPU, but share the code.

## What it bought

Little flash. newlib's `printf` and `strtod`, ArduinoJson, libstdc++'s hash tables and ESP-IDF call the same helpers,
so they stay in the image: 18.8 KB of helpers when the work started, 17.9 KB after it (#868). The saving is the call
sites' own code (the table below).

Speed where float ran per pixel or per glyph. Area 1: `drawBitmap1Bit`'s downscale ran four soft-float calls per
source pixel; a scaled 480x800 1-bit `/sleep.bmp` in the BMP viewer went from 1,426 to 554 ms on the X3. Area 2 runs
per glyph and per word; its device speed was not measured. The other areas follow the rule so that the check can cover
whole files.

## How

- **Images** scale by exact ratios: `ImageScale` (`lib/GfxRenderer/ImageScale.h`), `num` destination pixels per `den`
  source pixels, floored as `v * num / den` in 32-bit hardware arithmetic. `cutToBox` cuts an image to a box's shape
  and covers the box to its last pixel and no further.
- **Text** scales by `TextScale` (`lib/EpdFont/TextScale.h`): 2^-20ths of 1, built rounding up onto the grid. Its
  `apply()`, `applySymmetric()` and `applyTruncated()` each reproduce one float expression the code used, so the
  conversion moved no line break: 2^-20 rounded up was the only grid tried (against 1/1024, 1/4096 and 1/65536) that
  changes no non-half product below 2048.
- **CSS** numbers are read exactly as num / den (`CssNumber`, `lib/Epub/Epub/css/CssNumber.h`), without `strtof`;
  `CssLength` is 16.16 fixed point. The font-size multiplier, line height and line compression are `TextScale`.
- **Progress** is `ProgressPpb` (`lib/Epub/Epub/ProgressPpb.h`): parts per billion in a `uint32_t`, floored, with one
  rounding function per display rule. Billionths, not millionths: the sync screen shows hundredths of a percent with
  printf's ties-to-even, and flooring to millionths turned about one value in a hundred into a false exact half there.
- **Clock and sensors** keep the drift model in integers (`lib/hal/ClockDrift.h`): temperatures in hundredths of a
  degree, the learned drift scale in millionths, the correction in ppb. The IMU reports mg and mdps, the battery
  divider is held in thousandths (both in the SDK).
- **UI and weather**: one byte-size formatter (`src/util/ByteSize.h`), fixed decimals
  (`lib/Serialization/FixedDecimal.h`), the weather in stated units (`lib/Weather/WeatherUnits.h`).
- **A float from outside is turned into an integer once, where it enters**, mostly from its bits: KOReader's
  `percentage` (`lib/KOReaderSync/KOReaderJsonNumber.h`), the weather API's numbers, the IDF temperature sensor and the
  old stored floats (`lib/hal/FloatBits.h`), the SDK's touch taps (`lib/GfxRenderer/GfxRendererTouch.cpp`, compiled
  out on the C3). ArduinoJson and newlib keep their own float code.
- **A changed on-card format bumps its version constant**, and old files stay readable where a version cannot say it.
- **The check reads objects**, not sources, so a header's inline code counts in the files that include it. A listed
  source that built nothing on a board is reported, so a renamed file does not drop out unnoticed.

## The six areas

| Area | PR | Types | Formats | X3/X4 flash |
|---|---|---|---|---|
| 1 Image scaling | #862 | `ImageScale`, `drawBitmap` takes the visible size in source pixels (`BitmapRowTarget.h`) | none | -1,292 B |
| 2 Text scale | #867 | `TextScale` | `SECTION_FILE_VERSION` 129 -> 130 (TextBlock's multiplier) | -346 B |
| 3 CSS lengths and layout | #868 | `CssNumber`, 16.16 `CssLength` | `CSS_CACHE_VERSION` 36 -> 37, `SECTION_FILE_VERSION` 130 -> 131, compiled content 1 -> 2 | -896 B |
| 4 Progress and sync | #863 | `ProgressPpb`, `SeriesIndex.h` | none; KOSync's upload is byte-identical | -158 B |
| 5 Clock and sensors | #866 | `ClockDrift.h`, `FloatBits.h`; SDK `Imu`, `BatteryMonitor` | NVS `driftmicro`/`lasttempcc` (the old float keys still read), a new RTC-memory magic (an old snapshot converted) | +298 B |
| 6 UI screens | #864 | `ByteSize`, `FixedDecimal`, `WeatherUnits` | `weather_settings.json` adds `latitudeE6`/`longitudeE6` and keeps the old keys; `weather_cache.json` integers (`layout=2`) | -768 B |

Each flash figure is the PR's own, against its base. Area 5 grew: integer code with 64-bit guards and the old-format
readers is larger than the soft-float calls it replaced, and no helper left the image.

Known one-step differences against the float code, each found by a sweep in its PR: a scaled image can differ by one
row or column where float rounding was inexact (#862); integer page estimates (a cross-font rescale, the sync density
estimate) can land one page off where float fell just below a whole number (#863); a product that is exactly a half
rounds up (30 word positions of one centred heading moved 1 px, #867); ten image boxes in the corpus are 1 px
larger, where float computed 459.99997 and truncated it (#868); the weather settings' four-decimal coordinate rows
differ in about 0.4 % of values, where the old text depended on float noise (#864).

## What stays float

- ArduinoJson's number templates, compiled into `WeatherClient`, `WeatherSettingsStore`, `PluginEventCore`,
  `CrossPointWebServer`, `KOReaderSyncClient` and `KOReaderAutoSync`. These stay out of `FLOAT_FREE`; no arithmetic of
  ours is left in them.
- The SDK's touch taps (`GfxRendererTouch.cpp`), outside the C3 build.
- `HalCapabilities::uiScale()` returns the profile's float `uiScale`; nothing calls it.

## Measuring

`python scripts/float_inventory.py .pio/build/default/firmware.elf --helpers` lists every function of ours that calls
a float helper, with call counts, and which helpers only our code uses. Compare it before and after a change, with the
`Flash:` line of `pio run -e default` from the same worktree. Before area 1 it found 1,393 call sites in 195 functions;
after area 1, 1,100 in 170; after area 3, none. Code inlined from a library header, such as a lambda inside
`std::function`, is listed under that header; `--all` shows it.

## Not tested on a device

- Area 1 ran on the X3 and the Murphy, pixel-identical to the float build; the sleep screen's crop mode and the cover
  tile's column cut were not exercised on a device.
- Areas 2, 3, 4 and 6 are host-tested only. Area 2's page render and section build time on the X3 were not measured.
- Area 5 ran on the X3 and the Picco (the clock continuous across the update's boot and a silent reboot), but both
  take their time from a hardware RTC. The drift model runs only on the plain X4, which has none, and was not run
  there; nor were an NTP sync and a deep sleep with its wake.

## Next

`check_no_float.py` could check all of `src/` and `lib/` with a list of exceptions, instead of a list of float-free
sources.
