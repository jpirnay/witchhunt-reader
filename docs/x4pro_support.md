# Xteink X4 Pro support — the frontlight

Scope note: this file currently covers only the **frontlight**. The X4 Pro port is
much larger than that (ESP32-S3, GT911 touch, SSD1677/UC8179/UC8279 panel
selection, USB-MSC); those get their own sections when the work starts. What is
written down here is the part we already had to reason about, because idle light
sleep touches it.

> **Provenance.** The frontlight design is from
> [crosspoint-reader#2983](https://github.com/crosspoint-reader/crosspoint-reader/pull/2983)
> ("feat: Add support for x4pro & papermono devices") by Justin Mitchell
> ([@itsthisjustin](https://github.com/itsthisjustin)), branch
> `feat-x4-papermono-support`. That PR is **still open** — it is on neither
> `develop` nor `master`, so do not go looking for this on the mainline. He
> flagged the light-sleep interaction on our PR
> [#145](https://github.com/jpirnay/witchhunt-reader/pull/145).

## What already exists here

Nothing device-side. `lib/` and `src/` contain no `FrontlightManager` or
`HalFrontlight` reference, and the `Frontlight` singleton does not exist.

One thing **is** in place: [`HalPowerManager::lightSleep()`](../lib/hal/HalPowerManager.cpp)
declines to sleep on any board with a PWM frontlight.

```cpp
if (BoardConfig::hasPwmFrontlight()) {
  lightSleepStats_.rejFrontlight++;
  return false;
}
```

Light sleep stops the LEDC peripheral's clock, so an ESP-driven PWM frontlight
flickers visibly as the idle loop enters slice after slice. Because we have no
driver, we cannot ask whether the light is currently lit, so the guard is
deliberately blunt: it gives up idle light sleep on such a board entirely. It
never fires today (no board we build has a frontlight) and `rejFrontlight` stays 0.

**Tighten this when the HAL lands.** Justin's version is
`Frontlight.present() && Frontlight.isOn()`, so an X4 Pro with the light off still
gets the ~3.2x idle saving from PR #146. Leaving our blunt version in place would
silently cost the X4 Pro all of it.

## The hardware

From the SDK board profile (`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h`,
`XTEINK_X4_PRO`), recovered from the OEM LEDC init:

| Property | Value |
| --- | --- |
| Channels | Two: **GPIO8** (primary/brightness) and **GPIO9** (warm) |
| OEM LEDC channels | ch4 and ch5 |
| Frequency / resolution | 10 kHz, 10-bit |
| Polarity | Active-HIGH (init drives the pin LOW = off; brightness raises duty) |
| `FrontlightConfig` | `{8, 10000, 10, true, 9}` |

Which of GPIO8/GPIO9 is physically warm vs cold **is not confirmed**. If reversed,
the colour-temperature direction simply inverts — worth checking on real hardware
early, since it is a one-character fix but a confusing bug to chase later.

OEM NVS keys, for reference if we ever import stock settings:
`lightWarmValue`, `lightColdValue`, `lightCT`, `lightBri`, `lightOn`.

## The SDK already has the driver

`freeink-sdk/libs/hardware/FrontlightManager` is vendored in our submodule
already — we simply never link it. It is inert on boards without a frontlight, so
it is always safe to construct.

```cpp
void begin();
void setBrightness(uint8_t percent);   // 0-100; 0 = off. Total light on a warm/cool board
void off();
void on();
void setColorTemperature(uint8_t warmPercent);  // 0 = fully cool .. 100 = fully warm
bool present() const;
bool hasColorTemperature() const;
uint8_t brightness() const;
uint8_t colorTemperature() const;
```

On a warm/cool board `setBrightness()` is the *total* light, and the
colour-temperature split is preserved across brightness changes.

## What we would need to port

Everything below is on `feat-x4-papermono-support`. None of it exists here.

### Build flags

`FREEINK_CAP_FRONTLIGHT` and `FREEINK_CAP_WARMLIGHT` are derived in `BoardConfig.h`
from the device flags — `FREEINK_CAP_WARMLIGHT` is `(FREEINK_DEVICE_X4PRO)` today.
Both compile the feature out entirely when off, so a C3 build pays nothing. Note
this is separate from the runtime check: inside a multi-device build the profile's
`gpioWarm` stays the truth via `hasColorTemperature()`.

### `lib/hal/HalFrontlight.{h,cpp}`

A thin HAL over the SDK manager, exposed as a `Frontlight` singleton macro. Small
enough to port nearly verbatim. The one piece of real logic: the SDK represents
off as brightness 0, so the HAL keeps `lastBrightness` separately in order to
restore the previous level when toggling back on.

```cpp
void begin(uint8_t brightness, uint8_t warmth, bool on);
void setBrightness(uint8_t percent);
void setWarmth(uint8_t warmPercent);
void setOn(bool on);
bool present() const;  bool hasColorTemperature() const;  bool isOn() const;
```

### `src/activities/util/FrontlightPanelActivity.{h,cpp}`

The quick-adjust panel. Needs review against our activity conventions
(see `.skills` on activity surface and scope) rather than a straight copy.

### Settings and persistence — `src/CrossPointSettings.h`, `src/SettingsList.h`

```cpp
uint8_t frontlightBrightness = 60;
uint8_t frontlightWarmth = 50;   // 0 = cool .. 100 = warm
uint8_t frontlightOn = 0;
uint8_t frontlightRestoreOnWake = 0;
```

Brightness and warmth are registered as category-less `SettingInfo::Value`
entries, so they persist and are web-exposed but stay out of the settings menu
(the panel owns the UI). `STR_RESTORE_LIGHT_ON_WAKE` is a normal toggle under
`STR_CAT_DISPLAY`, gated on `#if FREEINK_CAP_FRONTLIGHT`.

### Input and menu wiring

- `src/MappedInputManager.{h,cpp}` — the X4 Pro **delays a single power click**
  until its frontlight double-click window expires, so a double-click can open the
  panel. This changes power-button timing, so it needs care against our own
  power-button handling and `waitForStablePowerRelease()`.
- `src/activities/reader/EpubReaderMenuActivity.h` — a `FRONTLIGHT` menu entry.

### i18n

All four strings are new here — `STR_FRONTLIGHT`, `STR_BRIGHTNESS`, `STR_WARMTH`,
`STR_RESTORE_LIGHT_ON_WAKE` (verified absent from
`lib/I18n/translations/english.yaml`; the `STR_CAT_DISPLAY` category they hang
off already exists). Adding them means regenerating
`lib/I18n/I18nStrings.{h,cpp}` via `scripts/gen_i18n.py` and getting translations
for every language in `lib/I18n/translations/`.

## Order of work

1. Board profile selectable + build flags — until `BoardConfig::ACTIVE.board` can
   be `XteinkX4Pro`, nothing below is testable.
2. `HalFrontlight` + `FrontlightManager` linked; verify on hardware that GPIO8/9
   are the right way round.
3. **Tighten the light-sleep guard** to `present() && isOn()`.
4. Settings persistence, then the panel, then input/menu wiring, then i18n.
