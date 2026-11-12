---
name: hal-and-abstractions
description: Layering and abstraction discipline for the firmware. Use when touching storage, files on the SD card, input, buttons, touch, display, the framebuffer, settings, i18n, rendering, or anything board-specific, or any code that could reach into freeink-sdk. Covers Storage/HalFile instead of SdFat, FileReplace and BookRelocation, HalCapabilities instead of board checks, the SPI bus lock, MappedInputManager logical buttons, GUI/UITheme for all drawing, tr() for user-facing text, the settings list, and where a new abstraction boundary belongs.
---

# HAL and Abstractions

Five boards share this code (main guide, "Boards"). The layers below exist so that a feature is written
once and the board differences stay in one place. Bypassing them is a correctness problem, not a style
issue: the HAL's locks are what keep the panel and the SD card from corrupting each other.

## Route through the layer

- **SD card:** `Storage` (`HalStorage`) and `HalFile`. Outside `lib/hal`, `FsFile` is an alias of
  `HalFile`. Never use `SdFat`, `SdSpiCard`, `FsBaseFile` or `SDCardManager` directly. Every SD access
  runs under the storage lock. On SPI-SD boards (`HalCapabilities::sdUsesSpi()`) that lock also takes
  `HalSpiBus::Lock`, because the panel shares the bus. An unlocked SD transfer during a refresh corrupts SdFat's SPI state
  and looks like heap corruption.
- **Files:** a `HalFile` closes in its destructor. Call `close()` only to release early or to check the
  result, and never on a handle that was never opened (it asserts). Size reads by a cap
  (`readFileToString(module, path, cap, out)`), not with `readFile()`, when the file comes from the user.
- **Replacing a file:** `FileReplace::replace()` (`lib/FsHelpers/FileReplace.h`). It keeps the old file
  until the new one is in place. Do not remove and then rename.
- **Moving or renaming a book:** after the rename succeeds, call `BookRelocation`
  (`src/BookRelocation.h`). The cache folder, position, bookmarks, recent entry and `state.json` are all
  keyed by path.
- **Display:** `HalDisplay` (global `display`) through `GfxRenderer` (global `renderer`). Never use
  `EInkDisplay` or `FreeInkDisplay` directly. Display entry points take `HalSpiBus::Lock`; a new one must
  too.
- **Input:** `HalGPIO` (global `gpio`) through `MappedInputManager`. Activities use
  `MappedInputManager::Button`, never raw `HalGPIO::BTN_*` indices outside the remap screen. Logical
  buttons survive remapping and orientation; raw indices do not. Global button actions arrive through
  `onButtonAction()`.
- **Touch:** compiled out on the C3 (`CP_TOUCH_UI=0`). Wrap touch-only code the way existing screens
  do (`src/TouchUi.h`), and keep list taps on `selectListRow()` / `ListRowTap` so a tap runs the same
  handler as Confirm. Reference: `docs/contributing/touch-architecture.md`.
- **Drawing:** everything through `GUI` (UITheme) and its tokens and themes, with the renderer's
  oriented metrics (`getScreenWidth()`, `getScreenHeight()`, `getOrientedViewableTRBL()`). No 800/480
  literals, hardcoded font IDs, colours or coordinates.
- **Shared state:** the singleton macros (`SETTINGS`, `APP_STATE`, `GUI`, `Storage`, `I18N`,
  `RECENT_BOOKS`, ...), not threaded pointers.

## Board differences

Ask for a capability, never for a board:

- `HalCapabilities` (`lib/hal/HalCapabilities.h`): `hasTouch()`, `hasFrontlight()`,
  `hasHardwareRtc()`, `sdUsesSpi()`, `keepsSecondaryBuffer()`, `panelShowsGray()`, and so on.
- The `GfxRenderer` / `HalDisplay` panel predicates for refresh behaviour.
- `BoardConfig::ACTIVE` (SDK) for raw profile data.

Never add a `deviceIsX3()` call, a `FREEINK_DEVICE_*` `#ifdef` or a board-name check in feature code.
When no capability fits, add one to `HalCapabilities` (or to the SDK board profile) and name it after
what the hardware can do. A new board then gets the behaviour from its profile.

## Settings

A setting is one row in `src/SettingsList.h`. That list feeds the device settings screen, the web
settings API and the serial `GET`/`SET` (`SettingsByKey`). A row that only some boards can use declares
`.requiring(SettingRequires::X)` with a capability. Do not build a second list for the web page.

## User-facing text

Every string a user reads goes through `tr(STR_*)`. Add the key to `lib/I18n/translations/english.yaml`
in the same change as its first use (the generator strips unreferenced keys). Log lines stay
hardcoded. Text drawn in a UI face must be in that face's character set. `docs/i18n.md` covers the
UI scripts that need care (Hebrew, Arabic, Persian, CJK); `docs/text-shaping.md` covers shaping on the
page (Indic, Arabic).

## Drawing a new boundary

When you need an SDK capability the HAL does not expose, **add the method to the HAL.** Do not reach
around it. The new method inherits the lock, logging and error contract the rest of the HAL carries.

Keep the layer thin. A wrapper that only renames an SDK call, without adding a lock, logging, an error
contract or a capability decision, is dead weight.

The SDK is a separate repository (`freeink-sdk/`, pinned from the fork's `witchhunt` branch). A change
there is its own commit in the SDK, landed on `witchhunt` before the firmware pins it.

## Self-review

- [ ] No direct SdFat, `SDCardManager`, `EInkDisplay`/`FreeInkDisplay` or `InputManager` use outside
      `lib/hal`.
- [ ] No `close()` on a never-opened `HalFile`; replacements use `FileReplace`; book moves call
      `BookRelocation`.
- [ ] Any new display or bus entry point takes `HalSpiBus::Lock`.
- [ ] Board behaviour is chosen by a `HalCapabilities`/panel predicate, not a board name or
      `deviceIsX3()`.
- [ ] Input uses `MappedInputManager::Button`; touch code is gated for the C3.
- [ ] Rendering goes through `GUI` and oriented metrics; no fixed screen size.
- [ ] User-facing strings use `tr(STR_*)`, and the key is added with its first use.
- [ ] A new setting is a `SettingsList.h` row with a capability requirement where needed.
- [ ] Any new SDK capability is a HAL method, not an inline SDK call.
