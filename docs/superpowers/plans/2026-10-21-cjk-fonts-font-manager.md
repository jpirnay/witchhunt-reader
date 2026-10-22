# The CJK families as `.otf` downloads in the font manager — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Chinese, Japanese or Korean reader installs their language's font, sans or serif, from the device's Font Manager or the web page in one step. The eight Noto CJK subsets ship as desubroutinized CFF `.otf` files, listed in a second manifest older firmware never reads. The three CJK bitmap families are gone.

**Architecture:**
- **The build.** `cjk_subset.py`, driven by `build-sd-fonts.py` from `sd-fonts.yaml`, cuts eight Regular `.otf` files from the pinned Noto CJK region fonts. They are committed under `assets/sd-fonts/<Family>/` and served from master through raw.githubusercontent.com, like the `.cpfont` families.
- **The manifests.** `generate-font-manifest.py --config` writes two from one scan: `fonts.json` (version 2, `.cpfont` only, what older firmware reads) and `fonts-v3.json` (every family with its `format`). The new firmware's `FONT_MANIFEST_URL` names the second.
- **The firmware.** `FontDownloadRules`, header-only and host-tested, decides a file's kind and first bytes, room on the card, what "Download all" takes and what counts as an update. `FontManifestParser`, `FontManifestReader` and `FontCatalog` carry the format and the Download-all flag; `FontInstaller`, the Font Manager and the web API use them.

**Spec:** `docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md` (Task 9 moves it to `docs/design/cjk-ttf-fonts.md`), sections 4, 5, 6, 10 and 11; step 5 of section 13.

**Prototype:** `proto/cjk-ttf-fonts` (worktree `C:/tmp/wt-cjkfonts-proto`; read it, never modify it), built for TrueType `.ttf` on master 2d09575f3. The manifest parser, reader, catalog and installer have not changed on master since, so its diffs to them still apply. Each task says what to take; where this plan and the prototype differ, the plan is right. Reviewed against master 5ae036f79 on 2026-10-22.

## Maintainer decisions (2026-10-22)

1. `README.md` keeps "Support for CJK" under "What this reader doesn't" until roadmap step 6 lands (book titles and the TOC show CJK). It changes once, then. This step rewrites only the USER_GUIDE limitation line.
2. The CJK fonts spec moves to `docs/design/cjk-ttf-fonts.md` when step 5 finishes (Task 9, Step 4).

## Global Constraints

**Base and branch.**
- The work runs on `feat/cjk-font-downloads` in `C:/tmp/wt-fonts`, which holds the spec and this plan on top of master 5ae036f79.
- Before Task 1, merge `origin/master` into it once synthetic bold (roadmap step 4) has merged there. Master has also moved on by #546 and #547; #547 removed `FontManifestParserTest.ParsesTheShippedManifest` and `ttf::probeStyle`, which nothing here uses.
- **The fonts are committed once** (spec 4.1: "The sets are final before the first commit"). The eight `.otf` files go into Task 4's commit only. If a set or the build changes after it, redo that commit (reset to before it and commit again) rather than adding a second copy of the files, before Task 10 Step 1 first pushes the branch.

**The families** (spec 4.1–4.2): one file each, `assets/sd-fonts/<Family>/<Family>-Regular.otf`.

| Family | `charset` | `fontName` | `description` (my call: at most 32 characters, a whole X4 row) |
|---|---|---|---|
| NotoSansJP, NotoSerifJP | `ja` | "Noto Sans JP", "Noto Serif JP" | "Japanese, sans serif", "Japanese, serif" |
| NotoSansSC, NotoSerifSC | `zh-Hans` | "Noto Sans SC", "Noto Serif SC" | "Simplified Chinese, sans serif", "Simplified Chinese, serif" |
| NotoSansTC, NotoSerifTC | `zh-Hant` | "Noto Sans TC", "Noto Serif TC" | "Traditional Chinese, sans serif", "Traditional Chinese, serif" |
| NotoSansKR, NotoSerifKR | `ko` | "Noto Sans KR", "Noto Serif KR" | "Korean, sans serif", "Korean, serif" |

- **Sources:** releases Sans2.004 and Serif2.003, each file pinned by its sha256; Unihan 17.0.0 is `https://www.unicode.org/Public/17.0.0/ucd/Unihan.zip`, sha256 `f7a48b2b545acfaa77b2d607ae28747404ce02baefee16396c5d2d7a8ef34b5e`. The URLs and hashes are the prototype's `sd-fonts.yaml`; all nine hashes match the copies in `C:/tmp/cjkfonts-src` (checked 2026-10-22).
- **Character sets, the ranges every family carries, and the gaps** left out by an explicit list are spec 4.1's. The prototype's `ttf_subset.py` implements them. Any other code point a source lacks fails the build.
- **U+FFFD** comes from `lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf` (OFL, 1000-unit em), its quadratic outline written as a CFF charstring.
- **Outlines, tables, names and licence** (spec 4.2–4.3) are Task 1's interfaces and checks and Task 4's `OFL.txt` and credits. A rebuild with unchanged inputs writes identical bytes.

**Manifests and installing** (spec 6.2–6.4) are Tasks 3 and 5–8. Both manifests' `baseUrl` stays `https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/`; `FONT_MANIFEST_URL` names `fonts-v3.json` there. "Installed" is "the registry lists it, or its folder is there".

**Not in this round** (spec 6.5): uploading `.ttf`/`.otf` through the web page, a screen that goes back by itself, and the `txtSdFontFamilyName` setting. Task 9 records all three in future work.

**Unchanged by this work:** `SECTION_FILE_VERSION`, `glyphstore::kFormatVersion`, `ttf::kRenderVersion`. Floating point must not reach the font engine (`scripts/check_no_float.py`).

**Project rules.**
- Comments say what and why, never history. No `Co-Authored-By` trailer. PR titles carry a conventional prefix (`feat:`, `docs:` …).
- User-facing device text goes through `tr(STR_*)` and `lib/I18n/translations/*.yaml`, regenerated by the build. Never hand-run `gen_i18n.py`. The web page's text stays English.
- SD access goes through `Storage` / `HalFile`.

**Commands.**
- **Host tests, from Git Bash.** Configure once:

  ```
  cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1 \
    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=C:/_development/witchhunt-reader/build/test/_deps/googletest-src \
    -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=C:/tmp/wt-serial-api/build/test/_deps/arduinojson-src
  ```

  Then `cmake --build build/test --target <T> -j 8` and `./build/test/<dir>/<T>.exe --gtest_filter=...`. `PngDecoderArenaTest`, `BookMetadataCacheTest` and `EpubPipelineTest` never build on Windows.
- **Python:** `C:/Python314/python` (fontTools 4.63.0, PyYAML 6). Each test file runs on its own (`python <file>`), as `test_kern_extract.py` does. CI does not run them; the C++ tests that read the shipped files do.
- **Web tests:** once, `npm install --prefix C:/tmp/web-deps --no-save --no-audit --no-fund jsdom@24.1.3 jszip@3.10.1`; then `node test/web/<name>_test.mjs C:/tmp/web-deps`, as CI's `web-tests` job runs them.
- **Firmware:** `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`, in the background, one `pio run` at a time. `x4pro` and `lilygo_t5s3` are left to CI.
- **Format:** clang-format 21: `PATH="/c/tmp/cf21/bin:$PATH" bash bin/clang-format-fix`, then `git status`.
- **Devices:** the X4 is driven over the serial test API (`tools/devctl.py --port <X4 port>`, `docs/serial-test-api.md`) and runs German. The X3 is flashed by the maintainer only: never RTS or esptool, and no `CMD:REBOOT`. `CMD:GET` every setting before changing it, and restore it afterwards.

## Review Focus

1. **Every Latin family installed, no CJK one: where most readers will be after the update.** Expected: no "Download All" row (not "Download All (0 B)"); the eight CJK rows offer Download with their sizes; the web page's `{"all": true}` installs none of them but still updates an installed one. Pinned by `FontDownloadRulesTest.DownloadAllTakesOnlyMissingFamiliesThatJoinIt` (Task 6), which both sides call, and Task 10 Step 3's screenshot.
2. **A folder of the same name that the reader filled.** Google Fonts' and notofonts' downloads are folders named `NotoSansJP` and so on, a family on the card since #506. Today's rule (any manifest file missing → update) would replace it, and a reader's own font, maybe with a real Bold, without a question. Expected: a folder holding none of the manifest's files is installed, offered for Delete, never an update. Pinned by `FontDownloadRulesTest.AFolderHoldingNoneOfTheManifestsFilesIsNotAnUpdate` (Task 5), used by both update scans, and Task 10 Step 8. Not covered: a file of the manifest's own name with other bytes, as notofonts' SubsetOTF `NotoSansJP-Regular.otf` would be, still reads as an update.
3. **The largest family end to end, on the slowest board.** NotoSerifKR is one 7.33 MB file: a TLS download in 1–6 KB Range chunks (`HttpDownloader` on a tight heap; the prototype's 23 s for 4.3 MB was plain HTTP from a PC), a CRC pass in 128-byte reads, an SD-mode block within the X3's font area and 32 mapping pages. Expected: it installs without a watchdog reset, is listed, and draws. Pinned by `ShippedOtfFamilies.EachInstallsWithinTheX3StoreAndDraws` (Task 4) and Task 10 Steps 4 and 9. If the CRC pass resets a board, `FontInstaller::computeFileCrc32` gets `HalSystem::feedWatchdog()` and `yield()` every 64 KB.
4. **Manifest keys in any order, and of the wrong type.** A `format` may follow `files` or come twice; `inDownloadAll` may be `"false"` or `0`. Expected: the last `format` given judges the family when it closes; a non-boolean `inDownloadAll` is ignored and the family stays in Download all. Pinned by `FontManifestReaderTest.TheFormatMayFollowTheFilesAndTheLastOneWins` and `FontManifestParserTest.AnInDownloadAllThatIsNotABooleanIsIgnored` (Task 6).
5. **A family on the card that the registry does not list** (the store cannot hold it, or the file is broken). Expected: it shows as installed, never as a fresh Download, and its install says so once: a note under "Font installed!" on the device, a sentence on the web page. Pinned by `fonts_page_test.mjs`'s "an install the reader does not list says so" (Task 8) and Task 10 Step 7 (a broken family served from the PC).

---

### Task 1: The CJK subset build (`cjk_subset.py`)

**Files:** Create `lib/EpdFont/scripts/cjk_subset.py` and `lib/EpdFont/scripts/test_cjk_subset.py`.

**Interfaces:**
- Consumes the fixture `test/fixtures/fonts/NotoSansJP-Cff.otf` (its facts checked 2026-10-22):
  - CID-keyed (`CIDCount` 65535, highest CID 47443), 5 font DICTs, 45 global and 342 local subroutines, and the tables `BASE`, `GSUB`, `VORG`, `vhea` and `vmtx` beside the kept ones;
  - 287 code points: ASCII, `、。「」・ー`, U+3041–3093, U+30A1–30F3 and `日本語文字書読鬱慶議驚龍鑑響漢点線曲円角`; U+0041 is drawn from the FD `NotoSansCJKjp-Regular-Proportional`;
  - the names "Noto Sans CJK JP" and "NotoSansCJKjp-Regular", in name IDs 0–6 only.
- Produces (module `cjk_subset`):

  ```python
  CHARSETS = ("ja", "zh-Hans", "zh-Hant", "ko")
  BASE: set[int]                      # the ranges every family carries (spec 4.1)
  BASE_GAPS: set[int]                 # 39 code points
  CHARSET_GAPS: dict[str, set[int]]   # ja 1, zh-Hans 0, zh-Hant 4, ko 34
  REPLACEMENT = 0xFFFD
  REPLACEMENT_DONOR: Path             # lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf
  DROPPED_TABLES = ["GSUB", "BASE", "vhea", "vmtx", "VORG", "DSIG"]
  KEPT_TABLES = frozenset({"CFF ", "GPOS", "OS/2", "cmap", "head", "hhea", "hmtx", "maxp", "name", "post"})
  MAX_POINTS = 512                    # the engine's CFF scratch caps (spec 9.2)
  MAX_CONTOURS = 256
  OVERSIZE: dict[str, frozenset[int]] = {"NotoSerifTC": frozenset({0x9F98})}  # 龘, 621 points
  class BuildError(Exception): ...
  def unihan_field(unihan_zip: Path, field: str) -> set[int]
  def charset(name: str, unihan_zip: Path) -> set[int]
  def family_set(name: str, unihan_zip: Path) -> set[int]     # (charset | BASE) - BASE_GAPS - CHARSET_GAPS[name]
  def check_fonttools(pinned: str) -> None                    # BuildError naming both versions unless equal
  def subset_cjk(source: Path, codepoints: set[int], font_name: str, out: Path, *,
                 donor: Path = REPLACEMENT_DONOR, oversize: frozenset[int] = frozenset()) -> dict
  def check(path: Path, codepoints: set[int], oversize: frozenset[int] = frozenset()) -> None
  def build_family(family: dict, output_dir: Path, source: Path, unihan_zip: Path) -> dict
      # family: an sd-fonts.yaml entry (name, charset, fontName); writes <output_dir>/<name>-Regular.otf
  ```

  - **The report** that `subset_cjk` and `build_family` return: `characters` (`len(codepoints)`), `cmap` (the output's mapped code points other than U+FFFD), `glyphs`, `bytes`, `dropped` (tag → its bytes in the source), `kernPairs` (the pair-index slots the kept kerning uses), `maxPoints`.
  - **The CLI:** `python cjk_subset.py <source.otf> <Unihan.zip> <charset> "<Font Name>" <out.otf>`.

**From the prototype** (`lib/EpdFont/scripts/ttf_subset.py`, `test_ttf_subset.py`): verbatim `BASE`, `BASE_GAPS`, `CHARSET_GAPS`, `_encodable`, `_double_byte`, `unihan_field`, `charset`, `family_set`, `_kern_pairs`, the CLI and the `CharacterSets` test class (`ttf_subset` renamed `cjk_subset`); `_rename`, extended to the CFF's names; `_add_replacement_glyph`, rewritten for a CID-keyed CFF. Not `_cff_to_truetype`, nor `check`'s TrueType rules.

- [ ] **Step 1: Write the failing tests.**
  - **`CharacterSets`**, the prototype's class, plus:
    - `test_the_gap_lists_are_the_measured_ones`: `len(BASE_GAPS) == 39`; `CHARSET_GAPS` is `{0x2252}` for `ja`, `{0x02CD, 0x203E, 0x223C, 0x2252}` for `zh-Hant`, 34 code points for `ko` and empty for `zh-Hans`.
    - `test_the_real_sets_have_their_measured_sizes`, `skipUnless` the pinned Unihan is cached at `lib/EpdFont/scripts/downloaded_fonts/Unihan/Unihan-17.0.0.zip`: the `family_set` sizes are `{"ja": 7367, "zh-Hans": 9214, "zh-Hant": 14052, "ko": 17304}`.
  - **`Build`.** `setUp` copies the fixture to a temp folder as `source.otf` and gives the copy a name ID 13 ("This Font Software is licensed under the SIL Open Font License, Version 1.1."), which the fixture lacks and the real sources have. `CODEPOINTS` is the fixture's cmap. `self.build(name="out.otf", codepoints=CODEPOINTS)` returns `(out, report)` from `subset_cjk(source, codepoints, "Test Sans JP", out)`.
    - `test_the_subset_is_cff_without_subroutines`: the file starts with `OTTO`; `cff.major == 1`; `"ROS" in top.rawDict` (still CID-keyed); no `GlobalSubrs`; no FD has `Private.Subrs`.
    - `test_only_kern_is_kept_and_the_listed_tables_are_dropped`: the tables other than `GlyphOrder` are a subset of `KEPT_TABLES`; the GPOS features are `{"kern"}`; `report["dropped"]` has exactly the keys `{"GSUB", "BASE", "vhea", "vmtx", "VORG"}`, each over 0.
    - `test_no_name_claims_to_be_noto_cjk`: name IDs 1, 2, 4 and 6 are "Test Sans JP", "Regular", "Test Sans JP" and "TestSansJP-Regular"; ID 3 contains "TestSansJP-Regular"; no ID 16, 17, 21 or 22; ID 13 survives (the OFL notice travels with the file). CFF `fontNames == ["TestSansJP-Regular"]`, the Top DICT's `FamilyName` is "Test Sans JP" and `FullName` "Test Sans JP Regular", and every FD `FontName` starts with "TestSansJP-Regular-". None of these names contains "CJK".
    - `test_the_replacement_glyph_is_noto_sans_as_a_cff_charstring`: every Unicode cmap subtable maps U+FFFD; its glyph draws 3 contours (`moveTo`s on a `RecordingPen`); `hmtx` is `(1000, lsb)`, `lsb` within 1 unit of the donor's 41; its `ControlBoundsPen` bounds are each within 1 unit of the donor's `(41, -156, 958, 760)`; its FD's `FontName` is that of the glyph mapped to U+0041 (`top.FDSelect[font.getGlyphID(name)]`); `report["cmap"] == len(CODEPOINTS)`.
    - `test_timestamps_are_the_sources_and_a_rebuild_writes_the_same_bytes`: `head.created` and `head.modified` equal the source's, and two builds to two paths are byte-identical.
    - `test_a_code_point_the_source_lacks_fails_the_build`: adding U+732B (猫) raises `BuildError` matching `U\+732B`.
    - `test_the_check_refuses_what_the_engine_does_not_read`, each a `BuildError` matching the quoted word: the fixture itself, "subroutines"; `test/fixtures/fonts/NotoSansJP-TrueType.ttf`, "OTTO"; a built font saved again with `head.macStyle = 1`, or with `OS/2.usWeightClass = 600`, "regular"; `check(out, CODEPOINTS | {0x732B})`, "lacks 1".
    - `test_a_glyph_over_the_engine_caps_fails_unless_listed`: a name-keyed CFF from `FontBuilder(1000, isTTF=False)` holds `.notdef` and a glyph `big`, mapped to U+E000, of one contour of 171 `curveTo` segments (514 points). `check(path, {0xE000})` raises matching "points"; with `oversize=frozenset({0xE000})` it passes.
    - `test_another_fonttools_version_is_refused`: `check_fonttools("0.0.0")` raises naming both versions; `check_fonttools(fontTools.version)` returns `None`.

- [ ] **Step 2: Run them to see them fail.** Run: `python lib/EpdFont/scripts/test_cjk_subset.py`. Expected: `ModuleNotFoundError: No module named 'cjk_subset'`.

- [ ] **Step 3: Implement `cjk_subset.py`.**
  - **`subset_cjk`**, in this order: load the source with `recalcTimestamp=False`; fail on any code point of the set it does not map, naming the first 20 as `U+XXXX`; record `dropped` from `font.reader.tables[tag].length`; subset with `subset.Options()` set to `hinting=False`, `desubroutinize=True`, `layout_features=["kern"]`, `drop_tables += DROPPED_TABLES`, `name_IDs=list(range(15))` (IDs 13 and 14 carry the OFL notice), `name_legacy=False`, `glyph_names=False`, `notdef_outline=True`, `recalc_timestamp=False`, then delete any of `DROPPED_TABLES` still present; rename; add U+FFFD; save with `recalcTimestamp=False`; `check(out, codepoints | {REPLACEMENT}, oversize)`.
  - **Names** (my call: the CFF's names too, because "no subset claims to be the full Noto Sans CJK"). Name IDs 1 and 4 become `font_name`, ID 2 "Regular", ID 6 `font_name` without spaces + "-Regular"; in ID 3 the source's PostScript name (its ID 6) is replaced by the new one. CFF `fontNames[0]` becomes the new PostScript name, the Top DICT's `FamilyName` `font_name` and `FullName` `font_name + " Regular"`; each FD's `FontName` keeps its suffix behind the new PostScript name.
  - **U+FFFD in a CID-keyed CFF**, the part the tests do not determine:
    - fail when the donor's em is not 1000 or its U+FFFD glyph is composite;
    - the FD is that of the glyph mapped to U+0041 (my call: the Latin proportional FD, so its FontMatrix and widths are the Latin glyphs');
    - draw the donor glyph into `T2CharStringPen(width, None)`: `BasePen` turns each quadratic into its exact cubic. My call: the pen rounds to whole units, as every Noto charstring is integer; the error is under 1/2000 em. `width` is `1000 - Private.nominalWidthX` of that FD, or `None` when 1000 is its `defaultWidthX`;
    - name it `cid<N:05d>`, N the subset's highest CID + 1, and fail unless N is below the Top DICT's `CIDCount`. Add it to the glyph order, `topDict.charset`, `topDict.FDSelect` and `topDict.CharStrings` (`charStringsIndex`, plus the name map when `charStringsAreIndexed`), with that FD's `Private`;
    - `hmtx` gets `(1000, control-bounds xMin)`; every Unicode cmap subtable maps U+FFFD to it.
  - **`check`** raises at the first rule that fails, in this order, naming the file and the rule:
    1. the first four bytes are not `OTTO`, there is no `CFF ` table, or there is a `CFF2` one ("OTTO");
    2. `cff.major != 1`;
    3. global subroutines, or any FD `Subrs` ("subroutines");
    4. no `(3,1)` format 4 or `(3,10)` format 12 cmap, or the best cmap lacks N code points of the set ("lacks N");
    5. `head.macStyle & 3` is set, or `usWeightClass >= 600` ("regular");
    6. a table outside `KEPT_TABLES`, or a GPOS feature other than `kern`;
    7. a glyph not mapped from `oversize` with more than `MAX_POINTS` points or `MAX_CONTOURS` contours ("points"), counted with a pen as `C:/tmp/cjkfonts-out/cff_points.py` did: 1 point per `moveTo` and `lineTo`, 3 per `curveTo`.
  - **`build_family`** calls `subset_cjk(source, family_set(family["charset"], unihan_zip), family["fontName"], out, oversize=OVERSIZE.get(family["name"], frozenset()))`.

- [ ] **Step 4: Run the tests.** Run: `python lib/EpdFont/scripts/test_cjk_subset.py`. Expected: `OK`, or `OK (skipped=1)` while the Unihan is not cached.

- [ ] **Step 5: Commit.**
  ```bash
  git add lib/EpdFont/scripts/cjk_subset.py lib/EpdFont/scripts/test_cjk_subset.py
  git commit -m "feat(fonts): cjk_subset.py cuts per-language CFF subsets of Noto CJK"
  ```

### Task 2: `sd-fonts.yaml` and `build-sd-fonts.py` build the `.otf` families

**Files:** Modify `lib/EpdFont/scripts/sd-fonts.yaml` and `lib/EpdFont/scripts/build-sd-fonts.py`; create `lib/EpdFont/scripts/test_build_sd_fonts.py`.

**Interfaces:**
- Consumes Task 1's `cjk_subset.build_family`, `check_fonttools`, `CHARSETS` and `BuildError`.
- **`sd-fonts.yaml`:**
  - top level: `unihan: {url, sha256}`, and `fonttools: "4.63.0"` (my call: a pin, because another fontTools may write other bytes, which is spec 3's anti-goal "rebuilding the fonts without a change to their inputs rewrites their bytes");
  - each CJK entry: `name`, `description`, `format: otf`, `inDownloadAll: false`, `charset`, `fontName`, `source: {url, sha256}`, with the Global Constraints' values;
  - the commented-out CJK bitmap block is removed;
  - the header documents `format: otf` (a CFF subset by `cjk_subset.py`, one Regular file drawn at every size), `charset`, `fontName`, `source`, `inDownloadAll`, and `pip install fonttools==4.63.0` before rebuilding them.
- **`build-sd-fonts.py`** (a module loaded by path in tests):
  ```python
  def sha256_of(path: Path) -> str
  def download_font(url: str, dest: Path, sha256: str | None = None) -> Path   # RuntimeError naming the URL on a mismatch, cached or downloaded
  def resolve_unihan(config: dict) -> Path | None                             # downloaded_fonts/Unihan/Unihan-<version>.zip
  def build_otf_family(family: dict, output_dir: Path, unihan: Path | None, fonttools_pin: str | None) -> tuple[str, bool, str]
  def build_family(family, output_base, verbose=False, timeout=600, unihan=None, fonttools_pin=None)  # `format: otf` first
  ```

**From the prototype** (`build-sd-fonts.py`, `sd-fonts.yaml`): `sha256_of`, `download_font(…, sha256)`, `resolve_unihan`, the download phase that resolves `source` too, the `OK:` line that carries the build's message, the summary that counts every font file, the eight entries' URLs and hashes, and the Unihan pin. Change `ttf` to `otf` and `ttf_subset` to `cjk_subset` throughout. Its `generate_manifest()` change belongs to Task 3.

- [ ] **Step 1: Write the failing tests** in `test_build_sd_fonts.py`, loading the script with `importlib.util.spec_from_file_location("build_sd_fonts", SCRIPTS / "build-sd-fonts.py")`.
  - `test_a_cached_source_must_have_its_pinned_sha256`: with `b"font"` written to a temp `dest`, `download_font("https://example.invalid/x.otf", dest, sha256_of(dest))` returns `dest` without the network, and with `"0" * 64` it raises `RuntimeError` naming the URL.
  - `test_an_otf_entry_is_built_by_cjk_subset`: with `cjk_subset.family_set` patched to return the fixture's cmap, `build_family({"name": "TestSansJP", "format": "otf", "charset": "ja", "fontName": "Test Sans JP", "source": {"path": <fixture>}}, tmp, unihan=<a made-up zip>, fonttools_pin=fontTools.version)` returns `("TestSansJP", True, <a message holding "characters">)` and writes `tmp/TestSansJP/TestSansJP-Regular.otf`.
  - `test_sd_fonts_yaml_pins_every_otf_family`, on the real file:
    ```python
    otf = [f for f in config["families"] if f.get("format") == "otf"]
    self.assertEqual([f["name"] for f in otf], ["NotoSansJP", "NotoSerifJP", "NotoSansSC", "NotoSerifSC",
                                                "NotoSansTC", "NotoSerifTC", "NotoSansKR", "NotoSerifKR"])
    for f in otf:
        self.assertIn(f["charset"], cjk_subset.CHARSETS)
        self.assertIs(f["inDownloadAll"], False)
        self.assertLessEqual(len(f["description"]), 32)
        self.assertEqual(f["fontName"].replace(" ", ""), f["name"])
        self.assertRegex(f["source"]["url"], r"^https://github\.com/notofonts/noto-cjk/raw/(Sans2\.004/Sans|Serif2\.003/Serif)/OTF/")
        self.assertRegex(f["source"]["sha256"], r"^[0-9a-f]{64}$")
    self.assertTrue(config["unihan"]["url"].endswith("/17.0.0/ucd/Unihan.zip"))
    self.assertRegex(config["unihan"]["sha256"], r"^[0-9a-f]{64}$")
    self.assertEqual(config["fonttools"], "4.63.0")
    # the bitmap entries, commented out or not (the source URLs name NotoSansCJKjp... and stay)
    self.assertNotRegex(text, r"(?m)^\s*#?\s*-?\s*name:\s*(NotoSansCJK|NotoSerifCJK|NotoSansHangul)\s*$")
    ```

- [ ] **Step 2: Run them to see them fail.** Run: `python lib/EpdFont/scripts/test_build_sd_fonts.py`. Expected: 3 errors (no `sha256_of`, no `otf` dispatch, no entries).

- [ ] **Step 3: Implement.**
  - The config opens as UTF-8.
  - **`build_otf_family`:** `check_fonttools(pin)`; resolves `source`, which takes `path` or `url` + `sha256` like a style spec; calls `cjk_subset.build_family(family, output_dir, source, unihan)`, `build_family` passing `output_dir = output_base / name`. It returns a one-line message: characters, cmap, glyphs, MB, `kernPairs`, the dropped tables in KB. It catches `BuildError`, `RuntimeError` and `FileNotFoundError`.
  - **`main()`:** resolves the Unihan only when an `otf` family is selected; passes `config.get("fonttools")` and the Unihan path to `build_family`; prints each family's message after `OK: <name> (<n> files, <MB>)`; counts `.cpfont` and `.otf` outputs.

- [ ] **Step 4: Run the tests.** Run: `python lib/EpdFont/scripts/test_build_sd_fonts.py`. Expected: `OK`.

- [ ] **Step 5: Commit.**
  ```bash
  git add lib/EpdFont/scripts/sd-fonts.yaml lib/EpdFont/scripts/build-sd-fonts.py lib/EpdFont/scripts/test_build_sd_fonts.py
  git commit -m "feat(fonts): sd-fonts.yaml lists the eight CJK .otf families, pinned; the CJK bitmap entries go"
  ```

### Task 3: Two manifests from one scan (`generate-font-manifest.py`)

**Files:** Modify `scripts/generate-font-manifest.py` and `lib/EpdFont/scripts/build-sd-fonts.py` (`generate_manifest()` only); create `scripts/test_generate_font_manifest.py`.

**Interfaces:**
```
generate-font-manifest.py --input DIR --base-url URL --output fonts.json [--output-v3 fonts-v3.json] [--config sd-fonts.yaml]
```
```python
def load_config(yaml_path: Path) -> dict[str, dict]                 # name -> entry
def read_outline_style(filepath: Path) -> str                       # head.macStyle & 3 -> STYLE_NAMES
def scan_font_files(input_dir: Path) -> dict[str, list[Path]]       # .cpfont, .ttf, .otf; ValueError on a family of two formats
def family_format(files: list[Path]) -> str                         # "cpfont" | "ttf" | "otf"
def build_manifest(families, base_url, input_dir, config=None, version=2) -> dict
def write_manifest(manifest: dict, output_path: Path) -> None       # UTF-8, LF, indent 2, ensure_ascii=False, final newline
```
- An outline file's family is its name before the last `-` (`<Family>-<Style>.otf`). An outline family's `styles` is each file's own style, the files in name order.
- **Version 2** keeps today's family keys and order, `name, description, styles, files`, and holds only `.cpfont` families. **Version 3** puts `format` after `description`, then `"inDownloadAll": false` only where the config says so, then `styles` and `files`.
- **With `--config`**, only listed families are written, with the config's descriptions. A folder it does not list is reported on stderr (`WARNING: <name> is not in <config>; left out of the manifests`). A family whose files are not of its configured `format` (default `cpfont`) is an error, exit 1. **Without `--config`**, every family found is written, with its name as its description.
- `--descriptions-from` goes. Its only caller, `build-sd-fonts.py`'s `generate_manifest()`, passes `--config <config>` and `--output-v3 <manifest dir>/fonts-v3.json` instead, in this commit.

**From the prototype:** its whole `generate-font-manifest.py` diff, its `generate_manifest()` change and `test_generate_font_manifest.py`. Change `.ttf`-only scanning to `.ttf` and `.otf`, `read_ttf_style` to `read_outline_style`, and the formats to `ttf`/`otf`.

- [ ] **Step 1: Write the failing tests**, the prototype's, adapted. The made-up input holds `Alpha/Alpha_10.cpfont` and `Alpha_12.cpfont` (a real `.cpfont` header, as the prototype writes them), `NotoSansJP/NotoSansJP-Regular.otf` (a copy of `test/fixtures/fonts/NotoSansJP-CffFlat.otf`), and a config listing Alpha (`cpfont`) and NotoSansJP (`format: otf`, `inDownloadAll: false`).
  - `test_fonts_json_lists_the_cpfont_families_as_version_2`: `version` 2; families `["Alpha"]`; neither `format` nor `inDownloadAll` anywhere in the file's text.
  - `test_fonts_v3_lists_every_family_with_its_format`:
    ```python
    self.assertEqual(v3["version"], 3)
    self.assertEqual(families["Alpha"]["format"], "cpfont"); self.assertNotIn("inDownloadAll", families["Alpha"])
    jp = families["NotoSansJP"]
    self.assertEqual((jp["format"], jp["inDownloadAll"], jp["styles"]), ("otf", False, ["regular"]))
    self.assertEqual(jp["files"], [{"name": "NotoSansJP/NotoSansJP-Regular.otf", "size": size, "crc32": zlib.crc32(data)}])
    self.assertEqual(list(jp), ["name", "description", "format", "inDownloadAll", "styles", "files"])
    ```
  - `test_a_folder_the_config_does_not_list_is_reported_and_left_out`: with `NotoSansCJK/NotoSansCJK_12.cpfont` added, stderr names `NotoSansCJK` and neither manifest lists it.
  - `test_without_a_config_every_family_found_is_listed`.
  - `test_a_family_whose_files_are_not_its_configured_format_fails` and `test_a_family_of_two_formats_fails` (`.otf` and `.ttf` in one folder): exit code 1.
  - `test_an_outline_files_style_is_its_mac_style`: a copy saved with `macStyle = 1` reads `"bold"`.
  - `test_the_same_fonts_give_the_same_bytes`: two runs are identical and hold no `b"\r\n"`.

- [ ] **Step 2: Run them to see them fail.** Run: `python scripts/test_generate_font_manifest.py`. Expected: FAIL, `--config` and `--output-v3` unknown.

- [ ] **Step 3: Implement** the interfaces above.

- [ ] **Step 4: Run the tests.** Run: `python scripts/test_generate_font_manifest.py && python lib/EpdFont/scripts/test_build_sd_fonts.py`. Expected: `OK` twice.

- [ ] **Step 5: Commit.**
  ```bash
  git add scripts/generate-font-manifest.py scripts/test_generate_font_manifest.py lib/EpdFont/scripts/build-sd-fonts.py
  git commit -m "feat(fonts): the manifest generator writes fonts.json and fonts-v3.json from one scan"
  ```

### Task 4: Build the fonts; remove the bitmap families

**Files:**
- Delete: `assets/sd-fonts/NotoSansCJK/`, `assets/sd-fonts/NotoSerifCJK/`, `assets/sd-fonts/NotoSansHangul/`
- Create: `assets/sd-fonts/<Family>/<Family>-Regular.otf` ×8, `assets/sd-fonts/OFL.txt`, `assets/sd-fonts/fonts-v3.json`, `test/ttf_font/ShippedFamiliesTest.cpp`
- Modify: `assets/sd-fonts/fonts.json`, `README.md` (Attributions), `docs/sd-fonts-preview.md`, `lib/EpdFont/scripts/generate-sd-fonts.sh`, `test/ttf_font/CMakeLists.txt` (the new file; `SD_FONTS_DIR="${REPO_ROOT}/assets/sd-fonts"`), `scripts/test_generate_font_manifest.py`, `lib/EpdFont/scripts/test_build_sd_fonts.py`

**Interfaces:**
- Consumes Tasks 1–3, and master's `ttf::isOutlineFontFileName`, `ttf::probeFile`, `ttf::assignStyles`, `ttf::fitsStore`, `TtfFontSet::load`, `TtfFont::setBodySize`, `TtfFont::glyph`, `ttf::parseFace`, `ttf::glyphIndex`, `MemoryFile` (`test/ttf_font/TtfFontTestUtil.h`) and `SimNorFlash`.
- Produces the committed families and manifests that Tasks 5, 6 and 10 read.

- [ ] **Step 1: The tests on what ships.**
  - **`test/ttf_font/ShippedFamiliesTest.cpp`**, from the prototype's file of that name:
    - `ShippedOtfFamilies.EachInstallsWithinTheX3StoreAndDraws`:
      - exactly 8 files under `SD_FONTS_DIR` pass `ttf::isOutlineFontFileName`;
      - each probes as style 0, weight 400, not variable, and `fitsStore(&bytes, &probe.sdBytes, 1, kX3FontAreaBytes, 32)` holds, with `kX3FontAreaBytes = 3538944 - 65536` as `TtfFamilyTest` uses it;
      - each loads alone into `SimNorFlash flash(kX3FontAreaBytes)` with `TtfFontSet::load`, and at 12 pt these draw (`glyph(0, cp) != nullptr`, `advanceX > 0`): the first mapped code point, every 97th mapped one from U+0020 to U+2FFFF but U+9F98, and U+FFFD;
      - more than 7,000 code points are mapped;
      - `glyph(EpdFontFamily::BOLD, 0x6F22)` (漢), or U+6C49 (汉) in the SC families, which do not map 漢, is not null: the families are regular only, and bold draws through it (synthetic since step 4).
    - `ShippedOtfFamilies.SerifTcDrawsTheOneOversizeGlyphAsMissing`: NotoSerifTC maps U+9F98 and `glyph(0, 0x9F98) == nullptr`, its 621 points being over the 512-point cap (spec 9.2); U+9F95, mapped, draws (U+9F97 is not in the set).
  - **`scripts/test_generate_font_manifest.py`** gains `ShippedManifests.test_the_shipped_manifests_are_what_the_generator_writes`: the generator, run on `assets/sd-fonts` with the real config and the master base URL into a temp folder, writes the committed `fonts.json` and `fonts-v3.json`, compared after `.replace(b"\r\n", b"\n")` (the working tree may hold CRLF under `core.autocrlf`).
  - **`lib/EpdFont/scripts/test_build_sd_fonts.py`** gains `test_the_ofl_names_every_copyright_the_hosted_fonts_carry`: for every `assets/sd-fonts/*/*.otf`, the year range of its name ID 0 ("2014-2021" or "2017-2024") appears in `assets/sd-fonts/OFL.txt`, and so does "Copyright 2022 The Noto Project Authors".

- [ ] **Step 2: Run them to see them fail.**
  - `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_filter='ShippedOtfFamilies.*'`. Expected: FAIL, 0 files found.
  - `python scripts/test_generate_font_manifest.py ShippedManifests`. Expected: FAIL, no `fonts-v3.json`.

- [ ] **Step 3: Delete the bitmap families** with `git rm -r -q assets/sd-fonts/NotoSansCJK assets/sd-fonts/NotoSerifCJK assets/sd-fonts/NotoSansHangul`.
  - `generate-sd-fonts.sh` (my call: it builds the all-language bitmap family this work replaces): remove the NotoSansCJK download and its header comment, its clean-up path, its job `[3/3]` and its `wait`; renumber the others `[1/2]` and `[2/2]`.
  - `docs/sd-fonts-preview.md`: remove the three rows and the clause about the commented-out CJK entries; add the eight `.otf` families to the table and to the "no preview image yet" list (the English sample says nothing of a CJK face).

- [ ] **Step 4: Fill the download cache from the pinned copies, then build.**
  ```bash
  S=lib/EpdFont/scripts/downloaded_fonts
  for f in JP:jp SC:sc TC:tc KR:kr; do r=${f%%:*}; l=${f##*:}
    mkdir -p $S/NotoSans$r $S/NotoSerif$r
    cp C:/tmp/cjkfonts-src/noto/NotoSansCJK$l-Regular.otf $S/NotoSans$r/
    cp C:/tmp/cjkfonts-src/noto/NotoSerifCJK$l-Regular.otf $S/NotoSerif$r/
  done
  mkdir -p $S/Unihan && cp C:/tmp/cjkfonts-src/unihan/Unihan-17.0.0.zip $S/Unihan/
  python lib/EpdFont/scripts/build-sd-fonts.py \
    --only NotoSansJP,NotoSerifJP,NotoSansSC,NotoSerifSC,NotoSansTC,NotoSerifTC,NotoSansKR,NotoSerifKR \
    --output-dir assets/sd-fonts --jobs 4 --timeout 3600 --manifest \
    --base-url https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/
  ```
  Expected: eight `OK` lines with these characters, cmap and glyphs, and bytes within 1% of these (the measured files of `C:/tmp/cjkfonts-out/otf`, which lack U+FFFD and the new names):

  | Family | characters | cmap | glyphs | bytes |
  |---|--:|--:|--:|--:|
  | NotoSansJP | 7,367 | 7,367 | 7,362 | 1,690,040 |
  | NotoSerifJP | 7,367 | 7,367 | 7,363 | 2,426,160 |
  | NotoSansSC | 9,214 | 9,216 | 9,209 | 2,011,932 |
  | NotoSerifSC | 9,214 | 9,216 | 9,211 | 2,879,896 |
  | NotoSansTC | 14,052 | 14,052 | 14,043 | 3,451,900 |
  | NotoSerifTC | 14,052 | 14,052 | 14,044 | 5,048,800 |
  | NotoSansKR | 17,304 | 17,304 | 17,035 | 2,695,124 |
  | NotoSerifKR | 17,304 | 17,304 | 17,038 | 7,329,368 |

  - The two extra SC cmap entries, U+220B and U+2245, come along with glyphs the set keeps; spec 4.1's "Characters" column counts the cmap.
  - The manifest step warns of nothing but the three deleted families, if their folders linger.
  - `git status --short assets/sd-fonts` shows the 8 new files, `fonts-v3.json` new and `fonts.json` modified by the three families fewer, no other family's entry changed (checked 2026-10-22: master's generator rewrites today's Latin entries unchanged).
  - Record each family's `kernPairs` and dropped-table sizes for the PR (spec 4.3).

- [ ] **Step 5: Rebuild to prove the bytes repeat.** Run the build again with `--only NotoSansJP,NotoSerifKR`, the smallest and the largest. Expected: `git status --short assets/sd-fonts` as after Step 4.

- [ ] **Step 6: `OFL.txt` and the credits.**
  - **`assets/sd-fonts/OFL.txt`:** an opening paragraph naming the eight families as subsets of Noto Sans CJK and Noto Serif CJK (releases Sans2.004 and Serif2.003, https://github.com/notofonts/noto-cjk), made by `lib/EpdFont/scripts/cjk_subset.py`, with U+FFFD from Noto Sans; then these three lines, the fonts' own name ID 0 years (the noto-cjk LICENSE files carry no copyright line); then the OFL 1.1 text of `lib/EpdFont/builtinFonts/source/NotoSans/OFL.txt` below its copyright line.
    ```
    Noto Sans CJK: Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.
    Noto Serif CJK: Copyright 2017-2024 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.
    Noto Sans: Copyright 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic).
    ```
  - **`README.md`, under `# Attributions`:** a new subsection `## Fonts the Font Manager downloads (assets/sd-fonts)` with one bullet: "**Noto Sans CJK and Noto Serif CJK** (Adobe) — subset per language into NotoSansJP … NotoSerifKR by `lib/EpdFont/scripts/cjk_subset.py`, with U+FFFD from Noto Sans. https://github.com/notofonts/noto-cjk — SIL Open Font License 1.1, [`assets/sd-fonts/OFL.txt`](assets/sd-fonts/OFL.txt)."

- [ ] **Step 7: Run the tests.**
  - `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_filter='ShippedOtfFamilies.*'`. Expected: PASS.
  - `python scripts/test_generate_font_manifest.py && python lib/EpdFont/scripts/test_build_sd_fonts.py && python lib/EpdFont/scripts/test_cjk_subset.py`. Expected: `OK` three times; the real-sets test runs now that the Unihan is cached.

- [ ] **Step 8: Commit.**
  ```bash
  git add assets/sd-fonts README.md docs/sd-fonts-preview.md lib/EpdFont/scripts/generate-sd-fonts.sh \
    test/ttf_font scripts/test_generate_font_manifest.py lib/EpdFont/scripts/test_build_sd_fonts.py
  git commit -m "feat(fonts): the eight Noto CJK subsets as .otf, fonts-v3.json; the CJK bitmap families removed"
  ```

### Task 5: `FontDownloadRules`

**Files:** Create `src/util/FontDownloadRules.h` and `test/font_manifest_parser/FontDownloadRulesTest.cpp`; modify `test/font_manifest_parser/CMakeLists.txt` (the new file; `SD_FONTS_DIR="${REPO_ROOT}/assets/sd-fonts"`).

**Interfaces:** header-only, everything `inline` or `constexpr`, namespace `FontDownloadRules`. Task 6 adds `downloadAllTakes`, which needs the catalog's flag.
```cpp
enum class FileKind : uint8_t { Unknown, Cpfont, TrueType, OpenTypeCff };
FileKind kindOf(const char* name);                        // ".cpfont" / ".ttf" / ".otf" in any case; else Unknown
FileKind kindOfFormat(const char* value, size_t len);     // "cpfont" / "ttf" / "otf" exactly; else Unknown
constexpr size_t kHeadBytes = 8;
bool headOk(FileKind kind, const uint8_t* head, size_t len);
constexpr uint64_t kSpaceMarginBytes = 1024 * 1024;
constexpr bool fits(uint64_t freeBytes, uint64_t downloadBytes);   // freeBytes >= downloadBytes + kSpaceMarginBytes
constexpr bool isUpdate(size_t same, size_t changed, size_t absent); // changed > 0 || (absent > 0 && same > 0)
```
- `headOk`: `Cpfont` needs `"CPFONT\0\0"`, `TrueType` `00 01 00 00` or `"true"`, `OpenTypeCff` `"OTTO"`. `Unknown`, and anything shorter than `kHeadBytes`, is refused; `ttcf`, `wOFF` and `wOF2` match no kind.
- `isUpdate`'s counts are of the manifest's files: `same` on the card at their manifest size, `changed` at another size, `absent` missing.

**From the prototype:** its `FontDownloadRules.h` and `FontDownloadRulesTest.cpp`, extended with `OpenTypeCff`, `kindOfFormat` and `isUpdate`.

- [ ] **Step 1: Write the failing tests** (`head(kind, s)` calls `headOk(kind, data, size)`; `bytes(s, n = 8)` builds a `std::string` of `n` bytes):
  ```cpp
  TEST(FontDownloadRulesTest, AFilesKindIsItsExtensionInAnyCase)   // .cpfont/.CPFONT, .ttf/.TTF, .otf/.OTF;
                                                                   // "x.otf.part", "otf", "x.ttc", "x.woff", "", nullptr -> Unknown
  TEST(FontDownloadRulesTest, AManifestFormatNamesAKindExactly)   // "cpfont","ttf","otf" -> kinds; "OTF","woff2","" -> Unknown
  TEST(FontDownloadRulesTest, ACpfontStartsWithItsMagic)
  TEST(FontDownloadRulesTest, ATrueTypeFileStartsWithTrueTypeOutlines) {
    EXPECT_TRUE(head(FileKind::TrueType, bytes("\x00\x01\x00\x00\x00\x0B\x00\x80")));
    EXPECT_TRUE(head(FileKind::TrueType, bytes("true\x00\x0B\x00\x80")));
    EXPECT_FALSE(head(FileKind::TrueType, bytes("OTTO\x00\x0B\x00\x80")));   // spec 10: OTTO named .ttf
    EXPECT_FALSE(head(FileKind::TrueType, bytes("ttcf\x00\x02\x00\x00")));
  }
  TEST(FontDownloadRulesTest, AnOpenTypeCffFileStartsWithOtto) {
    EXPECT_TRUE(head(FileKind::OpenTypeCff, bytes("OTTO\x00\x0B\x00\x80")));
    for (const char* bad : {"\x00\x01\x00\x00\x00\x0B\x00\x80", "ttcf\x00\x02\x00\x00", "wOFF\x00\x01\x00\x00",
                            "wOF2\x00\x01\x00\x00", "CPFONT\0\0"})   // spec 10: ttcf, WOFF, a .cpfont named .otf
      EXPECT_FALSE(head(FileKind::OpenTypeCff, bytes(bad, 8)));
    EXPECT_FALSE(head(FileKind::OpenTypeCff, bytes("OTTO", 4)));
  }
  TEST(FontDownloadRulesTest, EveryShippedOtfFamilyPasses)        // first 8 bytes of each SD_FONTS_DIR/*/*.otf: 8 checked
  TEST(FontDownloadRulesTest, TheCardMustHoldTheDownloadAndAMargin) {
    EXPECT_TRUE(fits(10 * MB + kSpaceMarginBytes, 10 * MB));
    EXPECT_FALSE(fits(10 * MB + kSpaceMarginBytes - 1, 10 * MB));
    EXPECT_FALSE(fits(0, 0));
    EXPECT_TRUE(fits(64ull * 1024 * MB, 9 * MB));                 // a 64 GB card, past 32 bits
  }
  TEST(FontDownloadRulesTest, AFolderHoldingNoneOfTheManifestsFilesIsNotAnUpdate) {  // Review Focus 2
    EXPECT_FALSE(isUpdate(0, 0, 1));   // someone else's family of that name
    EXPECT_FALSE(isUpdate(1, 0, 0));
    EXPECT_TRUE(isUpdate(1, 0, 1));    // a hosted family that gained a file
    EXPECT_TRUE(isUpdate(0, 1, 0));    // a file of the same name, other bytes
    EXPECT_FALSE(isUpdate(0, 0, 0));
  }
  ```

- [ ] **Step 2: Run them to see them fail.** Run: `cmake --build build/test --target FontManifestParserTest -j 8`. Expected: compile error, `FontDownloadRules.h` not found.

- [ ] **Step 3: Implement `src/util/FontDownloadRules.h`.** Its top comment says what the Font Manager and the web font API check of a download, kept apart from the card so the host can test it.

- [ ] **Step 4: Run the tests.** Run: `./build/test/font_manifest_parser/FontManifestParserTest.exe --gtest_filter='FontDownloadRulesTest.*'`. Expected: PASS, 8 tests.

- [ ] **Step 5: Commit.**
  ```bash
  git add src/util/FontDownloadRules.h test/font_manifest_parser
  git commit -m "feat(fonts): FontDownloadRules: file kinds, their first bytes, room on the card, updates"
  ```

### Task 6: Manifest version 3 in the parser, the reader and the catalog

**Files:**
- Modify: `lib/JsonParser/FontManifestParser.h`, `.cpp`; `src/util/FontManifestReader.h`, `.cpp`; `src/util/FontCatalog.h`, `.cpp`; `src/util/FontDownloadRules.h` (`downloadAllTakes`)
- Modify: `src/activities/settings/FontDownloadActivity.h` and `src/network/CrossPointWebServer.cpp` (each loses its `FONT_MANIFEST_URL` block; both `.cpp` files already include `util/FontManifestReader.h`)
- Modify: `test/font_manifest_parser/FontManifestParserTest.cpp`, `FontManifestReaderTest.cpp`, `FontCatalogTest.cpp`, `FontDownloadRulesTest.cpp`, `CMakeLists.txt` (`FONT_MANIFEST_V3_PATH="${REPO_ROOT}/assets/sd-fonts/fonts-v3.json"`)
- Modify: `docs/file-formats.md`

**Interfaces:**
- Consumes Task 5's `kindOf` and `kindOfFormat`.
- Produces:
  ```cpp
  // FontDownloadRules.h, which now includes FontCatalog.h: "Download all" on the device, and the web's "all" with updates.
  bool downloadAllTakes(const FontCatalog& catalog, size_t family);   // !installed(family) && inDownloadAll(family)
  // FontManifestParser.h, appended to FontManifestCallbacks, so `FontManifestCallbacks cb{}` stays valid
  void (*onFamilyFormat)(void* ctx, const char* value, size_t len);   // "format", version 3
  void (*onFamilyInDownloadAll)(void* ctx, bool value);                // "inDownloadAll", version 3; booleans only
  // FontManifestReader.h
  #define FONT_MANIFEST_URL "https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/fonts-v3.json"  // #ifndef-guarded
  // FontCatalog.h
  bool outline(size_t family) const;        // a TrueType or CFF family (format "ttf" or "otf"): one file per style, every size
  bool inDownloadAll(size_t family) const;  // false out of range
  void Builder::outline(bool value);        // the open family's; reset by beginFamily()
  void Builder::inDownloadAll(bool value);  // true until said otherwise
  ```
- **The catalog:** flags bit 3 is `OUTLINE` and bit 4 `NOT_IN_DOWNLOAD_ALL`; `keepFamily` writes the open family's flags where it wrote 0; `VERSION` goes to 2. My call: the catalog keeps `outline`, not the format, because the installer checks each file by its own kind and the registry treats `.ttf` and `.otf` alike.
- **The reader:**
  - accepts versions 1 to 3;
  - takes a family's `format` from the last one given, `cpfont` when none is, and leaves out a family of an unknown format with `LOG_INF(logTag, "Manifest entry %s left out: format %s", family, format)`;
  - records the kind of every file as it comes, since the format may follow the files: `Work` gains `FontDownloadRules::FileKind format`, `char formatName[16]` for the log and `uint8_t fileKinds`, one bit per `FileKind` seen;
  - when the family closes, rejects it if any kind but its format's was seen, with `LOG_ERR(logTag, "Manifest entry rejected, a file of %s is not of its format %s", family, format)` (the file names are gone by then);
  - sets `builder->outline(format != Cpfont)` before `keepFamily`.

**From the prototype:** the parser diff (the `FORMAT` and `IN_DOWNLOAD_ALL` keys, `onBool`), the catalog diff with `trueType` renamed `outline` and `TRUE_TYPE` `OUTLINE`, and the reader's callbacks. Its per-kind booleans become `fileKinds`.

- [ ] **Step 1: Write the failing tests.**
  - **`FontManifestParserTest`**, its `Recorder` recording `"format=<v>"` and `"inDownloadAll=<true|false>"`:
    - `AVersion3FamilyGivesItsFormatAndItsPlaceInDownloadAll`: the events in document order.
    - `AnInDownloadAllThatIsNotABooleanIsIgnored`: `"false"`, `0` and `null` give no `inDownloadAll=` event, and three whole families.
    - `ABooleanInAFamiliesArrayIsStillAnEmptyEntry`: `"families":[true]` gives `family{`, `}family`.
    - `AFormatOutsideAFamilyIsIgnored`: at the root and inside a file object, no `format=` event.
  - **`FontManifestReaderTest`:**
    - `OnlyVersions1To3AreAccepted` replaces `OnlyVersions1And2AreAccepted`: version 4 is `UnsupportedVersion` with `version() == 4`; 1 and 3 are `Ok`.
    - `AnOutlineFamilyIsMarkedAndMayStayOutOfDownloadAll`: NotoSansJP is `otf`, `inDownloadAll: false`, file `NotoSansJP/NotoSansJP-Regular.OTF`; Gamma `ttf` with a `.ttf`; Alpha `cpfont`; Beta gives no format. Expected: `outline` true for NotoSansJP and Gamma only; `inDownloadAll` false for NotoSansJP only; NotoSansJP's `fileLocalName` is `"NotoSansJP-Regular.OTF"`.
    - `AFamilyOfAFormatThisReaderDoesNotKnowIsLeftOut`: `"format":"woff2"` leaves only the other family, and `blockBytes() == blockBytesFor({1, 1, stringBytes(catalog)})`.
    - `AFileNotOfItsFamilysFormatRejectsTheFamily`: dropped are an `otf` family with a `.ttf` file, a format-less family with an `.otf` file, and an `otf` family with `x.otf.part`; `Fine` is kept.
    - `TheFormatMayFollowTheFilesAndTheLastOneWins` (Review Focus 4): `{"name":"Late","files":[{… .otf}],"format":"otf"}` and `{"name":"Twice","format":"otf","files":[{… _10.cpfont}],"format":"cpfont"}` are both kept, `outline` true then false.
    - `ReadsTheShippedV3Manifest`: version 3, `Ok`; the 8 outline families have the Global Constraints' names, `inDownloadAll` false and one file `<name>-Regular.otf` with a CRC; every other family is not outline and in Download all; no `NotoSansCJK`, `NotoSerifCJK` or `NotoSansHangul`; `blockBytes() <= 16 KB`; `baseUrl` is the master URL.
    - `TheShippedVersion2ManifestListsCpfontFamiliesOnly` (an older reader sees what it always saw): version 2; every file name ends `.cpfont`; the raw text holds neither `"format"` nor `inDownloadAll`; none of the three bitmap names; `count() == v3.count() - 8`.
    - `TheManifestUrlIsTheV3ManifestOnMaster`: `EXPECT_STREQ(FONT_MANIFEST_URL, "https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/fonts-v3.json")`.
  - **`FontCatalogTest`:** `TheOutlineAndDownloadAllFlagsSurviveACopyAndAStash` (built, `copyFamilyFrom`, then `writeTo`/`readFrom`: both flags survive each); `ACorruptOrShortStashIsRejected`'s "another version" case writes 1 instead of 2.
  - **`FontDownloadRulesTest.DownloadAllTakesOnlyMissingFamiliesThatJoinIt`** (Review Focus 1), on a catalog built with the `Builder`: A `cpfont`; B outline with `inDownloadAll(false)`; C `cpfont`, then `setInstalled(2, true)`. `downloadAllTakes` is true for A only; B stays out after `setHasUpdate(1, true)` (an update is the caller's `|| hasUpdate`); C is never taken.

- [ ] **Step 2: Run them to see them fail.** Run: `cmake --build build/test --target FontManifestParserTest -j 8`. Expected: compile errors (`onFamilyFormat`, `outline`, `FONT_MANIFEST_URL`).

- [ ] **Step 3: Implement** the interfaces, and delete the two `FONT_MANIFEST_URL` blocks.

- [ ] **Step 4: Run the tests.** Run: `cmake --build build/test --target FontManifestParserTest -j 8 && ./build/test/font_manifest_parser/FontManifestParserTest.exe`. Expected: PASS, every test of the target.

- [ ] **Step 5: `docs/file-formats.md`.**
  - A new section `## The font manifests (fonts.json, fonts-v3.json)`: the version 2 document; version 3's `format` (`cpfont`, `ttf`, `otf`) and `inDownloadAll`; who reads which (firmware before this work reads `fonts.json` and rejects a version it does not know); "a family of an unknown format is left out, and so is one whose files are not of its format"; both written by `scripts/generate-font-manifest.py --config`.
  - `fonts_families.bin`: version 2; flags bit 3 an outline family, bit 4 not in "Download all".

- [ ] **Step 6: Format and commit.**
  ```bash
  PATH="/c/tmp/cf21/bin:$PATH" bash bin/clang-format-fix
  git add lib/JsonParser src/util src/activities/settings/FontDownloadActivity.h src/network/CrossPointWebServer.cpp \
    test/font_manifest_parser docs/file-formats.md
  git commit -m "feat(fonts): manifest version 3: each family's format and its place in Download all"
  ```

### Task 7: The installer and the device's Font Manager

**Files:**
- Modify: `src/FontInstaller.h`, `src/FontInstaller.cpp`
- Modify: `src/activities/settings/FontDownloadActivity.h`, `.cpp`
- Modify: `src/network/CrossPointWebServer.cpp` (the rename at its two validation calls only)
- Modify: `lib/I18n/translations/*.yaml` (27 files); regenerated if the translations add characters: `lib/EpdFont/builtinFonts/cjk_ui_{zh,ja,ko}_{10,12}.h`
- Modify: `USER_GUIDE.md`

**Interfaces:**
- Consumes Task 5's rules and Task 6's catalog flags.
- Produces (`FontInstaller`; Task 8 uses them):
  ```cpp
  bool validateFontFile(const char* path);                                 // replaces validateCpfontFile: kindOf(path) + headOk
  static bool hasRoomFor(uint64_t bytes, uint64_t* freeBytes = nullptr);   // FontDownloadRules::fits(Storage.sdFreeBytes(), bytes)
  bool isFamilyOnCard(const char* familyName) const;                       // the registry lists it, or FONTS_DIR/<name> exists
  static bool hasUpdate(const FontCatalog& families, size_t family);       // counts same/changed/absent by opening
                                                                           // buildFontPath(name, fileLocalName(j)); isUpdate()
  ```
- New strings in `english.yaml`, translated in the 26 other files, each in its own words:
  ```yaml
  STR_FONT_SPACE_NEEDED: "SD card: %s needed, %s free"
  STR_FONT_NOT_LISTED: "Too large for this reader's font area"
  ```

**From the prototype:** `validateFontFile`, `hasRoomFor`, `isFamilyOnCard`, the row-size value with `mutable char rowSize_[16]`, and the `inDownloadAll` filters. Not its English `errorMessage_` strings; not its "installed but not listed" ERROR screen, which said "Font installation failed" over an installed family; not its summed room check in `downloadAll` (my call: one rule, per family, as the spec words it, on both sides).

- [ ] **Step 1: `FontInstaller`.**
  - Rename `validateCpfontFile` to `validateFontFile` at all four call sites: `FontDownloadActivity.cpp`'s resume and post-download checks, and the same two in `CrossPointWebServer.cpp`'s `installRemoteFamily`.
  - It logs `Not a font file the reader installs` for `Unknown` and `Not a <kind> file` for a bad head, reading `kHeadBytes` into a stack array.
  - Add `hasRoomFor`, `isFamilyOnCard` and `hasUpdate`; drop the `CPFONT_MAGIC` constants.

- [ ] **Step 2: The Font Manager** (`FontDownloadActivity`).
  - **`fetchAndParseManifest`:** `setInstalled(i, fontInstaller_.isFamilyOnCard(name))`, then `setHasUpdate(i, FontInstaller::hasUpdate(parsed, i))` for an installed family, replacing the loop that broke at the first difference. A family not installed keeps its resumable check.
  - **`refreshRowTotals` and `downloadAll`** count and take only `FontDownloadRules::downloadAllTakes(families_, i)`.
  - **`downloadFamily`**, after the DOWNLOADING screen is drawn and before `stashFamiliesToSd()`: when `FontInstaller::hasRoomFor(family.totalSize(0), &freeBytes)` is false, call `SerialTestApi::noteFontInstalled(name, false)` and go to `ERROR` with `pendingErrorAction_ = Download` (Retry once space is freed), `downloadingFamilyIndex_ = familyIdx`, and `errorMessage_` from `snprintf(tr(STR_FONT_SPACE_NEEDED), …)` of `totalSize + kSpaceMarginBytes` and `freeBytes`, both through `formatSize`.
  - **After a finished install** in `downloadFamilyImpl`, when `family.outline(0) && !fontInstaller_.isFamilyInstalled(familyName)`: log `LOG_INF("FONT", "%s installed but not listed: the glyph store cannot hold it", …)` and set `completeNote_ = tr(STR_FONT_NOT_LISTED)`; `state_ = COMPLETE` as before. `completeNote_` is a `const char*`, `nullptr` by default, cleared in `activateIndex` and in the ERROR-confirm retry, so a batch keeps any family's note. `drawChrome` draws it under "Font installed!" at `centerY + lineHeight + metrics.verticalSpacing`, in `UI_10_FONT_ID`.
  - **`provideRow`:** a family neither updatable, installed nor resumable shows its download size as the value (`formatSize` into `rowSize_`). One buffer is enough: the list reads a row's strings before it asks for the next (`ListProps::rowProvider` in FreeInkUI `list.h`).

- [ ] **Step 3: Translations and faces.**
  - Add the two keys to every `lib/I18n/translations/*.yaml`, in English order, keeping each file's line endings.
  - If the build (Step 5) stops with `CRITICAL: translation(s) use characters no UI font can draw`: copy `C:/_development/witchhunt-reader/lib/EpdFont/scripts/downloaded_fonts/NotoSansCJK` into the worktree's `downloaded_fonts/`, so the faces are cut from the files the current ones were; run `python lib/EpdFont/scripts/build_cjk_ui_fonts.py`; build again.

- [ ] **Step 4: `USER_GUIDE.md`.** Its font pages already mention `.otf` families on the card (#542) and synthetic bold (step 4).
  - Under **EPUB Font**, a bullet after **Font Size**: "- **Font Manager**: downloads font families over Wi-Fi and deletes the ones you no longer use. Each row shows the family, what it is for, and, until it is installed, its download size. **Download All** installs every missing family but the eight CJK ones (Japanese, Simplified Chinese, Traditional Chinese and Korean, each sans serif and serif, 1.6–7.0 MB): install your language's from its own row. A CJK family is one file drawn at every size. The reader restarts when you leave the Font Manager."
  - In **7. Current Limitations**, the CJK line becomes: "* **CJK (Chinese, Japanese, Korean):** books break lines and space punctuation by CJK rules; install your language's font from the **Font Manager**. Book titles, authors and chapter names outside the page still show a box for characters the menu fonts lack, and there is no vertical text, no ruby (furigana) and no CJK keyboard."

- [ ] **Step 5: Build the firmware** with the firmware command. Expected: `SUCCESS`, `check_no_float: … no floating point`, and no `CRITICAL` from `gen_i18n.py`. Note the flash and RAM against master's for the PR.

- [ ] **Step 6: Format and commit.**
  ```bash
  PATH="/c/tmp/cf21/bin:$PATH" bash bin/clang-format-fix
  git add src/FontInstaller.h src/FontInstaller.cpp src/activities/settings/FontDownloadActivity.* \
    src/network/CrossPointWebServer.cpp lib/I18n/translations lib/EpdFont/builtinFonts/cjk_ui_*.h USER_GUIDE.md
  git commit -m "feat(fonts): the Font Manager installs .otf families, shows sizes, checks the card's room"
  ```

### Task 8: The web font API and page

**Files:**
- Modify: `src/network/CrossPointWebServer.cpp` (`fetchRemoteFontManifest`, `installRemoteFamily`, `handleFontList`, `handleFontDownload`), `src/network/html/FontsPage.html`
- Create: `test/web/fonts_page_test.mjs`
- Modify: `docs/webserver.md` (section 7, Font Manager), `docs/webserver-endpoints.md` (section Fonts)

**Interfaces:**
- Consumes Task 7's `FontInstaller` functions and Task 6's `downloadAllTakes` and `outline`.
- Produces (JSON):
  - `GET /api/fonts`: each family gains `"outline": <bool>`, `SdCardFontFamilyInfo::ttf()`.
  - `POST /api/fonts/download` success: `{"ok":true,"installedCount":N,"notListed":["<family>", …]}`. `notListed` holds the installed outline families the registry does not list after the closing `refreshRegistry()`; it is empty when none.
  - Out of room: `{"ok":false,"error":"Not enough space on the SD card: %.1f MB needed, %.1f MB free","family":…,"installedCount":…}`. "Needed" is the total plus `kSpaceMarginBytes`, as on the device; the sizes are bytes / 1048576.0, as the page formats them.

- [ ] **Step 1: Write the failing web test** `test/web/fonts_page_test.mjs`. It copies `upload_names_test.mjs`'s `resolveIncludes`, `response` and a `loadPage` that fakes `fetch` only, prints `ok`/`FAIL` lines, and exits 1 on any failure. It checks:
  - **the installed list:** with `/api/fonts` answering `{families:[{name:"NotoSansJP",outline:true,sizes:[],files:[{name:"NotoSansJP-Regular.otf",size:1690040}]},{name:"Literata",outline:false,sizes:[10,12],files:[{name:"Literata_10.cpfont",size:272833},{name:"Literata_12.cpfont",size:341352}]}]}`, the two `.family-meta` texts are exactly `all sizes - 1.6 MB` and `10, 12pt - 266 KB + 333 KB`;
  - **an install the reader does not list says so** (Review Focus 5): `/api/fonts/manifest` lists NotoSerifKR not installed, and `/api/fonts/download` answers `{ok:true,installedCount:1,notListed:["NotoSerifKR"]}`. After the row's button is clicked, `#remoteStatus` reads `Installed "NotoSerifKR", but it is too large for this reader's font area: the reader does not list it.` with class `status-err`;
  - **a refused download shows the device's error:** `{ok:false,error:"Not enough space on the SD card: 8.0 MB needed, 2.1 MB free"}` gives `Install failed for "NotoSerifKR": Not enough space on the SD card: 8.0 MB needed, 2.1 MB free`.

- [ ] **Step 2: Run it to see it fail.** Run: `node test/web/fonts_page_test.mjs C:/tmp/web-deps`. Expected: `FAIL` on the first two checks.

- [ ] **Step 3: Implement.**
  - **`FontsPage.html`:** `loadFonts` writes `f.outline ? 'all sizes' : sizes + 'pt'` before ` - <file sizes>`. `downloadRemoteFamily` and `downloadAllRemoteUpdates` show the not-listed sentence, one per name in `data.notListed`, with `ok = false`, instead of the plain success line.
  - **`CrossPointWebServer.cpp`:** `fetchRemoteFontManifest` uses `isFamilyOnCard`, then `FontInstaller::hasUpdate` for an installed family; `installRemoteFamily` checks `FontInstaller::hasRoomFor(families.totalSize(familyIndex), &freeBytes)` before it creates the staging folder; `handleFontDownload`'s `all` filter is `downloadAllTakes(from, i) || from.hasUpdate(i)`, and its answer adds `notListed`; `handleFontList` adds `outline`.

- [ ] **Step 4: Run the web test and its siblings.** Run: `for t in test/web/*_test.mjs; do node "$t" C:/tmp/web-deps || echo "FAILED $t"; done`. Expected: no `FAILED` line.

- [ ] **Step 5: Docs.**
  - **`docs/webserver.md`, section 7:** the installed fonts show "with their sizes; a TrueType or OpenType family shows "all sizes"". Download fonts: "**Download all updates** fetches every missing family and every update, except the CJK families you have not installed (Japanese, Simplified Chinese, Traditional Chinese and Korean, sans serif and serif): install your language's with its own **Download**. The reader checks the card has room first. A family too large for the reader's font area is installed but not listed, and the page says so."
  - **`docs/webserver-endpoints.md`, section Fonts:** its opening names outline families beside `.cpfont`; the table gains `outline` in `/api/fonts` and `notListed` in `/api/fonts/download`; the error table gains the out-of-room answer; "`all` installs every missing family the manifest puts in Download all, and every update"; "downloads are checked by file kind: `.cpfont` magic, TrueType or `OTTO`".

- [ ] **Step 6: Build the firmware**, which bundles the page through `build_html.py`. Expected: `SUCCESS`.

- [ ] **Step 7: Format and commit.**
  ```bash
  PATH="/c/tmp/cf21/bin:$PATH" bash bin/clang-format-fix
  git add src/network/CrossPointWebServer.cpp src/network/html/FontsPage.html test/web/fonts_page_test.mjs \
    docs/webserver.md docs/webserver-endpoints.md
  git commit -m "feat(web): the font page installs .otf families, shows all sizes, says when one is not listed"
  ```

### Task 9: Future work and the CJK roadmap

**Files:** Modify `docs/future_work/cjk.md`, `docs/future_work/README.md`, `docs/future_work/reader-state.md`, `docs/design/README.md`, and every file that names the spec's path; create `docs/future_work/screen-flow.md`; move the spec to `docs/design/cjk-ttf-fonts.md`.

- [ ] **Step 1: `docs/future_work/cjk.md`.**
  - The opening list: Fonts is done. Roadmap row 5 becomes `shipped (#<PR>)`.
  - **"A CJK font readers can install"** becomes `**Shipped** (#<PR>)` in 3–4 lines: the eight families, their sizes, where they are built (`cjk_subset.py`, `sd-fonts.yaml`, `fonts-v3.json`), and that "Download all" leaves them out. The bitmap analysis and its options go.
  - **"TTF Stage 2c-4"** keeps only its Shipped paragraph and the measured line.
  - **A new section "CJK `.cpfont` fonts converted by hand"** holds spec 5's two bugs, each as what, why, where and next step:
    1. `SdCardFont::loadFromMmap` copies the interval table to the heap: 49 KB for Japanese, which turned pre-rendering off. A fix that reads it in place when 4-byte aligned is on branch `spike/mmap-intervals-in-flash`. `MAX_INTERVALS` is 4096; the Japanese set has 4,083.
    2. A `.cpfont` larger than the partition erases the whole partition (4–5 s) on the first book open after every restart, then fails to append.
  - **Under it, "龘 in Noto Serif TC":** U+9F98 has 621 points, over the CFF scratch cap of 512, and draws as missing in either outline format. A larger block for every family is not worth one glyph; a Hong Kong or full-set family would need it.
  - **A new item "Font Manager follow-ups":**
    - uploading a `.ttf` or `.otf` through the web page as it is (spec 6.5): the upload takes `.cpfont` only, and the Convert card needs its own choice for it;
    - a family installed but not listed is missing from the web page's installed list, which comes from the registry, so only the device can delete it;
    - Resume works per file, so an interrupted one-file family restarts from zero.
  - **"UI text in CJK"**, in its Next step: the fallback source is the installed CJK family (`NotoSans<XX>`/`NotoSerif<XX>`, `.otf`, drawn through `TtfFont`), not an SD bitmap font.
  - **"Testing on a device → Fonts"** becomes: install from the Font Manager; for an unmerged branch, the scratch-manifest recipe of this plan's Task 10 Step 1; rebuilding needs `pip install fonttools==4.63.0`.
- [ ] **Step 2: `docs/future_work/screen-flow.md`**, new, with one item, "A screen that goes back by itself after its message":
  - **Open:** "Font installed!", "Font installation failed" and other screens ending on a message wait for Back.
  - **Why:** the maintainer wants a timed return shared by every such screen: one mechanism, not one per screen.
  - **Where:** `FontDownloadActivity` (COMPLETE, ERROR) and the other message-only states (grep `STR_.*_DONE`, `COMPLETE`).
  - **Next step:** a design that lists the screens first.

  Index it in `docs/future_work/README.md`, and update cjk.md's row there.
- [ ] **Step 3: `docs/future_work/reader-state.md`**, one item: `FontInstaller::deleteFamily` clears `sdFontFamilyName` but not `txtSdFontFamilyName`, so the TXT/Markdown font can point at a deleted family. Found while mapping the font manager (spec 6.5). Next step: clear both, with a host test on the settings.
- [ ] **Step 4: The spec becomes a design record** (maintainer decision 2, 2026-10-22: this step ends its order of work, and code comments cite its sections).
  - `git mv docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md docs/design/cjk-ttf-fonts.md`.
  - Its header: the line saying it is deleted after the last step becomes "Design record: shipped in #531, #532, #542, #544, synthetic bold (#<PR>) and the CJK families (#<PR>). The code is the authority on what it does now."
  - `docs/design/README.md` has no list: its first paragraph's "(and, for the list scheme, by section number)" becomes "(and, for the list scheme and the CJK fonts, `cjk-ttf-fonts.md`, by section number)". Code in `lib/EpdFont/ttf/` cites it as "spec N.N"; older comments there cite the TTF engine spec, `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`.
  - Every path reference moves to the new name: run `git grep -n "2026-10-20-cjk-ttf-fonts-design"` and edit every hit (on 2026-10-22: cjk.md, the `**Spec:**` lines of five plans, this one included, and the UI-text fallback spec). Expected afterwards: hits only in this plan's Task 9, which names the old path.
  - The plans stay in `docs/superpowers/plans/` as the record. `README.md` keeps "Support for CJK" (maintainer decision 1).
- [ ] **Step 5: Check the links.** Run: `grep -n "](" docs/future_work/cjk.md docs/future_work/screen-flow.md docs/future_work/README.md docs/design/README.md`. Expected: every relative target exists.
- [ ] **Step 6: Commit.**
  ```bash
  git add docs/future_work docs/design docs/superpowers
  git commit -m "docs(cjk): the CJK fonts shipped; the spec becomes a design record; the bitmap bugs, 龘 and the font-manager items to future work"
  ```

### Task 10: On the devices

**Files:** none changed. The figures, the screenshots and the install times go into the PR description.

- [ ] **Step 1: Serve a scratch manifest; build and flash the X4.** My call: the firmware's URL names master, which has no `fonts-v3.json` before this merges, so the manifest comes from the PC over plain HTTP and the fonts from GitHub at the pushed commit. The 7 MB downloads then take the TLS path the shipped firmware will.
  ```bash
  git push -u origin feat/cjk-font-downloads; SHA=$(git rev-parse HEAD)
  python scripts/generate-font-manifest.py --input assets/sd-fonts --config lib/EpdFont/scripts/sd-fonts.yaml \
    --base-url "https://raw.githubusercontent.com/jpirnay/witchhunt-reader/$SHA/assets/sd-fonts/" \
    --output C:/tmp/fontsrv/fonts.json --output-v3 C:/tmp/fontsrv/fonts-v3.json
  python -m http.server 8000 --directory C:/tmp/fontsrv            # in the background
  PLATFORMIO_BUILD_FLAGS='-DFONT_MANIFEST_URL=\"http://<PC address>:8000/fonts-v3.json\"' \
    "$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port <X4 port>
  ```
  Then `cmd:GET:sleepTimeoutMinutes` and raise it to 60 for the run; `cmd:FS:LS:/test` must list the four CJK test books; `cmd:FS:LS:/.crosspoint/fonts` records what is installed; `cmd:EVENTS:ON`.

- [ ] **Step 2: Install from the web page.** On the File Transfer screen, join the network and read the address from a `shot:`.
  - `curl -s -X POST -H 'Content-Type: application/json' -d '{"family":"NotoSerifSC"}' http://<X4>/api/fonts/download`. Expected: `{"ok":true,"installedCount":1,"notListed":[]}` and `EVT:FONT_INSTALLED family=NotoSerifSC ok=1`.
  - `curl -s http://<X4>/api/fonts` lists NotoSerifSC with `"outline":true`; `curl -s http://<X4>/api/fonts/manifest` lists the eight CJK families, NotoSerifSC installed.

  Leave with Back; the device restarts.

- [ ] **Step 3: The rows.** Run `devctl.py cmd:ACT:SETTINGS:READER idle:20000 "select:Schriftverwaltung" cmd:BTN:CONFIRM idle:120000:SCREEN shot:/c/tmp/fm-x4-top.png`, then `"select:^NotoSansJP$:60" shot:/c/tmp/fm-x4-cjk.png`. Expected:
  - "Alle herunterladen (<n> MB)", totalling the Latin families not installed, or no such row when every Latin family is installed (Review Focus 1);
  - each CJK row shows its description whole and its size (1.6 … 7.0 MB); NotoSerifSC shows "Installiert".

- [ ] **Step 4: Install every other CJK family from the device**, in list order, NotoSerifKR last. For each family F, run `"select:^F$:60" cmd:BTN:CONFIRM "expect:EVT:FONT_INSTALLED family=F ok=1:900" idle:30000:SCREEN shot:/c/tmp/fm-x4-F.png cmd:BTN:BACK idle:10000` and record the time from Confirm to the event, from the log. Expected:
  - "Schriftart installiert!" without a note;
  - `cmd:FS:LS:/.crosspoint/fonts/F` shows `F-Regular.otf` at the manifest's size;
  - no boot log between Confirm and the event: no watchdog reset (Review Focus 3).

  Leave the Font Manager; the device restarts.

- [ ] **Step 5: The books, sans and serif, cold and stored.**

  | Book | Families | Cache folder |
  |---|---|---|
  | `/test/test_cjk_ja_soseki.epub` | NotoSansJP, NotoSerifJP | `epub_2247170459` |
  | `/test/test_cjk_zh_honglou.epub` | NotoSansTC, NotoSerifTC | `epub_1764791513` |
  | `/test/test_cjk_ko_hyeon.epub` | NotoSansKR, NotoSerifKR | `epub_791543826` |
  | `/test/test_cjk_zh_punctuation.epub` | NotoSansSC, NotoSerifSC | `epub_3251213462` |

  - First `cmd:GET:fontFamily cmd:GET:fontSize`. For each pair, set the family by the label `cmd:GET:*` lists (by its index if the label is refused) and `cmd:SET:fontSize=12pt`.
  - **Cold** (the store empty, the family new to it): `cmd:REBOOT idle:30000 "cmd:ACT:READER:<book>@1:1" idle:300000:SCREEN shot:… cmd:BTN:RIGHT idle:60000 cmd:BTN:RIGHT idle:60000 cmd:BTN:RIGHT idle:60000 cmd:TTF`. **Stored:** `cmd:FS:RM:/.crosspoint/<cache folder>/spines/0`, then the same sequence.
  - From the log, record: `createSectionFile spine=0 done: … parse=<ms>` (Appendix A's "Layout" is this `parse=` field, not `total=`: the spike's logs give `total=2903ms … parse=959` for Japanese; record `total=` beside it); the lowest `Watermark DROP … -> <bytes>`; `Page summary … prewarmMs=`, the glyph preparation; `Pre-rendered page` against `PreRender skipped`; `TTF: … sd=`.
  - These block shipping (spec 10): a failed allocation or a reader abort; pre-rendering refused on every page of a book; on the X4, a sans book slower than 1.25 × Appendix A:

    | Book | cold `parse` | stored `parse` | glyph prep |
    |---|--:|--:|--:|
    | Japanese | ≤ 1,199 ms | ≤ 621 ms | ≤ 8.75 ms/page |
    | Traditional Chinese | ≤ 1,389 ms | ≤ 335 ms | |
    | Korean | ≤ 561 ms | ≤ 286 ms | |
    | Simplified Chinese (punctuation) | ≤ 185 ms | ≤ 195 ms | |

    Serif figures are recorded only.
- [ ] **Step 6: Bold and the look.** The first page of each book carries its chapter heading in synthetic bold. Claude reviews the screenshots for stroke weight and open counters (鬱, 書, 慶 where they occur), and for no U+FFFD box in running text. The maintainer does not read CJK.
- [ ] **Step 7: A family on the card that the registry does not list** (Review Focus 5).
  - **Broken, served from the PC:** copy `C:/tmp/fontsrv/fonts-v3.json` aside; put the first 1 MB of `assets/sd-fonts/NotoSansJP/NotoSansJP-Regular.otf` at `C:/tmp/fontsrv/BrokenJP/BrokenJP-Regular.otf` (its `head` table comes first, its `CFF ` table is cut); regenerate both manifests into `C:/tmp/fontsrv` with `--input C:/tmp/fontsrv --base-url http://<PC address>:8000/` and no `--config`, so `fonts-v3.json` lists BrokenJP alone. In the Font Manager, BrokenJP installs with `ok=1` and shows "Schriftart installiert!" with the German `STR_FONT_NOT_LISTED` note; back on the list its row reads "Installiert", and Confirm opens the delete question: delete it. Then on the File Transfer screen, `{"family":"BrokenJP"}` answers `"notListed":["BrokenJP"]`. Remove it with `cmd:FS:RM`, and put the real `fonts-v3.json` back.
  - **A damaged installed copy:** over the USB Transfer screen (`tools/serial_transfer.py`, `MSYS_NO_PATHCONV=1`), replace `/.crosspoint/fonts/NotoSansJP/NotoSansJP-Regular.otf` with its first 1 MB. `cmd:GET:*` then lists no NotoSansJP family, and its Font Manager row reads "Aktualisieren", not a download size. Confirm reinstalls it: `ok=1`, and listed again.
- [ ] **Step 8: Someone else's family of the same name** (Review Focus 2). Remove `/.crosspoint/fonts/NotoSerifJP` (`cmd:FS:RM`) and upload `lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf` alone into `/.crosspoint/fonts/NotoSerifJP/`. The row reads "Installiert", and Confirm opens the delete question, not a download. Delete it there, then install NotoSerifJP again from its row.
- [ ] **Step 9: The X3** (`devctl.py --port <X3 port>`). The maintainer flashes Step 1's binary: never RTS or esptool. Then run Step 3; Step 4 for every family, NotoSerifKR last (Review Focus 3); and Step 5 for all four pairs, with `cmd:ACT:HOME idle:30000` in place of `cmd:REBOOT` (the X3 gets none; the family switch already makes the store new to the family). Its figures are recorded, not pass marks; the first two blocking rules apply (spec 10: "on either board").
- [ ] **Step 10: Restore.** Set back `fontFamily`, `fontSize` and `sleepTimeoutMinutes`; stop the HTTP server; rebuild without `PLATFORMIO_BUILD_FLAGS` and flash the X4. The CJK families may stay installed.
- [ ] **Step 11, after the merge:** a release build of the previous version, on either board. Its Font Manager loads `fonts.json`: the Latin families, no CJK family of either kind, and one Latin family installs. This is spec 3's anti-goal "an older firmware's font manager stops working, or offers a family it cannot install".
