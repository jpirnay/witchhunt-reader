# UI screens without float

Part of [Integer maths](../integer-math.md). Area 6. Counts from `scripts/float_inventory.py` on 2026-11-21, after area 1.

**Open.** Screens and the web server format sizes, speeds and percentages with float, and the weather screens keep
coordinates and temperatures as floats: about 130 call sites in 25 functions. Mostly `printf("%.1f")`; newlib's
`printf` keeps its float support either way, so this is for the rule and the check, not for flash.

**Where.** `CrossPointWebServer.cpp` (37: `handleUpload`, `onWebSocketEvent`, `handleFontDownload`),
`WeatherActivity.cpp` (27: the hourly graph, current conditions, days, details), `BaseTheme.cpp` (12:
`drawHourglass`), `FontDownloadActivity.cpp` (9), `OtaUpdateActivity.cpp` (9), `SystemInformationActivity.cpp` (8),
`WeatherSettingsStore.cpp` (6), `BookInfoActivity.cpp` (5: `formatFileSize`), `WeatherClient.cpp` (4),
`WeatherSettingsActivity.cpp` (3 plus two inlined lambdas parsing latitude and longitude with `strtof`),
`FlashLayoutNotice.cpp` (2), `PluginEventCore.cpp` (2).

**Format.** `WeatherSettingsStore` saves latitude and longitude as floats: keep reading the old file, write the new
integer form (for example micro-degrees), as `JsonSettingsIO` loads a missing key's default.

**Boundary.** The weather API's JSON numbers come in through ArduinoJson: convert them where `WeatherClient` reads
them.

**Next step.** One shared helper for "a size in KB/MB with one decimal" (`formatBytes`, `formatFileSize`,
`formatSize`, `sizeText` do the same thing), integer tenths for speeds and percentages, then the weather types.
