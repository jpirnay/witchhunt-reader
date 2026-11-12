# Witch Reader: agent guide

E-reader firmware (product name "Witch Reader", repo `witchhunt-reader`), a fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). One source tree runs five
boards. The SDK (`freeink-sdk/`, a git submodule) owns the panel drivers, input, board profiles and the UI
toolkit; this repo owns everything above that.

This file is for agents that write, review, design or plan changes here. It states the rules that are
not obvious from the code and points to the docs that hold the detail. Where this file and the code
disagree, the code is right: fix this file in the same change.

Before you propose a change, read the code it touches and cite `file:line`. Do not assume an API exists:
check `lib/`, `src/` or `freeink-sdk/`. State only what code, a test or a device log showed. A guessed
figure or device behaviour in a comment, doc or PR text is taken as fact by the next reader.

## Boards

| Board | Dev env | Release envs | MCU | PSRAM | Panel | Touch | SD |
|---|---|---|---|---|---|---|---|
| Xteink X4 + X3 (one binary, detected at boot) | `default` | `gh_release`, `gh_release_rc`, `slim` | ESP32-C3, single-core RISC-V, no FPU | none | X4 800×480, X3 792×528 | no | SPI, shares the bus with the panel |
| Xteink X4 Pro | `x4pro` | `x4pro_gh_release`, `x4pro_gh_release_rc` | ESP32-S3, dual-core | 8 MB | 800×480 | GT911 | SDMMC |
| LilyGo T5S3 | `lilygo_t5s3` | `lilygo_gh_release`, `lilygo_gh_release_rc` | ESP32-S3 | 8 MB | 960×540, LovyanGFX | yes | SPI |
| Murphy M3 | `murphy` | `murphy_gh_release`, `murphy_gh_release_rc` | ESP32-S3 | 8 MB | 416×240 UC8253 | CHSC6x | SDMMC |

CI builds `default`, `x4pro`, `lilygo_t5s3` and `murphy`. Build all four for any change that is not
plainly board-neutral. Board notes: `docs/contributing/board-support.md`.

**Design for the X3's heap, then use the S3s' PSRAM.** The C3 boards are the binding constraint: about
380 KB of SRAM shared by IRAM and heap, two framebuffers of up to ~52 KB each, and a heap that fragments.
Logic must work there. Do not cap the S3 boards at the C3 budget, though: where PSRAM helps, take it behind
a capability check (see `HalCapabilities::keepsSecondaryBuffer()` for the pattern).

## Where things live

- `src/main.cpp`: boot, the loop task, the globals `renderer`, `mappedInputManager` and
  `activityManager`, the font registry, sleep and power. `display` and `gpio` are the HAL's globals
  (`lib/hal/HalDisplay.cpp`, `lib/hal/HalGPIO.cpp`).
- `src/activities/<area>/`: screens (`home`, `reader`, `settings`, `network`, `browser`, `util`,
  `boot_sleep`, `weather`). `ActivityManager`, `RenderLock`, the list base classes sit in
  `src/activities/`.
- `src/components/`: `UITheme` (the `GUI` macro), themes, layouts, dialogs.
- `src/network/`: web server, WebDAV, OTA, downloads, web pages (`src/network/html/`).
- `lib/hal/`: the HAL (storage, display, GPIO, clock, power, frontlight, SPI and I2C bus locks, flash area,
  `HalCapabilities.h`).
- `lib/Epub/`: EPUB parsing, CSS, layout, section cache. `lib/GfxRenderer/`: drawing. `lib/EpdFont/`:
  built-in, SD-card and TTF fonts. `lib/Memory/`: allocation helpers. `lib/I18n/`: translations.
  `lib/TextShaping`, `lib/OtShaper`, `lib/MiniBidi`: complex scripts and bidi.
- `test/`: host GoogleTest suites, one folder each; `test/shims/` stands in for Arduino and the HAL.
- `docs/`: reference docs. `docs/design/`: design records for shipped features. `docs/future_work/`:
  open work, one file per topic. Read the relevant one before planning in that area.

Singletons: `SETTINGS`, `APP_STATE`, `GUI`, `Storage`, `I18N`, `RECENT_BOOKS`, `READING_STATS`,
`GLOBAL_BOOKMARKS`, `WIFI_STORE`, `OPDS_STORE`, `KOREADER_STORE`, `AUTOSYNC_STATE`, `WEATHER_SETTINGS`.

## Memory

Details: `docs/memory-allocation-strategy.md`, `docs/contributing/heap-and-data-structures.md`. The
`heap-discipline` skill has the decision procedure.

- **The largest free block decides, not free heap.** A 4 KB request fails on a heap with 15 KB free in
  pieces (seen on the X3: `lib/Memory/HeapFit.h`).
- **The build is `-fno-exceptions`.** A failed `new`, `std::vector` growth or `std::string` growth
  calls `abort()` and reboots. Use `makeUniqueNoThrow` (`lib/Memory/Memory.h`) and null-check. Before
  growing a std container by a size that comes from data, ask `HeapFit` (`lib/Memory/HeapFit.h`).
  Tables whose length the document decides use `ChunkedVector`. Section builds allocate from
  `BuildArena`. Lookups that may not fit use `PassTable`.
- `vector::shrink_to_fit()` frees nothing under `-fno-exceptions`. Use `std::vector<T>().swap(v)`.
- `const char* TABLE[]` puts the pointer array in DRAM. Write `const char* const TABLE[]`.
- Data per spine item, per page or per book goes on the SD card in one compact file, read in ranges.
  Never hold a RAM table sized by spine count: books with 1,700+ spine items exist. Avoid one file open
  per spine item, because a FAT open scans the directory linearly.
- Stacks: the loop task has 16 KB (it also runs sliced background section builds) and the render task
  10 KB. The render task's stack abuts the heap, so an overflow looks like heap corruption. Keep large
  buffers off the stack. A function-local `static` with a constructor takes a guard mutex on first use,
  which is a stack and heap cost at boot.
- On the C3, IRAM is carved from the heap. `IRAM_ATTR` costs heap; use it only for ISRs and code that
  runs with the flash cache off. The C3 links a generated IRAM-budget linker script
  (`scripts/iram_budget.py`, `tools/iram-budget/`).
- Flash: each 64 KB of app growth on the C3 costs the font cache one MMU map page. Quote size changes
  from the `RAM:` and `Flash:` lines of `pio run`, compared with a master build in the same worktree.
- The TTF engine (`lib/EpdFont/ttf/`) must not use floating point: `scripts/check_no_float.py` fails the
  build.

## Activities and rendering

Reference: `docs/activity-manager.md`, `docs/background-rendering.md`.

- `loop()` runs on the loop task. `render(RenderLock&&)` runs on the single render task with the
  render mutex held. Any state that `render()` reads and `loop()` writes is changed under a
  `RenderLock`. Do not hold the lock across blocking work.
- `render()` only reads state and draws. It never navigates, calls `finish()` or calls
  `requestUpdateAndWait()`.
- Navigation is queued and runs after `loop()` returns. Default to `replaceActivity()` / `goTo*()`, which
  free the caller first. Push (`startActivityForResult()`) only when a live parent needs the result.
- Do not create FreeRTOS tasks in activities. Reader background work goes through the existing lanes
  (`docs/background-rendering.md`). The one worker task, `KOReaderSyncWorker`, shows the shape when a
  task is unavoidable: module-level, not owned by an activity, jobs passed by value with no activity
  pointer, results polled by sequence number, and it deletes itself when idle.
- A buried activity keeps its resources but gets no `loop()`. An activity that has lent out a shared
  resource (the secondary framebuffer) hands it back in `startActivityForResult()`.
- `Activity.h` documents the hooks (`preventAutoSleep`, `keepAwake`, `handleForcedRefresh`,
  `prepareFramebufferForCapture`, `outputPolarity`, ...). Override only what the screen needs.

### Framebuffers and the panel

Reference: `docs/secondary-buffer-management.md`, `docs/grayscale-aa-rendering.md`,
`docs/contributing/eink-controllers.md`.

- There are two buffers: the write buffer and the secondary (previously displayed) buffer.
  `displayBuffer()` ends with a swap, so afterwards the write buffer holds the frame from two refreshes
  ago. A partial repaint that patches regions and displays again must call
  `renderer.syncWriteBufferFromDisplayed()` first. Sync before reading pixels back as well. Correct
  examples: `BookInfoActivity.cpp`, `FileBrowserActivity.cpp`, `BaseTheme::drawPopup`.
- The secondary buffer is lent to background work (section builds, the next-page pre-render, first-open
  indexing, cover decodes). Before a borrow whose next draw may be an overlay, sync at the call site. Do
  not sync inside `borrowSecondaryBuffer()`, because the reader's pre-rendered page lives there. Prefer
  lending to releasing: a lent block never enters the heap, so returning it cannot fail.
- S3 boards never release the secondary buffer. This is enforced centrally through
  `HalCapabilities::keepsSecondaryBuffer()`. After a release call, ask `hasSecondaryBuffer()`; do not
  rely on the return value.
- On SPI-SD boards the panel and the SD card share one bus. Display entry points take
  `HalSpiBus::Lock`. Storage takes its own lock, with the SPI lock outermost.
- Never assume 800×480 or a fixed orientation. Use `renderer.getScreenWidth()`/`getScreenHeight()` and
  `getOrientedViewableTRBL()`. Four orientations exist.

## HAL, boards and input

The `hal-and-abstractions` skill has the rules and a checklist. In short:

- SD access goes through `Storage` / `HalFile`. `FsFile` is an alias of `HalFile` outside the HAL. Never
  use SdFat, `SDCardManager`, `EInkDisplay` or `InputManager` directly outside `lib/hal`. A `HalFile`
  closes in its destructor. `close()` on a handle that was never opened asserts.
- To replace a file, use `FileReplace::replace()` (`lib/FsHelpers/FileReplace.h`). It never leaves the
  card with neither copy. When firmware code moves or renames a book, call `BookRelocation`
  (`src/BookRelocation.h`), or the book loses its cache, position and bookmarks.
- Board differences: ask `HalCapabilities` (`lib/hal/HalCapabilities.h`), the `GfxRenderer`/`HalDisplay`
  panel predicates, or `BoardConfig`. Never add a `deviceIsX3()` call or a board-name `#ifdef` in feature
  code. A setting that only some boards can use declares `.requiring(SettingRequires::X)`. Touch code is
  compiled out on the C3 (`CP_TOUCH_UI=0`, `src/TouchUi.h`).
- Input: `MappedInputManager::Button` (`Back, Confirm, Left, Right, Up, Down, Power, PageBack,
  PageForward`). Never use raw `HalGPIO::BTN_*` indices outside the remap screen. Users remap buttons
  and bind global actions; route through `onButtonAction()`/`takesButtonAction()`.
- Rendering goes through `GUI` (UITheme) and its tokens. Do not hardcode fonts, metrics or colours.
  Dark Mode inverts output per screen (`outputPolarity()`).
- User-facing text uses `tr(STR_*)`. Log lines do not.

## Code conventions

- C++20 (`-std=gnu++2a`), no exceptions, no RTTI. `#pragma once`. Classes `PascalCase`, functions and
  variables `camelCase`, constants `UPPER_SNAKE_CASE` or `kName`. File names match the class.
- Log with `LOG_ERR`/`LOG_INF`/`LOG_DBG`/`LOG_TRC("TAG", fmt, ...)` from `Logging.h`, not `Serial`.
  Log before returning an error. No `abort()` or exceptions for recoverable errors.
- Model closed sets as `enum class` and dispatch with `switch` (`control-flow-clarity` skill).
- `std::string` is fine on cold paths. In per-glyph, per-line and per-pixel loops use stack buffers,
  `string_view` and `snprintf`. A `string_view` is not NUL-terminated: copy it before passing it to a C
  API.
- `std::function` is used for activity results and UI callbacks. Keep it out of per-glyph and per-pixel
  code.
- Read multi-byte values from byte buffers with `memcpy`. The C3 faults on unaligned loads.
- ISRs are `IRAM_ATTR` and touch only `DRAM_ATTR` data. Signal tasks with the `FromISR` primitives.
- Before adding logic to one screen or page, look for its siblings (File Transfer and Calibre, the web
  pages' `shared/*.inc`) and share one helper. When you fix a bug in one copy, check the others.
- Comments say why: an invariant, a past incident, a constraint. They do not narrate the next line.
- Ported code carries a provenance line naming the upstream project, PR and author: in the file header
  for a whole file (`lib/FsHelpers/FileReplace.h`), at the member for a part
  (`src/activities/Activity.h` at `handleForcedRefresh`).
- Fix the cause, not the tripwire. When a HAL assert fires, fix the caller and teach the host shims the
  same contract (`abort()`, because host tests build with NDEBUG). Never relax the assert.

## Caches and on-card files

Formats: `docs/file-formats.md`, `docs/sidecar-files.md`.

- The firmware's own files live under `/.crosspoint/` on the card: `settings.json`, `state.json`,
  `recent.json`, the credential stores, `fonts/`, `lang/`, `hyphenation/`, `library/`, and one cache folder
  per book: `epub_<std::hash of the path>/` (also `txt_`, `xtc_`). That folder holds progress, bookmarks,
  `book.bin` and the laid-out chapters in `spines/<bucket>/` (the flat `sections/` layout is legacy and
  is only deleted). Screenshots go to `/screenshots/`.
- Nothing mounts SPIFFS. The `spiffs` partition is a raw flash area (`FlashArea`) for the font cache
  and the language slot; S3 boards without a usable one keep a card-backed image instead
  (`docs/design/flash-area.md`).
- A section file's header records every input that changes layout (font, sizes, spacing, margins, indent,
  viewport, ...). A new layout input goes into that header. Any change to a cache's binary layout bumps
  its version constant (`SECTION_FILE_VERSION` in `Section.cpp`, `BOOK_CACHE_VERSION` in
  `BookMetadataCache.cpp`, and the others).
- Version numbers collide between parallel branches. Fetch master right before you resolve a conflict
  or merge: master's numbers stand, and yours move above them. Two layouts that share a version load as
  valid and render wrong.
- Settings rows are declared once in `src/SettingsList.h`, which serves the device UI, the web API and
  the serial `GET`/`SET`. `JsonSettingsIO` saves each keyed row in `settings.json`, enums as numbers,
  and loads the row's default when the key is missing. Append new enum values; never renumber.

## Generated files

Never edit these by hand; edit the source. The i18n and web outputs are gitignored; `src/fontIds.h`,
`lib/SecureNet/include/CrossPointRoots.h` and `plugins/catalog.json` are generated but committed.

- **i18n**: `lib/I18n/translations/<language>.yaml` (`english.yaml` is the reference; missing keys fall
  back to English) → `I18nKeys.h`, `I18nStrings.*`, regenerated by `scripts/gen_i18n.py` as a pre-build
  step. The build strips keys that no code references and skips regeneration when the outputs are newer
  than the YAML. If you add a key in one build and use it in a later one, the compile fails with
  `'STR_X' is not a member of 'StrId'`. Add the key with its first use, or `touch` the YAML. Only
  English is in the app image; every other language is a card pack
  (`assets/sd-fonts/Language-<Name>/`), rebuilt with `gen_i18n.py --packs`. Details: `docs/i18n.md`.
- **Web pages**: `src/network/html/*.html` plus `shared/*.inc` (theme, header, nav) → `*.generated.gz`
  and `*.generated.h`, written by `scripts/build_html.py`. Only the setup page is in flash; the rest
  ship as an SD bundle `web-<hash>.bin`. Build pages from the shared components. Preview without a
  firmware build: `python3 scripts/web_preview.py --device http://<ip>` or `--mock <dir>`. Never write a
  tag name such as `<style>` inside a comment, because the minifier takes it for a real tag. Script
  tests: `node test/web/<name>_test.mjs <deps dir>` (see the `web-tests` CI job).
- **Fonts**: IDs in `src/fontIds.h` are 32-bit hashes. Store them as `int`/`int32_t`; a truncated ID
  draws nothing and raises no error.
- TLS roots: `scripts/gen_roots.py` → `CrossPointRoots.h`. Plugin catalog:
  `scripts/build_plugin_catalog.py` → `plugins/catalog.json`; CI runs it with `--check`, so commit the
  regenerated file with the plugin change.

## Build, test, check

`pio` may not be on `PATH`; then use `~/.platformio/penv/bin/pio`.

```bash
pio run -e default                    # also x4pro, lilygo_t5s3, murphy
cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/test && ctest --test-dir build/test --output-on-failure -j4
bin/clang-format-fix -g               # needs clang-format 21+; formats modified tracked files only
```

- **Tests are host GoogleTest under CMake, never `pio test`.** Build all targets, not only the one you
  changed: a shared header that starts using a new `.cpp` breaks other targets' links. Read
  "100% tests passed" before committing; a pipe into `grep` hides the exit status. Run ctest with the
  default `TMPDIR`, because a long temp path fails the SD-font suites. New tests use a per-process temp
  dir. Re-run a failure with `-j1` before you believe it, because a few suites share fixed `/tmp` paths.
- **Time in host tests is frozen.** `millis()` advances only inside a `host_clock::Ticking` scope. A test
  of sliced or time-budgeted code that does not open one compares the blocking path with itself; assert
  the step count is > 1.
- **Formatting**: CI checks clang-format 21. `-g` skips new untracked files: format those with
  `clang-format-21 -style=file -i <file>`. The pre-commit hook (`core.hooksPath=.githooks`) formats and
  then re-stages each staged path whole, so a partial `git add -p` does not survive. To split a commit,
  make the working tree match the commit.
- **cppcheck**: CI runs `pio check` (cppcheck 2.20) and fails on any defect in `src/` and in the `lib/`
  headers that `src/` includes. A file that "checks" in about 0.1 s was skipped, not passed. A local
  `pio check` can end up running a different cppcheck than CI's 2.20, with different findings; print
  `cppcheck --version` first. Suppress a finding only inline: `// cppcheck-suppress <id> ; <reason>`.
  Classes in headers need member initializers (`noConstructor`).
- **Device checks**: dev envs build the serial test API (`docs/serial-test-api.md`, driver
  `tools/devctl.py`), which drives buttons and screens and reads `STATE` and heap over USB. Release envs
  keep only the basic commands. Say plainly what ran on which device and what did not.

## Git and pull requests

- Remotes: `origin` is this fork (`jpirnay/witchhunt-reader`, default branch `master`). Push only to
  `origin`. `upstream` is CrossPoint (branch `develop`). To credit ported work, search
  `upstream/develop` history under each of the feature's names (`git log --grep ... -i`).
- Branch from `origin/master` in a git worktree. The main checkout stays on `master`. Prefixes: `feat/`,
  `fix/`, `perf/`, `refactor/`, `docs/`, `chore/`, `test/`.
- Commit subjects: `type(scope): summary`, lowercase, imperative (`fix(reader): ...`,
  `chore(sdk): pin ...`). One concern per commit (`refactor-for-review` skill). No AI co-author trailers.
  Ported work credits its human authors with `Co-authored-by:`.
- Ask before opening a PR. Keep the body short (about 2–4 KB): what, why, how verified, cost, open
  points. Run `gh pr checks` after every push.
- Do not stack PRs; GitHub merges a stacked PR into its base branch, not into `master`. Open the second
  PR against `master` after the first merges. Before pushing to an existing PR branch, check that the PR
  is still open (`gh pr view N --json state`).
- The SDK submodule pins commits from the fork's long-lived `witchhunt` branch
  (`.gitmodules` → `jpirnay/freeink-sdk`). Land SDK commits there before pinning them; CI's `sdk-pin`
  job checks this (`scripts/check_sdk_pin.sh`). Resyncs merge upstream into `witchhunt` and never rebase.

## Scope

A dedicated reader, not a Swiss Army knife. `SCOPE.md` is the test. It is inherited from CrossPoint,
and this fork already ships things it calls unsupported (a clock on boards with an RTC) or does not
list (weather, Dark Mode, SD-card plugins). For anything at the edge, ask the user rather than infer.
The `scope-discipline` skill has the gate.
