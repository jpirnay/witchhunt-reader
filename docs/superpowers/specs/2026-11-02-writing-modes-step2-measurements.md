# Writing modes step 2: measure first, items 1-3 (measurements)

The gated phase of the design (`2026-11-02-writing-modes-step2-design.md`, "Measure first"), run on 2026-11-02 by the
plan's Tasks 1-4 (`docs/superpowers/plans/2026-11-02-writing-modes-step2-pr1.md`). These figures replace the design's
estimates for the rows they measure; the design's own table is not edited. 1 KB is 1,024 B throughout.

Item 3 ran twice. The first recipe missed, the maintainer refined decision 6 the same day, and the re-run with the
refined recipe is the item's result. The first run is kept, marked superseded.

The workspace is `C:/tmp/wm2-measure` (durable: Task 9 runs the oracle and the probe again from it). Scratch firmware
builds ran in a detached worktree of master at `55589ef59`.

## Sources

**Upstream.** crosspoint-reader #3787's head is still `dd45ba6138eb0d5a332d9c082bf7bcdfec6f16cc` (open, last updated
2026-11-02 18:36:08 UTC).
- The 45 files Task 5 takes have the same blob SHA at that head (GitHub contents API) as in the local `pr-3787`
  (`0fbd1575`): 45 of 45.
- In the seven folders compared (79 files), the only differing blobs are four `scripts/` files that are not taken:
  `build_html.py`, `patch_pioarduino_cache.py`, `patch_sdfat.py`, `patch_wolfssl.py`.
- The workspace copy (`upstream/`) is `git archive pr-3787` of the plan's paths, run with `-c core.autocrlf=false
  -c core.eol=lf`. The clone has `core.autocrlf=true`, which made the plain command write CRLF text files. With the
  two settings the copy is byte-exact: `git hash-object --no-filters` matches the blob for 45 of 45.

**Tools.**

| Tool | Version |
|---|---|
| Python | 3.14.7 (`unicodedata.unidata_version` 16.0.0) |
| uharfbuzz | 0.56.2, `version_string()` 14.5.0 (wheel `cp310-abi3-win_amd64`, into `pydeps/`) |
| fontTools | 4.63.0 (wheel `cp314-win_amd64`, into `pydeps/`) |
| Host compiler | g++ 16.2.0 (MSYS2 UCRT64, Rev3), `-std=c++20 -O2 -fno-exceptions -static` |
| C3 compiler | riscv32-esp-elf-g++ 14.2.0 (crosstool-NG esp-14.2.0_20260121) |
| PlatformIO | `C:/pio/penv/Scripts/pio.exe`, core dir `C:\pio` |

**Fonts.** The 48 files of the plan, unhinted, from `notofonts/notofonts.github.io` at
`5c82cef0a8f08806f932062e18c02be62be278f0` (`fonts/<F>/unhinted/ttf/<F>-<S>.ttf`). All 48 downloads succeeded. Name
ID 5 read with fontTools.

| File | Bytes | sha256 | name ID 5 |
|---|---:|---|---|
| NotoNaskhArabic-Bold.ttf | 158,172 | `380ac34b249d336e1a43c21053df440594e1e3db8f75fabd4d85b649ca1ea4c6` | Version 2.021 |
| NotoNaskhArabic-Regular.ttf | 158,580 | `c34fdbd98af4dbc45ca192a23d2eeb77032add83086f5fe32957d23b3f36b221` | Version 2.021 |
| NotoSans-Bold.ttf | 432,376 | `87cb2d84472a7d66da659ee47b6cdb9552326e8c128245231f191b6ac72529d9` | Version 2.015 |
| NotoSans-Regular.ttf | 431,364 | `f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8` | Version 2.015 |
| NotoSansArabic-Bold.ttf | 141,852 | `f4cb79f842a07d1bcf597879574358fb7010f73761396d1d0f2445dfedb4d2ca` | Version 2.013 |
| NotoSansArabic-Regular.ttf | 142,140 | `bd86ca02f087d7f3c3788ba458fb6b73744c7639ed276b8d870dba6def6c40d0` | Version 2.013 |
| NotoSansBengali-Bold.ttf | 102,440 | `2fe8d779e0f19576d1fc11e39f9154b4326a17b693215017dd1f5a8898c3f2c3` | Version 3.011 |
| NotoSansBengali-Regular.ttf | 103,704 | `5dceda02816fece18ea6796f474f5d3170f1d862f21b1e809cdc94b1caa4b6ec` | Version 3.011 |
| NotoSansDevanagari-Bold.ttf | 183,412 | `ff2f76a23aad41e0608c2d7dbc4bacd247ff3bec78f0ec2a8fb106b561636e58` | Version 2.007 |
| NotoSansDevanagari-Regular.ttf | 184,228 | `9c7d935139ea6a1e6ad9dbac4f6d27ece1e04bca8123c8888d00a0f9df4724cd` | Version 2.007 |
| NotoSansGujarati-Bold.ttf | 140,812 | `3ce0cf6e1d0bfe2ef5823772eeb2f25da28ec0c2c1d99c0fce529025723e61a1` | Version 2.106 |
| NotoSansGujarati-Regular.ttf | 142,760 | `18727143b8eadade0a59dca80959ef784bf8d55e07285d1b4096594dbf829f0a` | Version 2.106 |
| NotoSansGurmukhi-Bold.ttf | 35,712 | `d16804da302ecb2a7defe0502e65136870445fd88f9fddaef60b68d5f1ee7386` | Version 2.004 |
| NotoSansGurmukhi-Regular.ttf | 35,956 | `2454c944fda799d2861f11c03e492c453e33083dd1e8d99bd6baac8de71ad0f0` | Version 2.004 |
| NotoSansKannada-Bold.ttf | 133,868 | `e989c72f6fec5a0b3366af5f90e3ce8b2b8a350fc5286c6374f0353937e513f5` | Version 2.006 |
| NotoSansKannada-Regular.ttf | 135,488 | `2870274619c2a4abe5d6b11fcb7c4a372eba765b6b7881d459b0eb0a609a68eb` | Version 2.006 |
| NotoSansMalayalam-Bold.ttf | 75,916 | `165d635d82b0eb0a04275b92b9334c1ece679b99f263727c24c7c92ebcab51e9` | Version 2.104 |
| NotoSansMalayalam-Regular.ttf | 77,312 | `cc39bb1f7d63b582a2aec1abf2469a43b805a3523a27e0a0eee8cd32702541d5` | Version 2.104 |
| NotoSansOriya-Bold.ttf | 130,104 | `1c575e8aba40d6baf200e8abac534e9517f431d38cc8131715643037eec149da` | Version 2.007 |
| NotoSansOriya-Regular.ttf | 131,216 | `a16645d056017927406546aa78e4ce15e782fd8783467267b75450453d007415` | Version 2.007 |
| NotoSansSinhala-Bold.ttf | 88,508 | `438ce1325e788ec0501042dd6e798c0ec02231a42dbf42e72d17b338aa46742a` | Version 3.000 |
| NotoSansSinhala-Regular.ttf | 90,296 | `46d5b54952a624e6e3981e0968aecc5464f3b1081131dd7a1fcd42fbf7966471` | Version 3.000 |
| NotoSansTamil-Bold.ttf | 46,428 | `04a472e49fa83b387976756554fe179de631a950d79f652ebba14a39a44f5d71` | Version 2.004 |
| NotoSansTamil-Regular.ttf | 46,600 | `6634d9cc97a726e670df41281dd32b167a6b9b71f2036e19671ff08fdde0c292` | Version 2.004 |
| NotoSansTelugu-Bold.ttf | 165,564 | `44393b05a863057cc2cbfe86623fc20b906b00cf19a2ae9304cdd0f3608889db` | Version 2.005 |
| NotoSansTelugu-Regular.ttf | 165,212 | `dcb86a5da09365a2d73d725e429403dca988e6d50c1b1d564579a25dbc18bd29` | Version 2.005 |
| NotoSerif-Bold.ttf | 481,892 | `24ad531e6b05ddad8c3d89572d2c93eb86a6b74e652ce7ee3c3e171de68e84c3` | Version 2.015 |
| NotoSerif-Regular.ttf | 482,540 | `a15cfbbc1539d707115111d672d590a3d70d4f74b4c0a315956da20ae19a14e1` | Version 2.015 |
| NotoSerifBengali-Bold.ttf | 192,952 | `18d666c8332d1e7c9440420e8fb9b7ae8f773c4319af758667b6031d2ac15b2d` | Version 3.000 |
| NotoSerifBengali-Regular.ttf | 193,872 | `bb5cc3ffa1d1b1f1adfe2c4c96af5c768882b594263c428e03046bdaebf83c26` | Version 3.000 |
| NotoSerifDevanagari-Bold.ttf | 207,684 | `0220d0e616fe2a1c77a7980a8c0ad5f44fdf5c6fed575436fda348b96dc3b916` | Version 2.006 |
| NotoSerifDevanagari-Regular.ttf | 207,984 | `565948535503fba5ec4989d9fad14e8c3056b998b1133e2e05d664a73b517007` | Version 2.006 |
| NotoSerifGujarati-Bold.ttf | 98,268 | `491513721d01c60519970d8467fb319e6a2cee1e0a6df50d1eb2af790b01f17e` | Version 2.106 |
| NotoSerifGujarati-Regular.ttf | 101,120 | `162aa1a06518d36fe016ad83e7ec4a189ca69f8b516eb2eebaa4516182b86406` | Version 2.106 |
| NotoSerifGurmukhi-Bold.ttf | 36,352 | `db66789e515bf1d2f28ff1e9ca75c6ecf7191c2141307f1852ce6f6f365dbf08` | Version 2.004 |
| NotoSerifGurmukhi-Regular.ttf | 36,536 | `00435bb0255a87db60f117251c6a25b4f7ccea2138414155eb90dc2b0dc7c0cd` | Version 2.004 |
| NotoSerifKannada-Bold.ttf | 98,112 | `8c8aac32de8cf81a101ef46fcd9d8a7170730cbbd0d5a000cdd97db7db4b6d15` | Version 2.005 |
| NotoSerifKannada-Regular.ttf | 133,736 | `e35d4e41f063855dca3351e43abaa84eb909eb4a211ac0ab7a076affe569339c` | Version 2.005 |
| NotoSerifMalayalam-Bold.ttf | 72,864 | `6a81189106cfd3ec7820a5d375079f21f6dfcbc8932f84db640af57019a60d08` | Version 2.104 |
| NotoSerifMalayalam-Regular.ttf | 74,212 | `2bcff462b44ebe9e22992d5edbb4b453c338caa118c055ccb4e5925566070d66` | Version 2.104 |
| NotoSerifOriya-Bold.ttf | 147,812 | `93d819ada9ce85f912ab16f3b420fcf3d7c74c1a977b78fc740b2765ed7626f9` | Version 1.051; ttfautohint (v1.8.4.7-5d5b) |
| NotoSerifOriya-Regular.ttf | 148,896 | `b5c914dabacc2ec089401adfcf8830eb1f34ae3a62f0cf2e570461a3e847b9b7` | Version 1.051; ttfautohint (v1.8.4.7-5d5b) |
| NotoSerifSinhala-Bold.ttf | 189,032 | `3e1f9e070195c78e3c1dbaa0f08abd622fe970df1bc76f195bf7ae83ecefb7a1` | Version 2.007 |
| NotoSerifSinhala-Regular.ttf | 193,304 | `06dc06fecacb12d5193ace92dca1ee24002cd4e3a31d2b124aba8026b0b42aa4` | Version 2.007 |
| NotoSerifTamil-Bold.ttf | 51,628 | `036fd499f8f27655983571457c0787ea4e76dc8c47a028508310e2ff2656d23f` | Version 2.004 |
| NotoSerifTamil-Regular.ttf | 51,508 | `3b166155947b5f620bf0aa5ad102a398408a9a31cbe2e8b58ad0f947276adf26` | Version 2.004 |
| NotoSerifTelugu-Bold.ttf | 190,820 | `a553fa5649d2813c846bc4c0c097e72342b52ba6f045c1819712d0c482a0b751` | Version 2.005 |
| NotoSerifTelugu-Regular.ttf | 192,996 | `c99f4a0f8bfc6cf1cf72a6995d63f29deacc3070668088593a7a88182a6d8bd5` | Version 2.005 |

**Corpus.** `generate_corpus.py corpus` (Python 3.14.7, Unicode 16.0.0) wrote ten files:

| Script | Strings |
|---|---:|
| devanagari | 20,522 |
| bengali | 14,852 |
| gurmukhi | 14,545 |
| gujarati | 14,668 |
| oriya | 14,891 |
| tamil | 11,203 |
| telugu | 15,680 |
| kannada | 14,916 |
| malayalam | 17,630 |
| sinhala | 13,157 |

- On Windows `Path.write_text` writes CRLF, so the corpus files have CRLF line ends. `compare_harfbuzz.py` strips each
  line before shaping it with HarfBuzz and before sending it to the CLI. No CR reached the shaper: it would show as
  one extra glyph at the end of every line of a run, and no run mismatched. Fed as the harness feeds it, the CLI
  answered every line of each script's corpus (`tools/check_cli_lines.py`, Noto Sans Regular of each script: N lines
  in, N out, 0 `FAIL`), so `compare_harfbuzz.py`'s `zip` dropped no line. The probe strips each line the same way.
- The longest corpus line is 7 code points.

## Item 1: upstream's engine on the host

**Builds.** `ot_shape_cli_upstream.exe` (`OtShapeCli.cpp` + `lib/OtShaper/*.cpp`, `-I lib/OtShaper -I lib/Utf8`) and
`ot_shaper_probe_upstream.exe` (`tools/OtShaperProbe.cpp` + `lib/OtShaper/*.cpp`) both compile and link with our flags,
with no diagnostics. With `-Wall -Wextra` added, the probe compiles clean as well.

**The probe** (`tools/OtShaperProbe.cpp`): the four fixtures, then the 40 Indic TTFs, each after its script. Its
replaced `operator new`/`delete` count every allocation; "throwing" counts the calls of the throwing forms. Times are
`steady_clock` on this host (Windows QPC, 0.1 us resolution), so a Face copy reads 0.000 at best, and its mean over
1,000 copies is mostly the clock's own overhead.

### Sizes

Engine: upstream (#3787). Host: 64-bit pointers.

| Type | sizeof (B) |
|---|---:|
| ot::GlyphInfo | 20 |
| ot::GlyphPosition | 20 |
| ot::Buffer | 1296 |
| ot::Face | 224 |
| ot::Plan | 376 |
| ot::PlannedLookup | 8 |
| ot::Stage | 4 |

### Faces

Face::init best of 100, a Face copy best of 1,000 (and its mean over the 1,000), steady_clock microseconds.

| Font | CPac (B) | init with CPac (us) | init without CPac (us) | copy best (us) | copy mean (us) |
|---|---:|---:|---:|---:|---:|
| NotoSansDevanagari-Regular.layout | 16044 | 2.10 | 0.10 | 0.000 | 0.0563 |
| NotoSansBengali-Regular.layout | 25056 | 2.80 | 0.10 | 0.000 | 0.0510 |
| NotoSansTamil-Regular.layout | 892 | 0.20 | 0.10 | 0.000 | 0.0507 |
| NotoSansSinhala-Regular.layout | 3056 | 0.60 | 0.00 | 0.000 | 0.0386 |
| NotoSansDevanagari-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0537 |
| NotoSansDevanagari-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0498 |
| NotoSerifDevanagari-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0744 |
| NotoSerifDevanagari-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0531 |
| NotoSansBengali-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0538 |
| NotoSansBengali-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0659 |
| NotoSerifBengali-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0525 |
| NotoSerifBengali-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0531 |
| NotoSansGurmukhi-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0530 |
| NotoSansGurmukhi-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0559 |
| NotoSerifGurmukhi-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0547 |
| NotoSerifGurmukhi-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0550 |
| NotoSansGujarati-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0665 |
| NotoSansGujarati-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0514 |
| NotoSerifGujarati-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0502 |
| NotoSerifGujarati-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0515 |
| NotoSansOriya-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0678 |
| NotoSansOriya-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0533 |
| NotoSerifOriya-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0526 |
| NotoSerifOriya-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0529 |
| NotoSansTamil-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0528 |
| NotoSansTamil-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0499 |
| NotoSerifTamil-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0494 |
| NotoSerifTamil-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0495 |
| NotoSansTelugu-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0490 |
| NotoSansTelugu-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0492 |
| NotoSerifTelugu-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0481 |
| NotoSerifTelugu-Bold.ttf | 0 | n/a | 0.10 | 0.000 | 0.0496 |
| NotoSansKannada-Regular.ttf | 0 | n/a | 0.10 | 0.000 | 0.0481 |
| NotoSansKannada-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0418 |
| NotoSerifKannada-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0423 |
| NotoSerifKannada-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0596 |
| NotoSansMalayalam-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0407 |
| NotoSansMalayalam-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0412 |
| NotoSerifMalayalam-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0415 |
| NotoSerifMalayalam-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0407 |
| NotoSansSinhala-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0414 |
| NotoSansSinhala-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0414 |
| NotoSerifSinhala-Regular.ttf | 0 | n/a | 0.00 | 0.000 | 0.0412 |
| NotoSerifSinhala-Bold.ttf | 0 | n/a | 0.00 | 0.000 | 0.0415 |

### Plans

Plan::build, no language. `computed`: CPac and CPpl dropped (the TrueType route); `stored`: CPpl dropped, CPac kept. Heap peak above the live bytes before the call; allocations of the first build (throwing ones in brackets); time best of 20, microseconds.

| Font | Script | Route | build | GSUB lookups | GPOS lookups | GSUB stages | GPOS stages | memoryBytes | heap peak (B) | allocations | time (us) |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| NotoSansDevanagari-Regular.layout | devanagari | computed | ok | 48 | 21 | 17 | 1 | 13708 | 15997 | 25 (25) | 36.0 |
| NotoSansDevanagari-Regular.layout | devanagari | stored | ok | 48 | 21 | 17 | 1 | 1000 | 3289 | 19 (19) | 8.0 |
| NotoSansBengali-Regular.layout | bengali | computed | ok | 63 | 47 | 17 | 1 | 18496 | 20785 | 25 (25) | 46.6 |
| NotoSansBengali-Regular.layout | bengali | stored | ok | 63 | 47 | 17 | 1 | 1328 | 3617 | 19 (19) | 8.6 |
| NotoSansTamil-Regular.layout | tamil | computed | ok | 5 | 5 | 17 | 1 | 1144 | 3433 | 25 (25) | 7.2 |
| NotoSansTamil-Regular.layout | tamil | stored | ok | 5 | 5 | 17 | 1 | 528 | 2817 | 19 (19) | 4.9 |
| NotoSansSinhala-Regular.layout | sinhala | computed | ok | 16 | 13 | 10 | 1 | 2496 | 4829 | 24 (24) | 9.9 |
| NotoSansSinhala-Regular.layout | sinhala | stored | ok | 16 | 13 | 10 | 1 | 652 | 2985 | 18 (18) | 5.9 |
| NotoSansDevanagari-Regular.ttf | devanagari | computed | ok | 53 | 24 | 17 | 1 | 14260 | 16549 | 25 (25) | 52.8 |
| NotoSansDevanagari-Bold.ttf | devanagari | computed | ok | 53 | 24 | 17 | 1 | 14260 | 16549 | 25 (25) | 58.1 |
| NotoSerifDevanagari-Regular.ttf | devanagari | computed | ok | 55 | 27 | 17 | 1 | 9184 | 11473 | 25 (25) | 43.6 |
| NotoSerifDevanagari-Bold.ttf | devanagari | computed | ok | 55 | 24 | 17 | 1 | 8956 | 11245 | 25 (25) | 35.8 |
| NotoSansBengali-Regular.ttf | bengali | computed | ok | 65 | 143 | 17 | 1 | 24448 | 26737 | 25 (25) | 71.7 |
| NotoSansBengali-Bold.ttf | bengali | computed | ok | 65 | 143 | 17 | 1 | 24448 | 26737 | 25 (25) | 73.1 |
| NotoSerifBengali-Regular.ttf | bengali | computed | ok | 30 | 21 | 17 | 1 | 3820 | 6109 | 25 (25) | 28.8 |
| NotoSerifBengali-Bold.ttf | bengali | computed | ok | 30 | 21 | 17 | 1 | 3820 | 6109 | 25 (25) | 27.7 |
| NotoSansGurmukhi-Regular.ttf | gurmukhi | computed | ok | 9 | 3 | 17 | 1 | 1432 | 3721 | 25 (25) | 14.9 |
| NotoSansGurmukhi-Bold.ttf | gurmukhi | computed | ok | 9 | 3 | 17 | 1 | 1432 | 3721 | 25 (25) | 14.1 |
| NotoSerifGurmukhi-Regular.ttf | gurmukhi | computed | ok | 7 | 6 | 17 | 1 | 1252 | 3541 | 25 (25) | 12.6 |
| NotoSerifGurmukhi-Bold.ttf | gurmukhi | computed | ok | 7 | 6 | 17 | 1 | 1252 | 3541 | 25 (25) | 12.3 |
| NotoSansGujarati-Regular.ttf | gujarati | computed | ok | 27 | 8 | 17 | 1 | 2860 | 5149 | 25 (25) | 22.3 |
| NotoSansGujarati-Bold.ttf | gujarati | computed | ok | 27 | 8 | 17 | 1 | 2860 | 5149 | 25 (25) | 15.5 |
| NotoSerifGujarati-Regular.ttf | gujarati | computed | ok | 34 | 11 | 17 | 1 | 4708 | 6997 | 25 (25) | 19.7 |
| NotoSerifGujarati-Bold.ttf | gujarati | computed | ok | 34 | 11 | 17 | 1 | 4708 | 6997 | 25 (25) | 19.1 |
| NotoSansOriya-Regular.ttf | oriya | computed | ok | 25 | 19 | 17 | 1 | 4432 | 6721 | 25 (25) | 16.7 |
| NotoSansOriya-Bold.ttf | oriya | computed | ok | 25 | 19 | 17 | 1 | 4432 | 6721 | 25 (25) | 16.7 |
| NotoSerifOriya-Regular.ttf | oriya | computed | ok | 18 | 12 | 17 | 1 | 3064 | 5353 | 25 (25) | 20.4 |
| NotoSerifOriya-Bold.ttf | oriya | computed | ok | 18 | 12 | 17 | 1 | 3064 | 5353 | 25 (25) | 20.1 |
| NotoSansTamil-Regular.ttf | tamil | computed | ok | 6 | 5 | 17 | 1 | 1204 | 3493 | 25 (25) | 8.1 |
| NotoSansTamil-Bold.ttf | tamil | computed | ok | 6 | 5 | 17 | 1 | 1204 | 3493 | 25 (25) | 8.1 |
| NotoSerifTamil-Regular.ttf | tamil | computed | ok | 5 | 6 | 17 | 1 | 1204 | 3493 | 25 (25) | 9.2 |
| NotoSerifTamil-Bold.ttf | tamil | computed | ok | 5 | 6 | 17 | 1 | 1204 | 3493 | 25 (25) | 9.3 |
| NotoSansTelugu-Regular.ttf | telugu | computed | ok | 23 | 12 | 17 | 1 | 2740 | 5029 | 25 (25) | 16.2 |
| NotoSansTelugu-Bold.ttf | telugu | computed | ok | 23 | 12 | 17 | 1 | 2740 | 5029 | 25 (25) | 16.2 |
| NotoSerifTelugu-Regular.ttf | telugu | computed | ok | 23 | 11 | 17 | 1 | 2632 | 4921 | 25 (25) | 14.7 |
| NotoSerifTelugu-Bold.ttf | telugu | computed | ok | 23 | 11 | 17 | 1 | 2632 | 4921 | 25 (25) | 14.9 |
| NotoSansKannada-Regular.ttf | kannada | computed | ok | 29 | 7 | 17 | 1 | 3064 | 5353 | 25 (25) | 14.3 |
| NotoSansKannada-Bold.ttf | kannada | computed | ok | 29 | 7 | 17 | 1 | 3064 | 5353 | 25 (25) | 14.0 |
| NotoSerifKannada-Regular.ttf | kannada | computed | ok | 26 | 10 | 17 | 1 | 2896 | 5185 | 25 (25) | 14.0 |
| NotoSerifKannada-Bold.ttf | kannada | computed | ok | 26 | 8 | 17 | 1 | 2728 | 5017 | 25 (25) | 11.2 |
| NotoSansMalayalam-Regular.ttf | malayalam | computed | ok | 19 | 8 | 17 | 1 | 2148 | 4437 | 25 (25) | 10.0 |
| NotoSansMalayalam-Bold.ttf | malayalam | computed | ok | 19 | 8 | 17 | 1 | 2148 | 4437 | 25 (25) | 9.5 |
| NotoSerifMalayalam-Regular.ttf | malayalam | computed | ok | 19 | 9 | 17 | 1 | 2208 | 4497 | 25 (25) | 9.7 |
| NotoSerifMalayalam-Bold.ttf | malayalam | computed | ok | 19 | 9 | 17 | 1 | 2208 | 4497 | 25 (25) | 9.8 |
| NotoSansSinhala-Regular.ttf | sinhala | computed | ok | 16 | 13 | 10 | 1 | 2496 | 4829 | 24 (24) | 9.4 |
| NotoSansSinhala-Bold.ttf | sinhala | computed | ok | 16 | 13 | 10 | 1 | 2496 | 4829 | 24 (24) | 10.3 |
| NotoSerifSinhala-Regular.ttf | sinhala | computed | ok | 17 | 6 | 10 | 1 | 1800 | 4133 | 24 (24) | 11.7 |
| NotoSerifSinhala-Bold.ttf | sinhala | computed | ok | 17 | 6 | 10 | 1 | 1800 | 4133 | 24 (24) | 11.8 |

### Shaping

Route `computed`, no language, Scale::set(2133, 33, upem). Every corpus line shaped once, all with one buffer. Heap peak above the live bytes before the loop; times per line, microseconds.

| Font | Script | lines | max code points | max glyphs | max glyphs - code points | failed, successful false | failed, successful true | allocations | heap peak (B) | mean (us) | max (us) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| NotoSansDevanagari-Regular.layout | devanagari | 20522 | 7 | 10 | 3 | 0 | 0 | 12 | 2328 | 6.05 | 135.2 |
| NotoSansBengali-Regular.layout | bengali | 14852 | 7 | 12 | 5 | 0 | 0 | 12 | 2416 | 8.15 | 76.0 |
| NotoSansTamil-Regular.layout | tamil | 11203 | 7 | 11 | 4 | 0 | 0 | 8 | 2328 | 2.27 | 33.7 |
| NotoSansSinhala-Regular.layout | sinhala | 13157 | 7 | 13 | 6 | 0 | 0 | 16 | 2416 | 2.06 | 52.8 |
| NotoSansDevanagari-Regular.ttf | devanagari | 20522 | 7 | 10 | 3 | 0 | 0 | 12 | 2328 | 5.65 | 117.9 |
| NotoSansDevanagari-Bold.ttf | devanagari | 20522 | 7 | 10 | 3 | 0 | 0 | 12 | 2328 | 5.69 | 92.0 |
| NotoSerifDevanagari-Regular.ttf | devanagari | 20522 | 7 | 10 | 3 | 0 | 0 | 12 | 2328 | 5.13 | 81.6 |
| NotoSerifDevanagari-Bold.ttf | devanagari | 20522 | 7 | 10 | 3 | 0 | 0 | 12 | 2328 | 4.69 | 62.3 |
| NotoSansBengali-Regular.ttf | bengali | 14852 | 7 | 12 | 5 | 0 | 0 | 12 | 2416 | 8.61 | 123.3 |
| NotoSansBengali-Bold.ttf | bengali | 14852 | 7 | 12 | 5 | 0 | 0 | 12 | 2416 | 8.39 | 82.0 |
| NotoSerifBengali-Regular.ttf | bengali | 14852 | 7 | 11 | 4 | 0 | 0 | 12 | 2416 | 4.02 | 47.0 |
| NotoSerifBengali-Bold.ttf | bengali | 14852 | 7 | 11 | 4 | 0 | 0 | 12 | 2416 | 3.66 | 52.0 |
| NotoSansGurmukhi-Regular.ttf | gurmukhi | 14545 | 7 | 10 | 3 | 0 | 0 | 12 | 2372 | 2.32 | 38.0 |
| NotoSansGurmukhi-Bold.ttf | gurmukhi | 14545 | 7 | 10 | 3 | 0 | 0 | 12 | 2372 | 2.37 | 33.1 |
| NotoSerifGurmukhi-Regular.ttf | gurmukhi | 14545 | 7 | 10 | 3 | 0 | 0 | 12 | 2372 | 2.65 | 69.0 |
| NotoSerifGurmukhi-Bold.ttf | gurmukhi | 14545 | 7 | 10 | 3 | 0 | 0 | 12 | 2372 | 2.92 | 52.0 |
| NotoSansGujarati-Regular.ttf | gujarati | 14668 | 7 | 10 | 3 | 0 | 0 | 8 | 2240 | 3.77 | 78.0 |
| NotoSansGujarati-Bold.ttf | gujarati | 14668 | 7 | 10 | 3 | 0 | 0 | 8 | 2240 | 4.11 | 61.3 |
| NotoSerifGujarati-Regular.ttf | gujarati | 14668 | 7 | 10 | 3 | 0 | 0 | 8 | 2240 | 4.29 | 51.1 |
| NotoSerifGujarati-Bold.ttf | gujarati | 14668 | 7 | 10 | 3 | 0 | 0 | 8 | 2240 | 3.64 | 38.4 |
| NotoSansOriya-Regular.ttf | oriya | 14891 | 7 | 11 | 4 | 0 | 0 | 8 | 2416 | 4.60 | 42.4 |
| NotoSansOriya-Bold.ttf | oriya | 14891 | 7 | 11 | 4 | 0 | 0 | 8 | 2416 | 4.44 | 69.7 |
| NotoSerifOriya-Regular.ttf | oriya | 14891 | 7 | 11 | 4 | 0 | 0 | 8 | 2416 | 4.66 | 56.9 |
| NotoSerifOriya-Bold.ttf | oriya | 14891 | 7 | 11 | 4 | 0 | 0 | 8 | 2416 | 4.65 | 46.0 |
| NotoSansTamil-Regular.ttf | tamil | 11203 | 7 | 11 | 4 | 0 | 0 | 8 | 2328 | 2.28 | 34.3 |
| NotoSansTamil-Bold.ttf | tamil | 11203 | 7 | 11 | 4 | 0 | 0 | 8 | 2328 | 2.49 | 45.8 |
| NotoSerifTamil-Regular.ttf | tamil | 11203 | 7 | 11 | 4 | 0 | 0 | 8 | 2328 | 3.06 | 49.8 |
| NotoSerifTamil-Bold.ttf | tamil | 11203 | 7 | 11 | 4 | 0 | 0 | 8 | 2328 | 3.20 | 67.9 |
| NotoSansTelugu-Regular.ttf | telugu | 15680 | 7 | 11 | 4 | 0 | 0 | 8 | 2416 | 4.40 | 50.5 |
| NotoSansTelugu-Bold.ttf | telugu | 15680 | 7 | 11 | 4 | 0 | 0 | 8 | 2416 | 4.46 | 64.6 |
| NotoSerifTelugu-Regular.ttf | telugu | 15680 | 7 | 11 | 4 | 0 | 0 | 8 | 2416 | 4.38 | 58.7 |
| NotoSerifTelugu-Bold.ttf | telugu | 15680 | 7 | 11 | 4 | 0 | 0 | 8 | 2416 | 4.19 | 44.4 |
| NotoSansKannada-Regular.ttf | kannada | 14916 | 7 | 12 | 6 | 0 | 0 | 12 | 2416 | 4.19 | 49.5 |
| NotoSansKannada-Bold.ttf | kannada | 14916 | 7 | 12 | 6 | 0 | 0 | 12 | 2416 | 4.50 | 52.6 |
| NotoSerifKannada-Regular.ttf | kannada | 14916 | 7 | 12 | 6 | 0 | 0 | 12 | 2416 | 4.70 | 71.5 |
| NotoSerifKannada-Bold.ttf | kannada | 14916 | 7 | 12 | 6 | 0 | 0 | 12 | 2416 | 3.78 | 43.9 |
| NotoSansMalayalam-Regular.ttf | malayalam | 17630 | 7 | 12 | 5 | 0 | 0 | 12 | 2416 | 2.58 | 41.6 |
| NotoSansMalayalam-Bold.ttf | malayalam | 17630 | 7 | 12 | 5 | 0 | 0 | 12 | 2416 | 2.73 | 38.1 |
| NotoSerifMalayalam-Regular.ttf | malayalam | 17630 | 7 | 12 | 5 | 0 | 0 | 12 | 2416 | 2.75 | 61.3 |
| NotoSerifMalayalam-Bold.ttf | malayalam | 17630 | 7 | 12 | 5 | 0 | 0 | 12 | 2416 | 2.67 | 60.3 |
| NotoSansSinhala-Regular.ttf | sinhala | 13157 | 7 | 13 | 6 | 0 | 0 | 16 | 2416 | 2.22 | 68.5 |
| NotoSansSinhala-Bold.ttf | sinhala | 13157 | 7 | 13 | 6 | 0 | 0 | 16 | 2416 | 2.24 | 85.4 |
| NotoSerifSinhala-Regular.ttf | sinhala | 13157 | 7 | 12 | 6 | 0 | 0 | 16 | 2416 | 3.14 | 106.2 |
| NotoSerifSinhala-Bold.ttf | sinhala | 13157 | 7 | 12 | 6 | 0 | 0 | 16 | 2416 | 3.12 | 71.9 |

**C3 sizes** (`tools/sizeof_c3.cpp`, `riscv32-esp-elf-g++ -std=gnu++2a -Os -fno-exceptions -fsyntax-only`): seven
errors, one per assert, each "the comparison reduces to '(N == 0)'".

| Type | Host (B) | C3 (B) | Design's figure |
|---|---:|---:|---|
| `ot::GlyphInfo` | 20 | 20 | 20 |
| `ot::GlyphPosition` | 20 | 20 | 20 |
| a slot (2 `GlyphInfo` + `GlyphPosition` + `u16`) | 62 | 62 | 62 |
| `ot::Buffer` | 1,296 | 1,248 | its `matchStack` 1,152 (9 x 64 x 2 B, inside the 1,248) |
| `ot::Face` | 224 | 120 | about 120 |
| `ot::Plan` | 376 | 232 | (none) |
| `ot::PlannedLookup` | 8 | 8 | 8 a planned lookup |
| `ot::Stage` | 4 | 4 | (none) |

**What the probe shows.**
- Every `computed` build is ok (44 of 44), and `stored` is ok on the four fixtures.
- No line failed, either way (both columns 0 on all 44 fonts): the 2n + 16 bound held on the corpus. The most glyphs
  out of one line was 13 (Noto Sans Sinhala), and the largest growth over a line's code points 6 glyphs (Kannada,
  Sinhala), far inside the n + 16 the bound leaves.
- A plan's heap peak during `Plan::build` is `memoryBytes()` plus a constant: the planner's scratch, 2,289 B on the
  host for the Indic shaper and 2,333 B for USE (Sinhala). For example, Noto Sans Devanagari's 16,549 B peak =
  14,260 + 2,289.
  - On the 40 TTFs, the TrueType route (`computed`) peaks between 3,493 B (Noto Sans and Serif Tamil) and 26,737 B
    (Noto Sans Bengali, 65 GSUB and 143 GPOS lookups planned). It makes 25 allocations (24 for USE), all of them
    throwing (`std::vector`).
  - With `CPac` kept (`stored`), the fixtures' plans hold 528-1,328 B (host) and peak at 2,817-3,617 B, in 19
    allocations (18 for USE).
  - On the C3 the same plan is 144 B smaller (`sizeof(Plan)` 232 against 376; the lists' element sizes are equal), for
    example 856 B for the Devanagari fixture with `CPac`, and 1,184 B for Bengali. That is derived from the sizes, not
    measured on the device.
- The runtime filters `CPac` avoids, measured as `computed` minus `stored` `memoryBytes()` on the fixtures: 12,708 B
  (Devanagari), 17,168 B (Bengali), 616 B (Tamil), 1,844 B (Sinhala). The design's upper bounds were 17.5 KB, 27.1 KB,
  1.0 KB and 3.5 KB.
- `Face::init` validates `CPac` by walking every lookup: 2.1 us (Devanagari fixture) and 2.8 us (Bengali) on the host,
  against 0.1 us or less without it. A `Face` copy (deviation 12's re-point) is below the clock's resolution.
- The shaping loop: upstream's buffer reserves as the longest line so far grows, 4 allocations each time (8-16 per
  font), with a heap peak of 2,240-2,416 B for lines of at most 7 code points. Per line, 2.1-8.6 us mean and up to
  135 us at worst on the host.

**The oracle** (`tools/oracle.sh bin/ot_shape_cli_upstream.exe logs/oracle-upstream.log`): one `compare_harfbuzz.py` run
per font, size and language. The fonts: the four fixtures and the 40 Indic TTFs. The sizes: `--scale 2133 --ppem 33`
(16 pt at 150 dpi) and `--scale 1600 --ppem 25` (12 pt). The languages: none and the script's own (Devanagari `hi`,
`mr`, `ne`, `sa`; Bengali `bn`, `as`; then `pa`, `gu`, `or`, `ta`, `te`, `kn`, `ml`, `si`). One row per font and size:

| Font | Size (scale/ppem) | Languages | Lines | Identical |
|---|---|---|---:|---|
| NotoSansDevanagari-Regular.layout | 2133/33 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSansDevanagari-Regular.layout | 1600/25 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSansDevanagari-Regular.ttf | 2133/33 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSansDevanagari-Regular.ttf | 1600/25 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSansDevanagari-Bold.ttf | 2133/33 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSansDevanagari-Bold.ttf | 1600/25 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSerifDevanagari-Regular.ttf | 2133/33 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSerifDevanagari-Regular.ttf | 1600/25 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSerifDevanagari-Bold.ttf | 2133/33 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSerifDevanagari-Bold.ttf | 1600/25 | none, hi, mr, ne, sa | 20,522 | 20,522 in each of 5 |
| NotoSansBengali-Regular.layout | 2133/33 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSansBengali-Regular.layout | 1600/25 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSansBengali-Regular.ttf | 2133/33 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSansBengali-Regular.ttf | 1600/25 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSansBengali-Bold.ttf | 2133/33 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSansBengali-Bold.ttf | 1600/25 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSerifBengali-Regular.ttf | 2133/33 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSerifBengali-Regular.ttf | 1600/25 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSerifBengali-Bold.ttf | 2133/33 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSerifBengali-Bold.ttf | 1600/25 | none, bn, as | 14,852 | 14,852 in each of 3 |
| NotoSansGurmukhi-Regular.ttf | 2133/33 | none, pa | 14,545 | 14,545 in each of 2 |
| NotoSansGurmukhi-Regular.ttf | 1600/25 | none, pa | 14,545 | 14,545 in each of 2 |
| NotoSansGurmukhi-Bold.ttf | 2133/33 | none, pa | 14,545 | 14,545 in each of 2 |
| NotoSansGurmukhi-Bold.ttf | 1600/25 | none, pa | 14,545 | 14,545 in each of 2 |
| NotoSerifGurmukhi-Regular.ttf | 2133/33 | none, pa | 14,545 | 14,545 in each of 2 |
| NotoSerifGurmukhi-Regular.ttf | 1600/25 | none, pa | 14,545 | 14,545 in each of 2 |
| NotoSerifGurmukhi-Bold.ttf | 2133/33 | none, pa | 14,545 | 14,545 in each of 2 |
| NotoSerifGurmukhi-Bold.ttf | 1600/25 | none, pa | 14,545 | 14,545 in each of 2 |
| NotoSansGujarati-Regular.ttf | 2133/33 | none, gu | 14,668 | 14,668 in each of 2 |
| NotoSansGujarati-Regular.ttf | 1600/25 | none, gu | 14,668 | 14,668 in each of 2 |
| NotoSansGujarati-Bold.ttf | 2133/33 | none, gu | 14,668 | 14,668 in each of 2 |
| NotoSansGujarati-Bold.ttf | 1600/25 | none, gu | 14,668 | 14,668 in each of 2 |
| NotoSerifGujarati-Regular.ttf | 2133/33 | none, gu | 14,668 | 14,668 in each of 2 |
| NotoSerifGujarati-Regular.ttf | 1600/25 | none, gu | 14,668 | 14,668 in each of 2 |
| NotoSerifGujarati-Bold.ttf | 2133/33 | none, gu | 14,668 | 14,668 in each of 2 |
| NotoSerifGujarati-Bold.ttf | 1600/25 | none, gu | 14,668 | 14,668 in each of 2 |
| NotoSansOriya-Regular.ttf | 2133/33 | none, or | 14,891 | 14,891 in each of 2 |
| NotoSansOriya-Regular.ttf | 1600/25 | none, or | 14,891 | 14,891 in each of 2 |
| NotoSansOriya-Bold.ttf | 2133/33 | none, or | 14,891 | 14,891 in each of 2 |
| NotoSansOriya-Bold.ttf | 1600/25 | none, or | 14,891 | 14,891 in each of 2 |
| NotoSerifOriya-Regular.ttf | 2133/33 | none, or | 14,891 | 14,891 in each of 2 |
| NotoSerifOriya-Regular.ttf | 1600/25 | none, or | 14,891 | 14,891 in each of 2 |
| NotoSerifOriya-Bold.ttf | 2133/33 | none, or | 14,891 | 14,891 in each of 2 |
| NotoSerifOriya-Bold.ttf | 1600/25 | none, or | 14,891 | 14,891 in each of 2 |
| NotoSansTamil-Regular.layout | 2133/33 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSansTamil-Regular.layout | 1600/25 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSansTamil-Regular.ttf | 2133/33 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSansTamil-Regular.ttf | 1600/25 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSansTamil-Bold.ttf | 2133/33 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSansTamil-Bold.ttf | 1600/25 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSerifTamil-Regular.ttf | 2133/33 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSerifTamil-Regular.ttf | 1600/25 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSerifTamil-Bold.ttf | 2133/33 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSerifTamil-Bold.ttf | 1600/25 | none, ta | 11,203 | 11,203 in each of 2 |
| NotoSansTelugu-Regular.ttf | 2133/33 | none, te | 15,680 | 15,680 in each of 2 |
| NotoSansTelugu-Regular.ttf | 1600/25 | none, te | 15,680 | 15,680 in each of 2 |
| NotoSansTelugu-Bold.ttf | 2133/33 | none, te | 15,680 | 15,680 in each of 2 |
| NotoSansTelugu-Bold.ttf | 1600/25 | none, te | 15,680 | 15,680 in each of 2 |
| NotoSerifTelugu-Regular.ttf | 2133/33 | none, te | 15,680 | 15,680 in each of 2 |
| NotoSerifTelugu-Regular.ttf | 1600/25 | none, te | 15,680 | 15,680 in each of 2 |
| NotoSerifTelugu-Bold.ttf | 2133/33 | none, te | 15,680 | 15,680 in each of 2 |
| NotoSerifTelugu-Bold.ttf | 1600/25 | none, te | 15,680 | 15,680 in each of 2 |
| NotoSansKannada-Regular.ttf | 2133/33 | none, kn | 14,916 | 14,916 in each of 2 |
| NotoSansKannada-Regular.ttf | 1600/25 | none, kn | 14,916 | 14,916 in each of 2 |
| NotoSansKannada-Bold.ttf | 2133/33 | none, kn | 14,916 | 14,916 in each of 2 |
| NotoSansKannada-Bold.ttf | 1600/25 | none, kn | 14,916 | 14,916 in each of 2 |
| NotoSerifKannada-Regular.ttf | 2133/33 | none, kn | 14,916 | 14,916 in each of 2 |
| NotoSerifKannada-Regular.ttf | 1600/25 | none, kn | 14,916 | 14,916 in each of 2 |
| NotoSerifKannada-Bold.ttf | 2133/33 | none, kn | 14,916 | 14,916 in each of 2 |
| NotoSerifKannada-Bold.ttf | 1600/25 | none, kn | 14,916 | 14,916 in each of 2 |
| NotoSansMalayalam-Regular.ttf | 2133/33 | none, ml | 17,630 | 17,630 in each of 2 |
| NotoSansMalayalam-Regular.ttf | 1600/25 | none, ml | 17,630 | 17,630 in each of 2 |
| NotoSansMalayalam-Bold.ttf | 2133/33 | none, ml | 17,630 | 17,630 in each of 2 |
| NotoSansMalayalam-Bold.ttf | 1600/25 | none, ml | 17,630 | 17,630 in each of 2 |
| NotoSerifMalayalam-Regular.ttf | 2133/33 | none, ml | 17,630 | 17,630 in each of 2 |
| NotoSerifMalayalam-Regular.ttf | 1600/25 | none, ml | 17,630 | 17,630 in each of 2 |
| NotoSerifMalayalam-Bold.ttf | 2133/33 | none, ml | 17,630 | 17,630 in each of 2 |
| NotoSerifMalayalam-Bold.ttf | 1600/25 | none, ml | 17,630 | 17,630 in each of 2 |
| NotoSansSinhala-Regular.layout | 2133/33 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSansSinhala-Regular.layout | 1600/25 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSansSinhala-Regular.ttf | 2133/33 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSansSinhala-Regular.ttf | 1600/25 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSansSinhala-Bold.ttf | 2133/33 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSansSinhala-Bold.ttf | 1600/25 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSerifSinhala-Regular.ttf | 2133/33 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSerifSinhala-Regular.ttf | 1600/25 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSerifSinhala-Bold.ttf | 2133/33 | none, si | 13,157 | 13,157 in each of 2 |
| NotoSerifSinhala-Bold.ttf | 1600/25 | none, si | 13,157 | 13,157 in each of 2 |

216/216 runs identical.

216 runs, 3,436,140 lines shaped by both engines, 0 mismatches. The script exited 0.

**Verdict: item 1 passes.** Every oracle run reports N/N: upstream's engine gives HarfBuzz 14.5.0's glyphs, advances
and offsets on upstream's corpus under our compiler and flags.

## Item 2: the engine's flash in `default`

**How.** A detached worktree of master (`55589ef59`, SDK `5bd4a0a`), `C:/tmp/wt-wm2-scratch`, built with
`PLATFORMIO_CORE_DIR=C:\pio`, no other pio or scons process running.
- The baseline: master as it is, `default`, then `gh_release`.
- The call: upstream's `lib/OtShaper` (25 files) and `lib/Utf8/IndicScripts.h`, `IndicReorder.h`, `IndicReorder.cpp`
  copied in (from `pr-3787`, byte-exact), and `src/OtShaperFlashProbe.cpp` (a copy is kept at
  `C:/tmp/wm2-measure/tools/OtShaperFlashProbe.cpp`). Its inputs are `volatile` globals (a run flag, a font pointer
  and length, the script, the language string); an `__attribute__((constructor))` function returns unless the flag is
  set, and else calls `ot::tablesFromSfnt`, `Face::init`, `Plan::build` (not `Plan::load`), `Scale::set`, `ot::shape`
  on two code points into a local `ot::Buffer`, and `indicReorderForDisplay`. Nothing sets the flag.
- PlatformIO's dependency finder took `lib/OtShaper` in as a library of its own (`|-- OtShaper` in the graph). Every
  engine file and `IndicReorder.cpp` (against our `Utf8.h`) also compile clean with `riscv32-esp-elf-g++ -std=gnu++2a
  -Os -fno-exceptions -Wall`, checked before the build.

**The sizes** (the `Flash:` and `RAM:` lines' bytes used):

| Env | Flash, master | Flash, with the call | Delta | RAM, master | RAM, with the call | Delta |
|---|---:|---:|---:|---:|---:|---:|
| `default` | 6,161,905 | 6,214,173 | **+52,268** | 57,776 | 57,800 | +24 |
| `gh_release` | 6,039,675 | 6,091,675 | +52,000 | 57,272 | 57,296 | +24 |

The master figures are the baseline Task 10 checks PR 1 against (PR 1 links nothing, so its sizes must equal them).
They are `default` 11,310 B above step 1 PR 1's base (6,150,595), and `gh_release` 1,674 B above it (6,038,001); RAM
is unchanged on `default` and `gh_release` (57,776 and 57,272).

What the call links, from `.pio/build/default/firmware.map` (input sections per object; `gh_release`'s are the same):

| Object | text | rodata | data | bss |
|---|---:|---:|---:|---:|
| OtBuffer.cpp.o | 1,590 | 28 | 0 | 4 |
| OtContext.cpp.o | 3,272 | 0 | 0 | 0 |
| OtFace.cpp.o | 2,614 | 72 | 0 | 0 |
| OtGpos.cpp.o | 4,524 | 4 | 0 | 0 |
| OtGsub.cpp.o | 2,752 | 26 | 0 | 0 |
| OtIndic.cpp.o | 4,144 | 2,439 | 0 | 0 |
| OtLayout.cpp.o | 3,146 | 26 | 0 | 0 |
| OtNormalize.cpp.o | 2,464 | 2,256 | 0 | 0 |
| OtPlan.cpp.o | 2,084 | 334 | 0 | 0 |
| OtPlanBuild.cpp.o | 7,150 | 328 | 0 | 0 |
| OtShaper.cpp.o | 3,016 | 751 | 0 | 0 |
| OtTable.cpp.o | 518 | 0 | 0 | 0 |
| OtUse.cpp.o | 1,492 | 1,093 | 0 | 0 |
| IndicReorder.cpp.o | 1,354 | 1,817 | 0 | 0 |
| OtShaperFlashProbe.cpp.o | 748 | 1 | 4 | 14 |
| **total** | 40,868 | 9,175 | 4 | 18 |

The objects hold 50,047 B of the 52,268 B; the other 2,221 B are what they pull in from libstdc++ and newlib, and
alignment. The runtime planner, `OtPlanBuild.cpp`, is the largest single object at 7,478 B.

**Evaluation.**
- E, the engine's cost on `default`: **52,268 B (51.0 KB)**.
- A, the allowance for PRs 2-3's glue, the `CPac` writer and the glyph-run draw: 15,360 B.
- P2, step 1 PR 2's `default` delta: no figure exists yet. Step 1 PR 2 (`feat/writing-modes-step1-pr2`, worktree
  `C:/tmp/wt-wm1`) has no PR, and its ledger is at Task 4 of 12; its plan records `Flash:` only in its final task.
  So P2 = 20,480 B, and the free-slot figure is **provisional**: PR 3 measures it for real.
- Pass line 1: E + A = 52,268 + 15,360 = **67,628 B <= 92,160 B**, 24,532 B to spare: **pass**.
- Pass line 2: 6,553,600 - (6,214,173 + 15,360 + 20,480) = **303,587 B >= 256,000 B**, 47,587 B to spare: **pass
  (provisional)**. The app slot is 0x640000 B (`partitions.csv`, `app0`).
- E against the design's estimate of 55-75 KB for PRs 1-3: the engine alone is 51.0 KB, under the estimate's low end;
  E + A is 66.0 KB, inside it.
- Static RAM: +24 B on both envs (upstream measured +224 B for its route). The 18 B of `.bss` and 4 B of `.data`
  above are the engine's heap-check pointer and the probe's own volatile inputs.

**Verdict: item 2 passes** (line 2 provisional on P2).

## Item 3: the merged fonts of decision 6

**Decision 6, refined by the maintainer (2026-11-02).** The first run of this item, recorded below as "The first
recipe (superseded)", missed: two families did not merge, and the Latin line check could not hold. The maintainer then
added two rules to decision 6, and the plan's Task 3 now carries them:
- the merge drops `vhea` and `vmtx`, since our engine never sets Indic text vertically;
- Latin letters A-Z and a-z always come from Noto Sans or Noto Serif, so Latin looks the same in every family.
  Space, digits, punctuation and everything else the script font maps come from the script font. Noto Sans Sinhala's
  own Latin letters are therefore replaced by Noto Sans's.

The Latin check is redefined to match: each Latin line, restricted to Latin letters, must shape identically in the
merged family and in Noto Sans or Serif itself. The check against the script's own font stays.

### The refined recipe

**How.** `tools/merge_families.py dl/fonts merged corpus logs/fonts.md` (rewritten for the refined recipe; the first
recipe's script is kept as `tools/merge_families_first_recipe.py`). For each of the 22 families, regular with regular
and bold with bold:
- the script font: A-Z and a-z leave every cmap subtable. Its glyphs stay, so its glyph IDs and its own shaping do not
  change. Only Noto Sans Sinhala maps any letters (52); the other 42 files go into the merge as they are;
- Noto's Latin: the `fontTools.subset` of the first recipe, less every code point the script font maps other than
  A-Z and a-z;
- upem: all 1,000, so nothing was scaled;
- the merge: `fontTools.merge.Merger(options=Options(drop_tables=["vhea", "vmtx"])).merge([script, latin])`, the
  script font first;
- the sizes, the `.sizes` files and `CPac`, counted as before.

The checks:
- **Equal against the script's own font:** as in the first recipe. HarfBuzz 14.5.0 at 2133/33, every line of the
  script's corpus, with no language and each of the script's languages; Arabic uses `corpus/arabic.txt`, rtl, `Arab`,
  with none, `ar`, `fa` and `ur`. Glyph IDs, advances and offsets must be equal.
- **Latin kept whole:** each of the three Latin lines is cut into its longest runs of A-Z and a-z, 18 runs. Each run
  is shaped by the merged font and by the Latin font file itself (Noto Sans or Serif, script and direction guessed).
  The glyph names (each merged glyph's name mapped back through the merger's renaming), advances and offsets must be
  equal. A glyph of the script font's part never matches.

**Results.**
- **22 of 22 families merged**, both styles. Noto Serif Bengali and Noto Serif Tamil lost `vhea` and `vmtx`; no other
  script file has them, and no merged file keeps them.
- In every merged style, all 52 letters map to the Latin font's glyphs.
  - In Noto Sans Sinhala they are Noto Sans's (`A.1` and so on in the merged order, since its own `A` keeps the
    name).
  - Its own Latin glyphs stay in the file, out of the cmap: 167,108 B (Regular) against 161,628 B in the first recipe.
- The other 19 families' merged styles (38) have the first recipe's sizes, glyph counts and lookup counts. Noto
  Serif Bengali's and Noto Serif Tamil's are the sizes of the first run's supplementary merge.
- **Equal against the script's own font: 1,531,416 of 1,531,416 lines**, 112 runs (the 22 families, both styles, each
  language).
- **Latin kept whole: 792 of 792 letter runs equal** (18 in each of the 44 styles): the kerning (`AVATAR`'s three `A`s
  take three advances) and the ligatures (`fi`, `fl`, `f_f_i`) are Noto Sans's or Serif's own.
- **The fit:** every family fits the X3's store **in flash mode**, 22 of 22 (`Step2MergedFamiliesProbe` in a fresh
  scratch tree of master `f27a7fb96`, whose only change against `55589ef59` is `docs/serial-test-api.md`). The
  largest is still Noto Serif Devanagari: two styles of 311,580 B and 295,780 B, with a log of 1,376,256 B left.
  `ttf::fitsStore` agreed with `planLayout` for every family.

**One row per family and style:**

| Family | Style | bytes | GDEF | CPac | flash bytes | SD bytes | mode | equal against the script's font | Latin letter runs equal | script font's Latin: letters / 52, U+0020-007E / 95 |
|---|---|---:|---:|---:|---:|---:|---|---|---|---|
| NotoSansDevanagari | Regular | 262,000 | 652 | 18,244 | 280,244 | 125,132 | flash | 20,522 of 20,522 lines x 5 (none, hi, mr, ne, sa) | 18 of 18 | 0 / 39 |
| NotoSansDevanagari | Bold | 261,660 | 674 | 18,244 | 279,904 | 126,180 | flash | 20,522 of 20,522 lines x 5 (none, hi, mr, ne, sa) | 18 of 18 | 0 / 39 |
| NotoSerifDevanagari | Regular | 293,268 | 940 | 18,312 | 311,580 | 134,548 | flash | 20,522 of 20,522 lines x 5 (none, hi, mr, ne, sa) | 18 of 18 | 0 / 39 |
| NotoSerifDevanagari | Bold | 277,700 | 922 | 18,080 | 295,780 | 119,964 | flash | 20,522 of 20,522 lines x 5 (none, hi, mr, ne, sa) | 18 of 18 | 0 / 39 |
| NotoSansBengali | Regular | 189,700 | 1,638 | 37,368 | 227,068 | 106,452 | flash | 14,852 of 14,852 lines x 3 (none, bn, as) | 18 of 18 | 0 / 7 |
| NotoSansBengali | Bold | 188,856 | 1,660 | 37,368 | 226,224 | 107,440 | flash | 14,852 of 14,852 lines x 3 (none, bn, as) | 18 of 18 | 0 / 7 |
| NotoSerifBengali | Regular | 275,992 | 378 | 6,672 | 282,664 | 66,860 | flash | 14,852 of 14,852 lines x 3 (none, bn, as) | 18 of 18 | 0 / 39 |
| NotoSerifBengali | Bold | 275,220 | 360 | 6,672 | 281,892 | 68,572 | flash | 14,852 of 14,852 lines x 3 (none, bn, as) | 18 of 18 | 0 / 39 |
| NotoSansGurmukhi | Regular | 112,068 | 330 | 2,744 | 114,812 | 30,424 | flash | 14,545 of 14,545 lines x 2 (none, pa) | 18 of 18 | 0 / 39 |
| NotoSansGurmukhi | Bold | 112,300 | 352 | 2,744 | 115,044 | 30,668 | flash | 14,545 of 14,545 lines x 2 (none, pa) | 18 of 18 | 0 / 39 |
| NotoSerifGurmukhi | Regular | 119,984 | 370 | 2,092 | 122,076 | 28,624 | flash | 14,545 of 14,545 lines x 2 (none, pa) | 18 of 18 | 0 / 39 |
| NotoSerifGurmukhi | Bold | 119,952 | 352 | 2,092 | 122,044 | 28,612 | flash | 14,545 of 14,545 lines x 2 (none, pa) | 18 of 18 | 0 / 39 |
| NotoSansGujarati | Regular | 233,576 | 990 | 9,372 | 242,948 | 84,444 | flash | 14,668 of 14,668 lines x 2 (none, gu) | 18 of 18 | 0 / 1 |
| NotoSansGujarati | Bold | 232,076 | 1,012 | 9,372 | 241,448 | 84,712 | flash | 14,668 of 14,668 lines x 2 (none, gu) | 18 of 18 | 0 / 1 |
| NotoSerifGujarati | Regular | 184,656 | 638 | 6,748 | 191,404 | 50,024 | flash | 14,668 of 14,668 lines x 2 (none, gu) | 18 of 18 | 0 / 39 |
| NotoSerifGujarati | Bold | 181,952 | 620 | 6,748 | 188,700 | 48,952 | flash | 14,668 of 14,668 lines x 2 (none, gu) | 18 of 18 | 0 / 39 |
| NotoSansOriya | Regular | 208,252 | 698 | 12,876 | 221,128 | 72,132 | flash | 14,891 of 14,891 lines x 2 (none, or) | 18 of 18 | 0 / 39 |
| NotoSansOriya | Bold | 207,608 | 720 | 12,876 | 220,484 | 72,568 | flash | 14,891 of 14,891 lines x 2 (none, or) | 18 of 18 | 0 / 39 |
| NotoSerifOriya | Regular | 234,340 | 528 | 5,640 | 239,980 | 55,092 | flash | 14,891 of 14,891 lines x 2 (none, or) | 18 of 18 | 0 / 39 |
| NotoSerifOriya | Bold | 233,424 | 510 | 5,640 | 239,064 | 54,828 | flash | 14,891 of 14,891 lines x 2 (none, or) | 18 of 18 | 0 / 39 |
| NotoSansTamil | Regular | 124,036 | 356 | 2,424 | 126,460 | 35,116 | flash | 11,203 of 11,203 lines x 2 (none, ta) | 18 of 18 | 0 / 39 |
| NotoSansTamil | Bold | 124,352 | 378 | 2,424 | 126,776 | 35,388 | flash | 11,203 of 11,203 lines x 2 (none, ta) | 18 of 18 | 0 / 39 |
| NotoSerifTamil | Regular | 136,444 | 534 | 2,008 | 138,452 | 34,668 | flash | 11,203 of 11,203 lines x 2 (none, ta) | 18 of 18 | 0 / 39 |
| NotoSerifTamil | Bold | 136,704 | 516 | 2,008 | 138,712 | 34,804 | flash | 11,203 of 11,203 lines x 2 (none, ta) | 18 of 18 | 0 / 39 |
| NotoSansTelugu | Regular | 244,660 | 834 | 7,928 | 252,588 | 67,280 | flash | 15,680 of 15,680 lines x 2 (none, te) | 18 of 18 | 0 / 39 |
| NotoSansTelugu | Bold | 245,460 | 856 | 7,928 | 253,388 | 67,912 | flash | 15,680 of 15,680 lines x 2 (none, te) | 18 of 18 | 0 / 39 |
| NotoSerifTelugu | Regular | 277,972 | 566 | 7,516 | 285,488 | 59,608 | flash | 15,680 of 15,680 lines x 2 (none, te) | 18 of 18 | 0 / 39 |
| NotoSerifTelugu | Bold | 275,948 | 548 | 7,516 | 283,464 | 60,312 | flash | 15,680 of 15,680 lines x 2 (none, te) | 18 of 18 | 0 / 39 |
| NotoSansKannada | Regular | 211,800 | 772 | 12,404 | 224,204 | 79,588 | flash | 14,916 of 14,916 lines x 2 (none, kn) | 18 of 18 | 0 / 39 |
| NotoSansKannada | Bold | 210,656 | 794 | 12,404 | 223,060 | 79,896 | flash | 14,916 of 14,916 lines x 2 (none, kn) | 18 of 18 | 0 / 39 |
| NotoSerifKannada | Regular | 219,364 | 420 | 7,760 | 227,124 | 76,264 | flash | 14,916 of 14,916 lines x 2 (none, kn) | 18 of 18 | 0 / 39 |
| NotoSerifKannada | Bold | 183,900 | 376 | 7,660 | 191,560 | 42,936 | flash | 14,916 of 14,916 lines x 2 (none, kn) | 18 of 18 | 0 / 39 |
| NotoSansMalayalam | Regular | 153,404 | 286 | 5,408 | 158,812 | 43,228 | flash | 17,630 of 17,630 lines x 2 (none, ml) | 18 of 18 | 0 / 39 |
| NotoSansMalayalam | Bold | 152,484 | 308 | 5,408 | 157,892 | 43,600 | flash | 17,630 of 17,630 lines x 2 (none, ml) | 18 of 18 | 0 / 39 |
| NotoSerifMalayalam | Regular | 156,744 | 342 | 5,224 | 161,968 | 39,192 | flash | 17,630 of 17,630 lines x 2 (none, ml) | 18 of 18 | 0 / 39 |
| NotoSerifMalayalam | Bold | 155,548 | 324 | 5,224 | 160,772 | 39,220 | flash | 17,630 of 17,630 lines x 2 (none, ml) | 18 of 18 | 0 / 39 |
| NotoSansSinhala | Regular | 167,108 | 316 | 4,432 | 171,540 | 36,688 | flash | 13,157 of 13,157 lines x 2 (none, si) | 18 of 18 | 52 / 91 |
| NotoSansSinhala | Bold | 165,724 | 338 | 4,432 | 170,156 | 37,052 | flash | 13,157 of 13,157 lines x 2 (none, si) | 18 of 18 | 52 / 91 |
| NotoSerifSinhala | Regular | 279,896 | 576 | 5,292 | 285,188 | 48,912 | flash | 13,157 of 13,157 lines x 2 (none, si) | 18 of 18 | 0 / 39 |
| NotoSerifSinhala | Bold | 275,784 | 558 | 5,292 | 281,076 | 49,004 | flash | 13,157 of 13,157 lines x 2 (none, si) | 18 of 18 | 0 / 39 |
| NotoNaskhArabic | Regular | 252,656 | 652 | 5,084 | 257,740 | 63,264 | flash | 577 of 577 lines x 4 (none, ar, fa, ur) | 18 of 18 | 0 / 15 |
| NotoNaskhArabic | Bold | 252,444 | 634 | 5,084 | 257,528 | 63,328 | flash | 577 of 577 lines x 4 (none, ar, fa, ur) | 18 of 18 | 0 / 15 |
| NotoSansArabic | Regular | 227,748 | 534 | 5,036 | 232,784 | 64,464 | flash | 577 of 577 lines x 4 (none, ar, fa, ur) | 18 of 18 | 0 / 16 |
| NotoSansArabic | Bold | 227,976 | 556 | 5,036 | 233,012 | 64,924 | flash | 577 of 577 lines x 4 (none, ar, fa, ur) | 18 of 18 | 0 / 16 |

**The merges** (lookups, glyph counts, what the recipe changed in the script font):

| Family | Style | Latin | upem (script / Latin) | scaled | dropped from the script font | letters out of the script cmap | script glyphs | Latin subset glyphs (code points) | merged glyphs | letters from the Latin font | merged bytes | GDEF (B) | GSUB lookups / subtables | GPOS lookups / subtables | CPac (B) | result |
|---|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| NotoSansDevanagari | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 845 | 858 (618) | 1,703 | 52 | 262,000 | 652 | 193 / 461 | 32 / 36 | 18,244 | merged |
| NotoSansDevanagari | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 845 | 858 (618) | 1,703 | 52 | 261,660 | 674 | 193 / 461 | 32 / 36 | 18,244 | merged |
| NotoSerifDevanagari | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 997 | 834 (618) | 1,831 | 52 | 293,268 | 940 | 246 / 403 | 32 / 35 | 18,312 | merged |
| NotoSerifDevanagari | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 997 | 834 (618) | 1,831 | 52 | 277,700 | 922 | 246 / 403 | 28 / 30 | 18,080 | merged |
| NotoSansBengali | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 418 | 969 (656) | 1,387 | 52 | 189,700 | 1,638 | 243 / 698 | 251 / 282 | 37,368 | merged |
| NotoSansBengali | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 418 | 969 (656) | 1,387 | 52 | 188,856 | 1,660 | 243 / 698 | 251 / 282 | 37,368 | merged |
| NotoSerifBengali | Regular | NotoSerif | 1000 / 1000 | no | vhea, vmtx | 0 | 714 | 834 (618) | 1,548 | 52 | 275,992 | 378 | 93 / 117 | 23 / 25 | 6,672 | merged |
| NotoSerifBengali | Bold | NotoSerif | 1000 / 1000 | no | vhea, vmtx | 0 | 714 | 834 (618) | 1,548 | 52 | 275,220 | 360 | 93 / 117 | 23 / 25 | 6,672 | merged |
| NotoSansGurmukhi | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 282 | 858 (618) | 1,140 | 52 | 112,068 | 330 | 36 / 37 | 10 / 23 | 2,744 | merged |
| NotoSansGurmukhi | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 282 | 858 (618) | 1,140 | 52 | 112,300 | 352 | 36 / 37 | 10 / 23 | 2,744 | merged |
| NotoSerifGurmukhi | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 294 | 834 (618) | 1,128 | 52 | 119,984 | 370 | 30 / 30 | 9 / 11 | 2,092 | merged |
| NotoSerifGurmukhi | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 294 | 834 (618) | 1,128 | 52 | 119,952 | 352 | 30 / 30 | 9 / 11 | 2,092 | merged |
| NotoSansGujarati | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 780 | 981 (667) | 1,761 | 52 | 233,576 | 990 | 146 / 185 | 15 / 17 | 9,372 | merged |
| NotoSansGujarati | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 780 | 981 (667) | 1,761 | 52 | 232,076 | 1,012 | 146 / 185 | 15 / 17 | 9,372 | merged |
| NotoSerifGujarati | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 456 | 834 (618) | 1,290 | 52 | 184,656 | 638 | 86 / 150 | 13 / 15 | 6,748 | merged |
| NotoSerifGujarati | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 456 | 834 (618) | 1,290 | 52 | 181,952 | 620 | 86 / 150 | 13 / 15 | 6,748 | merged |
| NotoSansOriya | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 627 | 859 (619) | 1,486 | 52 | 208,252 | 698 | 198 / 254 | 23 / 24 | 12,876 | merged |
| NotoSansOriya | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 627 | 859 (619) | 1,486 | 52 | 207,608 | 720 | 198 / 254 | 23 / 24 | 12,876 | merged |
| NotoSerifOriya | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 690 | 835 (619) | 1,525 | 52 | 234,340 | 528 | 78 / 111 | 14 / 16 | 5,640 | merged |
| NotoSerifOriya | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 690 | 835 (619) | 1,525 | 52 | 233,424 | 510 | 78 / 111 | 14 / 16 | 5,640 | merged |
| NotoSansTamil | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 244 | 856 (616) | 1,100 | 52 | 124,036 | 356 | 34 / 38 | 10 / 11 | 2,424 | merged |
| NotoSansTamil | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 244 | 856 (616) | 1,100 | 52 | 124,352 | 378 | 34 / 38 | 10 / 11 | 2,424 | merged |
| NotoSerifTamil | Regular | NotoSerif | 1000 / 1000 | no | vhea, vmtx | 0 | 222 | 834 (616) | 1,056 | 52 | 136,444 | 534 | 28 / 30 | 8 / 11 | 2,008 | merged |
| NotoSerifTamil | Bold | NotoSerif | 1000 / 1000 | no | vhea, vmtx | 0 | 222 | 834 (616) | 1,056 | 52 | 136,704 | 516 | 28 / 30 | 8 / 11 | 2,008 | merged |
| NotoSansTelugu | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 961 | 858 (618) | 1,819 | 52 | 244,660 | 834 | 110 / 118 | 38 / 39 | 7,928 | merged |
| NotoSansTelugu | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 961 | 858 (618) | 1,819 | 52 | 245,460 | 856 | 110 / 118 | 38 / 39 | 7,928 | merged |
| NotoSerifTelugu | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 743 | 834 (618) | 1,577 | 52 | 277,972 | 566 | 106 / 112 | 35 / 36 | 7,516 | merged |
| NotoSerifTelugu | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 743 | 834 (618) | 1,577 | 52 | 275,948 | 548 | 106 / 112 | 35 / 36 | 7,516 | merged |
| NotoSansKannada | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 657 | 858 (618) | 1,515 | 52 | 211,800 | 772 | 214 / 227 | 15 / 22 | 12,404 | merged |
| NotoSansKannada | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 657 | 858 (618) | 1,515 | 52 | 210,656 | 794 | 214 / 227 | 15 / 22 | 12,404 | merged |
| NotoSerifKannada | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 419 | 834 (618) | 1,253 | 52 | 219,364 | 420 | 84 / 90 | 58 / 67 | 7,760 | merged |
| NotoSerifKannada | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 419 | 834 (618) | 1,253 | 52 | 183,900 | 376 | 84 / 90 | 57 / 64 | 7,660 | merged |
| NotoSansMalayalam | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 365 | 858 (618) | 1,223 | 52 | 153,404 | 286 | 86 / 90 | 14 / 18 | 5,408 | merged |
| NotoSansMalayalam | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 365 | 858 (618) | 1,223 | 52 | 152,484 | 308 | 86 / 90 | 14 / 18 | 5,408 | merged |
| NotoSerifMalayalam | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 355 | 834 (618) | 1,189 | 52 | 156,744 | 342 | 82 / 86 | 14 / 19 | 5,224 | merged |
| NotoSerifMalayalam | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 355 | 834 (618) | 1,189 | 52 | 155,548 | 324 | 82 / 86 | 14 / 19 | 5,224 | merged |
| NotoSansSinhala | Regular | NotoSans | 1000 / 1000 | no | none | 52 | 371 | 861 (621) | 1,232 | 52 | 167,108 | 316 | 60 / 74 | 18 / 19 | 4,432 | merged |
| NotoSansSinhala | Bold | NotoSans | 1000 / 1000 | no | none | 52 | 371 | 861 (621) | 1,232 | 52 | 165,724 | 338 | 60 / 74 | 18 / 19 | 4,432 | merged |
| NotoSerifSinhala | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 645 | 836 (620) | 1,481 | 52 | 279,896 | 576 | 92 / 92 | 9 / 10 | 5,292 | merged |
| NotoSerifSinhala | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 645 | 836 (620) | 1,481 | 52 | 275,784 | 558 | 92 / 92 | 9 / 10 | 5,292 | merged |
| NotoNaskhArabic | Regular | NotoSerif | 1000 / 1000 | no | none | 0 | 1,415 | 870 (646) | 2,285 | 52 | 252,656 | 652 | 65 / 77 | 26 / 28 | 5,084 | merged |
| NotoNaskhArabic | Bold | NotoSerif | 1000 / 1000 | no | none | 0 | 1,415 | 870 (646) | 2,285 | 52 | 252,444 | 634 | 65 / 77 | 26 / 28 | 5,084 | merged |
| NotoSansArabic | Regular | NotoSans | 1000 / 1000 | no | none | 0 | 1,399 | 893 (645) | 2,292 | 52 | 227,748 | 534 | 64 / 74 | 27 / 29 | 5,036 | merged |
| NotoSansArabic | Bold | NotoSans | 1000 / 1000 | no | none | 0 | 1,399 | 893 (645) | 2,292 | 52 | 227,976 | 556 | 64 / 74 | 27 / 29 | 5,036 | merged |

**Latin in the script fonts** (the inputs, unchanged by the refinement): #3787's "no Latin" claim holds for letters in
42 of the 44 script files, not for Noto Sans Sinhala (all 52 letters, 91 of the 95 code points U+0020-007E).
- 34 of the 40 Indic files map the same 39 of the 95: space, the digits and the punctuation
  ``!"#%'()*+,-./:;<=>?[\]^_{|}~``.
- Noto Sans Bengali maps 7 (space `! ? [ ] { }`), Noto Sans Gujarati 1 (space), Noto Naskh Arabic 15 and Noto Sans
  Arabic 16.
- Each file's counts are in the last column of the table above.

**A consequence of the second rule, for the design to know:** Latin text in a merged family takes the script font's
word space and punctuation, and their widths differ from Noto's in many families. The space is 260 units in Noto Sans
and Noto Serif. Measured in the Regular files (font units, upem 1,000):
- it is 260 units in 8 of the 22 script fonts;
- in the other 14 it ranges from 221 (Noto Naskh Arabic) to 500 (Noto Serif Sinhala), with 390 in Noto Sans Sinhala
  and 400 in Noto Serif Oriya;
- the punctuation and digits differ too: in Noto Sans Devanagari and Gurmukhi every one the font maps but `,` and `.`
  has another advance than Noto Sans's, and in Noto Sans and Serif Oriya every one does.

Nothing in the pass lines checks this; it is how the rule draws.

**The in-place clause.** Item 3 has nothing to measure for it. The gate keys on the book's language with a TrueType
set (`InPlaceBuildGate::admitsResident`: `cjkTrueType = r.cjkRepertoire && r.trueTypeSet`, the repertoire from
`TypographyProfile::forLanguage(...).cjkRepertoire`), not on a family's mode. PR 3 extends that rule to the shaped
scripts (the design's "The X4's in-place build").

**Verdict: item 3 passes** with the refined recipe:
- all 22 families merged;
- both checks hold: 1,531,416 of 1,531,416 lines equal against the script's own font, and 792 of 792 Latin letter
  runs equal against the Latin font;
- every family fits the X3's store, all 22 in flash mode.

### The first recipe (superseded)

The first run, as Task 3 first stood: the Latin subset less every code point the script font maps, and a plain
`Merger().merge`. Its outputs were renamed afterwards: `merged-first-recipe/`, `latin-first-recipe/`, and the
`*-first-recipe.md` logs; its supplementary merges stay in `merged-alt/`.

**How.** `tools/arabic_sample.py corpus` wrote `corpus/arabic.txt`: 141 Arabic letters in four positions each, the
four lam-alef pairs, beh with each of U+064B-0652 and with shadda + fatha, and beh ZWNJ beh, 577 strings after the
one duplicate (beh beh) went. Then `tools/merge_families.py dl/fonts merged corpus logs/fonts.md` (now
`tools/merge_families_first_recipe.py`, writing `merged-first-recipe`) ran the plan's
recipe for each of the 22 families, regular with regular and bold with bold:
- Noto's Latin: a `fontTools.subset` of Noto Sans or Noto Serif (U+0020-007E, U+00A0-024F, U+2000-206F, U+20A0-20CF,
  U+2122, U+FFFD, less every code point the script font maps), with `layout_features='*'`, `glyph_names=True`,
  `notdef_outline=True`, `name_IDs='*'`, `hinting=False`;
- upem: every script font and both Latin fonts have 1,000 units per em, so nothing was scaled;
- the merge: `fontTools.merge.Merger().merge([script, latin])`;
- the oracle against the script's own font, with HarfBuzz 14.5.0 at 2133/33, as the plan sets it;
- "Latin kept whole": the three lines, shaped by the Latin subset as merged and by the merged font.

Two departures from the plan's wording, neither changing what is measured:
- fontTools 4.63 renames a duplicate glyph name with a `.n` suffix (`computeMegaGlyphOrder`: `.notdef.1`), not `#n`.
  The Latin check maps each subset glyph's name through the merger's own renaming instead of stripping a suffix,
  which is exact.
- `Merger.merge` opens its inputs twice, so the Latin subsets are written to files
  (`C:/tmp/wm2-measure/latin-first-recipe/`).

**Latin in the script fonts.** #3787's claim that Noto's Indic TTFs carry no Latin holds for letters in 42 of the 44
script files, not for Noto Sans Sinhala, and not for the ASCII punctuation and digits:
- Noto Sans Sinhala (Regular and Bold) maps all 52 letters, and 91 of the 95 code points U+0020-007E (all but
  `$`, `&`, `@` and the grave accent).
- 34 of the 40 Indic files map the same 39 of the 95: space, the digits and the punctuation
  ``!"#%'()*+,-./:;<=>?[\]^_{|}~``, no letters.
- Noto Sans Bengali maps 7 (space `! ? [ ] { }`), Noto Sans Gujarati 1 (space), Noto Naskh Arabic 15 and Noto Sans
  Arabic 16 (space, `! , . :`, the digits, and Sans Arabic's hyphen).
- Since the recipe takes every code point the script font maps from the script font, a merged family draws those
  characters with the script font's glyphs, in Latin text too.
- Each file's two counts are in the last column of the table of rows below.

**The merges.** 20 of the 22 families merged. **Noto Serif Bengali and Noto Serif Tamil did not**: for both styles of
each, `Merger.merge` raised `TypeError: '<' not supported between instances of 'NotImplementedType' and 'int'` while
merging `vhea`. Those two fonts have `vhea` and `vmtx`, which Noto Serif has not, and fontTools' `vhea` rule takes the
maximum of each field across the fonts, which fails on the missing table (`tools/diagnose_merge.py` names the
table). The recipe records them as misses.

| Family | Style | Latin | upem (script / Latin) | scaled | script glyphs | Latin subset glyphs (code points) | merged glyphs | merged bytes | GDEF (B) | GSUB lookups / subtables | GPOS lookups / subtables | CPac (B) | result |
|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| NotoSansDevanagari | Regular | NotoSans | 1000 / 1000 | no | 845 | 858 (618) | 1,703 | 262,000 | 652 | 193 / 461 | 32 / 36 | 18,244 | merged |
| NotoSansDevanagari | Bold | NotoSans | 1000 / 1000 | no | 845 | 858 (618) | 1,703 | 261,660 | 674 | 193 / 461 | 32 / 36 | 18,244 | merged |
| NotoSerifDevanagari | Regular | NotoSerif | 1000 / 1000 | no | 997 | 834 (618) | 1,831 | 293,268 | 940 | 246 / 403 | 32 / 35 | 18,312 | merged |
| NotoSerifDevanagari | Bold | NotoSerif | 1000 / 1000 | no | 997 | 834 (618) | 1,831 | 277,700 | 922 | 246 / 403 | 28 / 30 | 18,080 | merged |
| NotoSansBengali | Regular | NotoSans | 1000 / 1000 | no | 418 | 969 (656) | 1,387 | 189,700 | 1,638 | 243 / 698 | 251 / 282 | 37,368 | merged |
| NotoSansBengali | Bold | NotoSans | 1000 / 1000 | no | 418 | 969 (656) | 1,387 | 188,856 | 1,660 | 243 / 698 | 251 / 282 | 37,368 | merged |
| NotoSerifBengali | Regular | NotoSerif | 1000 / 1000 | no | | | | | | | | | MERGE FAILED: TypeError: '<' not supported between instances of 'NotImplementedType' and 'int' |
| NotoSerifBengali | Bold | NotoSerif | 1000 / 1000 | no | | | | | | | | | MERGE FAILED: TypeError: '<' not supported between instances of 'NotImplementedType' and 'int' |
| NotoSansGurmukhi | Regular | NotoSans | 1000 / 1000 | no | 282 | 858 (618) | 1,140 | 112,068 | 330 | 36 / 37 | 10 / 23 | 2,744 | merged |
| NotoSansGurmukhi | Bold | NotoSans | 1000 / 1000 | no | 282 | 858 (618) | 1,140 | 112,300 | 352 | 36 / 37 | 10 / 23 | 2,744 | merged |
| NotoSerifGurmukhi | Regular | NotoSerif | 1000 / 1000 | no | 294 | 834 (618) | 1,128 | 119,984 | 370 | 30 / 30 | 9 / 11 | 2,092 | merged |
| NotoSerifGurmukhi | Bold | NotoSerif | 1000 / 1000 | no | 294 | 834 (618) | 1,128 | 119,952 | 352 | 30 / 30 | 9 / 11 | 2,092 | merged |
| NotoSansGujarati | Regular | NotoSans | 1000 / 1000 | no | 780 | 981 (667) | 1,761 | 233,576 | 990 | 146 / 185 | 15 / 17 | 9,372 | merged |
| NotoSansGujarati | Bold | NotoSans | 1000 / 1000 | no | 780 | 981 (667) | 1,761 | 232,076 | 1,012 | 146 / 185 | 15 / 17 | 9,372 | merged |
| NotoSerifGujarati | Regular | NotoSerif | 1000 / 1000 | no | 456 | 834 (618) | 1,290 | 184,656 | 638 | 86 / 150 | 13 / 15 | 6,748 | merged |
| NotoSerifGujarati | Bold | NotoSerif | 1000 / 1000 | no | 456 | 834 (618) | 1,290 | 181,952 | 620 | 86 / 150 | 13 / 15 | 6,748 | merged |
| NotoSansOriya | Regular | NotoSans | 1000 / 1000 | no | 627 | 859 (619) | 1,486 | 208,252 | 698 | 198 / 254 | 23 / 24 | 12,876 | merged |
| NotoSansOriya | Bold | NotoSans | 1000 / 1000 | no | 627 | 859 (619) | 1,486 | 207,608 | 720 | 198 / 254 | 23 / 24 | 12,876 | merged |
| NotoSerifOriya | Regular | NotoSerif | 1000 / 1000 | no | 690 | 835 (619) | 1,525 | 234,340 | 528 | 78 / 111 | 14 / 16 | 5,640 | merged |
| NotoSerifOriya | Bold | NotoSerif | 1000 / 1000 | no | 690 | 835 (619) | 1,525 | 233,424 | 510 | 78 / 111 | 14 / 16 | 5,640 | merged |
| NotoSansTamil | Regular | NotoSans | 1000 / 1000 | no | 244 | 856 (616) | 1,100 | 124,036 | 356 | 34 / 38 | 10 / 11 | 2,424 | merged |
| NotoSansTamil | Bold | NotoSans | 1000 / 1000 | no | 244 | 856 (616) | 1,100 | 124,352 | 378 | 34 / 38 | 10 / 11 | 2,424 | merged |
| NotoSerifTamil | Regular | NotoSerif | 1000 / 1000 | no | | | | | | | | | MERGE FAILED: TypeError: '<' not supported between instances of 'NotImplementedType' and 'int' |
| NotoSerifTamil | Bold | NotoSerif | 1000 / 1000 | no | | | | | | | | | MERGE FAILED: TypeError: '<' not supported between instances of 'NotImplementedType' and 'int' |
| NotoSansTelugu | Regular | NotoSans | 1000 / 1000 | no | 961 | 858 (618) | 1,819 | 244,660 | 834 | 110 / 118 | 38 / 39 | 7,928 | merged |
| NotoSansTelugu | Bold | NotoSans | 1000 / 1000 | no | 961 | 858 (618) | 1,819 | 245,460 | 856 | 110 / 118 | 38 / 39 | 7,928 | merged |
| NotoSerifTelugu | Regular | NotoSerif | 1000 / 1000 | no | 743 | 834 (618) | 1,577 | 277,972 | 566 | 106 / 112 | 35 / 36 | 7,516 | merged |
| NotoSerifTelugu | Bold | NotoSerif | 1000 / 1000 | no | 743 | 834 (618) | 1,577 | 275,948 | 548 | 106 / 112 | 35 / 36 | 7,516 | merged |
| NotoSansKannada | Regular | NotoSans | 1000 / 1000 | no | 657 | 858 (618) | 1,515 | 211,800 | 772 | 214 / 227 | 15 / 22 | 12,404 | merged |
| NotoSansKannada | Bold | NotoSans | 1000 / 1000 | no | 657 | 858 (618) | 1,515 | 210,656 | 794 | 214 / 227 | 15 / 22 | 12,404 | merged |
| NotoSerifKannada | Regular | NotoSerif | 1000 / 1000 | no | 419 | 834 (618) | 1,253 | 219,364 | 420 | 84 / 90 | 58 / 67 | 7,760 | merged |
| NotoSerifKannada | Bold | NotoSerif | 1000 / 1000 | no | 419 | 834 (618) | 1,253 | 183,900 | 376 | 84 / 90 | 57 / 64 | 7,660 | merged |
| NotoSansMalayalam | Regular | NotoSans | 1000 / 1000 | no | 365 | 858 (618) | 1,223 | 153,404 | 286 | 86 / 90 | 14 / 18 | 5,408 | merged |
| NotoSansMalayalam | Bold | NotoSans | 1000 / 1000 | no | 365 | 858 (618) | 1,223 | 152,484 | 308 | 86 / 90 | 14 / 18 | 5,408 | merged |
| NotoSerifMalayalam | Regular | NotoSerif | 1000 / 1000 | no | 355 | 834 (618) | 1,189 | 156,744 | 342 | 82 / 86 | 14 / 19 | 5,224 | merged |
| NotoSerifMalayalam | Bold | NotoSerif | 1000 / 1000 | no | 355 | 834 (618) | 1,189 | 155,548 | 324 | 82 / 86 | 14 / 19 | 5,224 | merged |
| NotoSansSinhala | Regular | NotoSans | 1000 / 1000 | no | 371 | 828 (569) | 1,199 | 161,628 | 378 | 55 / 69 | 17 / 18 | 4,120 | merged |
| NotoSansSinhala | Bold | NotoSans | 1000 / 1000 | no | 371 | 828 (569) | 1,199 | 160,116 | 378 | 55 / 69 | 17 / 18 | 4,120 | merged |
| NotoSerifSinhala | Regular | NotoSerif | 1000 / 1000 | no | 645 | 836 (620) | 1,481 | 279,896 | 576 | 92 / 92 | 9 / 10 | 5,292 | merged |
| NotoSerifSinhala | Bold | NotoSerif | 1000 / 1000 | no | 645 | 836 (620) | 1,481 | 275,784 | 558 | 92 / 92 | 9 / 10 | 5,292 | merged |
| NotoNaskhArabic | Regular | NotoSerif | 1000 / 1000 | no | 1,415 | 870 (646) | 2,285 | 252,656 | 652 | 65 / 77 | 26 / 28 | 5,084 | merged |
| NotoNaskhArabic | Bold | NotoSerif | 1000 / 1000 | no | 1,415 | 870 (646) | 2,285 | 252,444 | 634 | 65 / 77 | 26 / 28 | 5,084 | merged |
| NotoSansArabic | Regular | NotoSans | 1000 / 1000 | no | 1,399 | 893 (645) | 2,292 | 227,748 | 534 | 64 / 74 | 27 / 29 | 5,036 | merged |
| NotoSansArabic | Bold | NotoSans | 1000 / 1000 | no | 1,399 | 893 (645) | 2,292 | 227,976 | 556 | 64 / 74 | 27 / 29 | 5,036 | merged |

**The fit** (`Step2MergedFamiliesProbe.cpp` in the scratch tree's `TtfFontTest`,
`STEP2_MERGED=C:/tmp/wm2-measure/merged-first-recipe`): each style probed as `ShippedFamiliesTest` does
(`MemoryFile`, `ttf::probeFile`); flash bytes = the file + `CPac` padded to 4; SD bytes = `sdBytes` + 16 + GDEF
padded to 4 + 16 + `CPac` padded to 4; `glyphstore::planLayout` with both styles in `kModeFlash`, else both in
`kModeSd`, on the X3's font area (3,473,408 B) with 32 free map pages; and `ttf::fitsStore` agreed with it for every
family. Every family that merged fits **in flash mode**, the largest
(Noto Serif Devanagari) with two styles of 311,580 B and 295,780 B, and a log of 1,376,256 B left.

**One row per family and style (first recipe):**

| Family | Style | bytes | GDEF | CPac | flash bytes | SD bytes | mode | oracle vs the script's font | Latin lines (3) | Latin runs | script font's Latin: letters / 52, U+0020-007E / 95 |
|---|---|---:|---:|---:|---:|---:|---|---|---|---|---|
| NotoSansDevanagari | Regular | 262,000 | 652 | 18,244 | 280,244 | 125,132 | flash | equal: 20,522 lines x 5 (none, hi, mr, ne, sa) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansDevanagari | Bold | 261,660 | 674 | 18,244 | 279,904 | 126,180 | flash | equal: 20,522 lines x 5 (none, hi, mr, ne, sa) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifDevanagari | Regular | 293,268 | 940 | 18,312 | 311,580 | 134,548 | flash | equal: 20,522 lines x 5 (none, hi, mr, ne, sa) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifDevanagari | Bold | 277,700 | 922 | 18,080 | 295,780 | 119,964 | flash | equal: 20,522 lines x 5 (none, hi, mr, ne, sa) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansBengali | Regular | 189,700 | 1,638 | 37,368 | 227,068 | 106,452 | flash | equal: 14,852 lines x 3 (none, bn, as) | 0/3 equal | 21/21 equal | 0 / 7 |
| NotoSansBengali | Bold | 188,856 | 1,660 | 37,368 | 226,224 | 107,440 | flash | equal: 14,852 lines x 3 (none, bn, as) | 0/3 equal | 21/21 equal | 0 / 7 |
| NotoSerifBengali | Regular | merge raised | | | | | none | not run | not run | not run | 0 / 39 |
| NotoSerifBengali | Bold | merge raised | | | | | none | not run | not run | not run | 0 / 39 |
| NotoSansGurmukhi | Regular | 112,068 | 330 | 2,744 | 114,812 | 30,424 | flash | equal: 14,545 lines x 2 (none, pa) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansGurmukhi | Bold | 112,300 | 352 | 2,744 | 115,044 | 30,668 | flash | equal: 14,545 lines x 2 (none, pa) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifGurmukhi | Regular | 119,984 | 370 | 2,092 | 122,076 | 28,624 | flash | equal: 14,545 lines x 2 (none, pa) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifGurmukhi | Bold | 119,952 | 352 | 2,092 | 122,044 | 28,612 | flash | equal: 14,545 lines x 2 (none, pa) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansGujarati | Regular | 233,576 | 990 | 9,372 | 242,948 | 84,444 | flash | equal: 14,668 lines x 2 (none, gu) | 0/3 equal | 21/21 equal | 0 / 1 |
| NotoSansGujarati | Bold | 232,076 | 1,012 | 9,372 | 241,448 | 84,712 | flash | equal: 14,668 lines x 2 (none, gu) | 0/3 equal | 21/21 equal | 0 / 1 |
| NotoSerifGujarati | Regular | 184,656 | 638 | 6,748 | 191,404 | 50,024 | flash | equal: 14,668 lines x 2 (none, gu) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifGujarati | Bold | 181,952 | 620 | 6,748 | 188,700 | 48,952 | flash | equal: 14,668 lines x 2 (none, gu) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansOriya | Regular | 208,252 | 698 | 12,876 | 221,128 | 72,132 | flash | equal: 14,891 lines x 2 (none, or) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansOriya | Bold | 207,608 | 720 | 12,876 | 220,484 | 72,568 | flash | equal: 14,891 lines x 2 (none, or) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifOriya | Regular | 234,340 | 528 | 5,640 | 239,980 | 55,092 | flash | equal: 14,891 lines x 2 (none, or) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifOriya | Bold | 233,424 | 510 | 5,640 | 239,064 | 54,828 | flash | equal: 14,891 lines x 2 (none, or) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansTamil | Regular | 124,036 | 356 | 2,424 | 126,460 | 35,116 | flash | equal: 11,203 lines x 2 (none, ta) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansTamil | Bold | 124,352 | 378 | 2,424 | 126,776 | 35,388 | flash | equal: 11,203 lines x 2 (none, ta) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifTamil | Regular | merge raised | | | | | none | not run | not run | not run | 0 / 39 |
| NotoSerifTamil | Bold | merge raised | | | | | none | not run | not run | not run | 0 / 39 |
| NotoSansTelugu | Regular | 244,660 | 834 | 7,928 | 252,588 | 67,280 | flash | equal: 15,680 lines x 2 (none, te) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansTelugu | Bold | 245,460 | 856 | 7,928 | 253,388 | 67,912 | flash | equal: 15,680 lines x 2 (none, te) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifTelugu | Regular | 277,972 | 566 | 7,516 | 285,488 | 59,608 | flash | equal: 15,680 lines x 2 (none, te) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifTelugu | Bold | 275,948 | 548 | 7,516 | 283,464 | 60,312 | flash | equal: 15,680 lines x 2 (none, te) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansKannada | Regular | 211,800 | 772 | 12,404 | 224,204 | 79,588 | flash | equal: 14,916 lines x 2 (none, kn) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansKannada | Bold | 210,656 | 794 | 12,404 | 223,060 | 79,896 | flash | equal: 14,916 lines x 2 (none, kn) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifKannada | Regular | 219,364 | 420 | 7,760 | 227,124 | 76,264 | flash | equal: 14,916 lines x 2 (none, kn) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifKannada | Bold | 183,900 | 376 | 7,660 | 191,560 | 42,936 | flash | equal: 14,916 lines x 2 (none, kn) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansMalayalam | Regular | 153,404 | 286 | 5,408 | 158,812 | 43,228 | flash | equal: 17,630 lines x 2 (none, ml) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansMalayalam | Bold | 152,484 | 308 | 5,408 | 157,892 | 43,600 | flash | equal: 17,630 lines x 2 (none, ml) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifMalayalam | Regular | 156,744 | 342 | 5,224 | 161,968 | 39,192 | flash | equal: 17,630 lines x 2 (none, ml) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifMalayalam | Bold | 155,548 | 324 | 5,224 | 160,772 | 39,220 | flash | equal: 17,630 lines x 2 (none, ml) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSansSinhala | Regular | 161,628 | 378 | 4,120 | 165,748 | 33,380 | flash | equal: 13,157 lines x 2 (none, si) | 0/3 equal | 1/1 equal | 52 / 91 |
| NotoSansSinhala | Bold | 160,116 | 378 | 4,120 | 164,236 | 33,632 | flash | equal: 13,157 lines x 2 (none, si) | 0/3 equal | 1/1 equal | 52 / 91 |
| NotoSerifSinhala | Regular | 279,896 | 576 | 5,292 | 285,188 | 48,912 | flash | equal: 13,157 lines x 2 (none, si) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoSerifSinhala | Bold | 275,784 | 558 | 5,292 | 281,076 | 49,004 | flash | equal: 13,157 lines x 2 (none, si) | 0/3 equal | 19/19 equal | 0 / 39 |
| NotoNaskhArabic | Regular | 252,656 | 652 | 5,084 | 257,740 | 63,264 | flash | equal: 577 lines x 4 (none, ar, fa, ur) | 0/3 equal | 20/20 equal | 0 / 15 |
| NotoNaskhArabic | Bold | 252,444 | 634 | 5,084 | 257,528 | 63,328 | flash | equal: 577 lines x 4 (none, ar, fa, ur) | 0/3 equal | 20/20 equal | 0 / 15 |
| NotoSansArabic | Regular | 227,748 | 534 | 5,036 | 232,784 | 64,464 | flash | equal: 577 lines x 4 (none, ar, fa, ur) | 0/3 equal | 20/20 equal | 0 / 16 |
| NotoSansArabic | Bold | 227,976 | 556 | 5,036 | 233,012 | 64,924 | flash | equal: 577 lines x 4 (none, ar, fa, ur) | 0/3 equal | 20/20 equal | 0 / 16 |

- "oracle vs the script's font": every line of the script's corpus (Arabic: `corpus/arabic.txt`, rtl, `Arab`), with
  no language and each language of Task 1 (Arabic: none, `ar`, `fa`, `ur`): glyph IDs, advances and offsets equal on
  every line of all 102 runs, 1,397,492 lines.
- "Latin lines (3)", the check the plan sets: **0 of the 120 line checks are equal.** Every script font maps the
  space at least, and each line has spaces, so the subset, which leaves the script font's code points out, shapes
  them as `.notdef` (`.notdef.1` in the merged order) where the merged font has the script font's space. Noto Sans
  Devanagari, `AVATAR Wave fi fl ffi`: every glyph, kerning and ligature equal (`A V A T A R`, `W a v e`, `fi`, `fl`,
  `f_f_i`), and the four spaces `.notdef.1` 1,280 against `space` 555. The line check cannot hold for any of these
  fonts as the recipe stands.
- "Latin runs" (supplementary, `tools/merge_supplement.py`, not a pass line): each line cut into the longest runs of
  code points the subset maps, each run shaped by the subset and by the merged font. Every run is equal in all 40
  merged styles: the Latin font's own GSUB and GPOS (kerning, the `fi`/`fl`/`ffi` ligatures) survive the merge. Noto
  Sans Sinhala keeps only one run (`€`), since its own Latin serves the rest.

**Supplementary: the two failed merges with `vhea` and `vmtx` dropped** (`Merger(options=Options(drop_tables=["vhea",
"vmtx"]))`, into `C:/tmp/wm2-measure/merged-alt`; not the plan's recipe, so not counted for the pass line):

| Family | Style | merged bytes | GDEF (B) | GSUB lookups / subtables | GPOS lookups / subtables | CPac (B) | oracle (languages: lines equal) | Latin runs equal |
|---|---|---:|---:|---:|---:|---:|---|---|
| NotoSerifBengali | Regular | 275,992 | 378 | 93 / 117 | 23 / 25 | 6,672 | none 14,852/14,852; bn 14,852/14,852; as 14,852/14,852 | 19/19 |
| NotoSerifBengali | Bold | 275,220 | 360 | 93 / 117 | 23 / 25 | 6,672 | none 14,852/14,852; bn 14,852/14,852; as 14,852/14,852 | 19/19 |
| NotoSerifTamil | Regular | 136,444 | 534 | 28 / 30 | 8 / 11 | 2,008 | none 11,203/11,203; ta 11,203/11,203 | 19/19 |
| NotoSerifTamil | Bold | 136,704 | 516 | 28 / 30 | 8 / 11 | 2,008 | none 11,203/11,203; ta 11,203/11,203 | 19/19 |

Both then fit in flash mode too (`STEP2_MERGED=C:/tmp/wm2-measure/merged-alt`):

| Family | Style | bytes | GDEF | CPac | probe style / weight | sdBytes | flash bytes | SD bytes |
|---|---|---:|---:|---:|---|---:|---:|---:|
| NotoSerifBengali | Regular | 275992 | 378 | 6672 | 0 / 400 | 59776 | 282664 | 66860 |
| NotoSerifBengali | Bold | 275220 | 360 | 6672 | 1 / 700 | 61508 | 281892 | 68572 |
| NotoSerifTamil | Regular | 136444 | 534 | 2008 | 0 / 400 | 32092 | 138452 | 34668 |
| NotoSerifTamil | Bold | 136704 | 516 | 2008 | 1 / 700 | 32248 | 138712 | 34804 |

| Family | mode | fitsStore | log (B) |
|---|---|---|---:|
| NotoSerifBengali | flash | true | 1376256 |
| NotoSerifTamil | flash | true | 1507328 |

**The in-place clause.** Item 3 has nothing to measure for it. The gate keys on the book's language with a TrueType
set (`InPlaceBuildGate::admitsResident`: `cjkTrueType = r.cjkRepertoire && r.trueTypeSet`, the repertoire from
`TypographyProfile::forLanguage(...).cjkRepertoire`), not on a family's mode. PR 3 extends that rule to the shaped
scripts (the design's "The X4's in-place build").

**Verdict of the first run: item 3 missed**, on two of its three conditions:
- 20 of the 22 families merged, not 22: Noto Serif Bengali and Noto Serif Tamil raise in fontTools' `vhea` merge.
- The oracle against the script's own font is equal on every line (1,397,492 of 1,397,492), but the Latin line check
  is equal on 0 of 120, because every script font maps the space (and most map the ASCII digits and punctuation),
  which the recipe takes from the script font.
- Every merged family fits the X3's store in flash mode (20 of 20; the two supplementary merges as well).

What the supplementary runs suggested for the design's reopening (not measured as the plan's recipe): dropping `vhea`
and `vmtx` from the merge makes both failed families merge, with their oracle equal and both fitting in flash mode;
and the Latin check holds run by run, so a line check that excludes, or expects, the script font's own code points
would pass.
Whether the merged families should draw Latin text's space, digits and punctuation with the script font's glyphs (and
Noto Sans Sinhala's Latin letters with its own) was a design question the recipe then answered "yes". The maintainer
answered it on 2026-11-02 (decision 6's refinement, above): letters from Noto Sans or Serif, the rest from the script
font.

## PR 1: the engine re-measured

Task 9 ran the same oracle, probe, C3 size check and scratch flash build on this branch's engine (`lib/OtShaper` with
deviation 1: nothrow fixed storage, the `Buffer` bound to caller memory with `setStorage`), at `e82bb5181`. The
workspace logs are `logs/oracle-ours.log`, `logs/probe-ours.md` (counts), `logs/probe-ours-run2.md` (times),
`logs/sizeof-c3-ours.log` and `logs/pio-ours-{default,gh_release}.log`. The probe is kept as a host tool,
`test/ot_shaper_port/OtShaperProbe.cpp` (target `ot_shaper_probe`; `tools/OtShaperProbe.cpp` formatted with clang-format
21, as CI formats every tracked C++ file under `test/`, and copied back so the two stay identical; under
`__has_include("OtFixedVector.h")` it binds each `Buffer` to `Buffer::bytesFor(4096)` bytes before first use).

**Oracle.** `tools/oracle.sh bin/ot_shape_cli_ours.exe logs/oracle-ours.log`, built with the design's flags
(`-std=c++20 -O2 -fno-exceptions -static`, `-Ilib/OtShaper -Ilib/Utf8 -Ilib/Memory`).

| Engine | Runs | Runs N/N | Summary lines |
|---|---:|---:|---|
| upstream (Task 1) | 216 | 216 | `logs/oracle-upstream.log` |
| ours | 216 | 216 | `logs/oracle-ours.log`: all 216 `<font>: N/N lines identical` lines equal Task 1's |

**The probe, side by side** (the same 44 entries and arguments as Task 1). Counts come from the first run
(`probe-ours.md`), which overlapped the oracle; the counts of the second run (`probe-ours-run2.md`) are equal. The times
are from that second run, set against Task 1's upstream run: upstream was not run again beside ours, and host builds from
another worktree may still have run during the second run. The time differences below are therefore indicative only and
may move either way on a quiet machine; no time bar is set by the design or the plan.

| | upstream | ours | |
|---|---|---|---|
| builds | link, no diagnostics | link, no diagnostics | equal |
| Plans (48 rows): `build`, GSUB/GPOS lookups, GSUB/GPOS stages | all ok | all ok, same figures | equal |
| Shaping (44 rows): lines, max code points, max glyphs, max glyphs - code points | e.g. Devanagari 20,522 / 7 / 10 / 3 | same on all 44 | equal |
| Shaping: lines failed, either flag | 0 / 0 | 0 / 0 | equal |
| Allocations in the shaping loop | 8-16 a font (480 over the 44) | **0** | ours lower |
| Heap peak in the shaping loop | up to 2,416 B | **0** | ours lower |
| Allocations in `Plan::build` | 25 (`computed`), 19 (`stored`), all throwing | **5, 3**, 0 throwing | ours lower |
| `Plan::memoryBytes` | 13,708 B (Devanagari `computed`) | 13,828 B | **ours higher, +120 B on all 48 rows** |
| `Plan::build` heap peak | 15,997 B (Devanagari `computed`) | 16,468 B | **ours higher, +427 to +471 B on all 48 rows** |
| `Plan::build` time (mean of the 48 rows) | 19.9 us | 22.8 us | ours 15% slower (Devanagari `computed`: 51.6 us) |
| Shaping time per line (mean of the 44 means) | 4.0 us | 4.7 us | ours 18% slower |
| Shaping time per line, slowest line | 135.2 us | 112.5 us | not a trend |

**The rulings (controller), stated plainly.** The `Plan::build` heap peak above upstream's is accepted, and it departs
from the plan's expectation that it be no higher than upstream's.
- `memoryBytes` is +120 B on the host because `sizeof(Plan)` grew (376 to 496 B).
- The rest of the excess (about 350 B on the host) is the builder's fixed worst-case scratch block (`MAX_FEATURES` 64,
  2 x `MAX_STAGES` pauses), where upstream `reserve()`d exact counts. It lives during one build only.
- A build peaks at 3.3-27.2 KB (Tamil `stored` to Bengali `.ttf`). The excess is a near-constant +427 to +471 B, so
  about +2% on the large plans (Devanagari `computed` +2.9%) and +12-17% on the smallest (Tamil `.layout`: `stored`
  2,817 to 3,288 B, `computed` 3,433 to 3,904 B). In exchange the allocations fall from 25 to 5, none throwing, and the
  shaping loop allocates nothing.

**The sizes on the C3** (`riscv32-esp-elf-g++ -std=gnu++2a -Os -fno-exceptions -fsyntax-only`, `-Ilib/Memory` added):

| Type | upstream | ours |
|---|---:|---:|
| `GlyphInfo` | 20 | 20 |
| `GlyphPosition` | 20 | 20 |
| `Buffer` | 1,248 | 1,272 |
| `Face` | 120 | 120 |
| `Plan` | 232 | 292 |
| `PlannedLookup` | 8 | 8 |
| `Stage` | 4 | 4 |

On the host: `Buffer` 1,296 to 1,344 B, `Plan` 376 to 496 B, the rest equal.

**The engine's flash, by the scratch build.** The same recipe as item 2 (master `55589ef59`, SDK `5bd4a0a`,
`src/OtShaperFlashProbe.cpp` linking the engine through a never-run call), with this branch's `lib/OtShaper` and the
three `lib/Utf8/Indic*` files copied in by `git archive HEAD`. The scratch worktree had been removed after item 2; it was
recreated at that exact commit and SDK, so item 2's master baseline stands without a rebuild, and the delta is against
it.

| Env | Flash, master | Flash, upstream's engine | Flash, ours | Delta, upstream | Delta, ours | RAM, master | RAM, ours | Delta |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `default` | 6,161,905 | 6,214,173 | 6,209,481 | +52,268 | **+47,576** | 57,776 | 57,800 | +24 |
| `gh_release` | 6,039,675 | 6,091,675 | 6,087,079 | +52,000 | +47,404 | 57,272 | 57,296 | +24 |

Our engine links 4,692 B (`default`) and 4,596 B (`gh_release`) less than upstream's, with the same static RAM. E
drops from 52,268 B to 47,576 B, so item 2's pass lines hold with more room: E + A = 62,936 B against 92,160 B, and the
free-slot line 6,553,600 - (6,209,481 + 15,360 + 20,480) = 308,279 B against 256,000 B (still provisional on P2).

## The gate

| Item | Pass line | Measured | Result |
|---|---|---|---|
| 1 | 0 differences from HarfBuzz on upstream's corpus under our compiler and flags | 216 of 216 runs N/N; 3,436,140 lines, 0 mismatches | **pass** |
| 2 | E + A <= 92,160 B | E = 52,268 B; E + A = 67,628 B | **pass** |
| 2 | 6,553,600 - (`default` with the call + A + P2) >= 256,000 B | 303,587 B free (with P2 = 20,480 B, provisional: step 1 PR 2 has no figure yet) | **pass (provisional)** |
| 3 | all 22 families merged | 22 of 22 (decision 6 refined: `vhea`/`vmtx` dropped) | **pass** |
| 3 | both checks hold | equal against the script's own font: 1,531,416 of 1,531,416 lines (112 runs); Latin kept whole: 792 of 792 letter runs equal against Noto Sans/Serif | **pass** |
| 3 | every family fits, in flash or SD mode | 22 of 22, all in flash mode | **pass** |

**Item 1 passes, item 2 passes (its free-slot line provisional on P2), item 3 passes with decision 6 as refined.**
By the plan's Task 4, PR 1 can start with Task 5.

The first run of item 3, with the first recipe, missed: 20 of 22 families merged (Noto Serif Bengali and Noto Serif
Tamil raised in fontTools' `vhea` merge), and the Latin line check held on 0 of 120 lines. The maintainer refined
decision 6 on 2026-11-02, the plan's Task 3 followed, and item 3 was run again (above).

**Estimate -> measured**, for the design's rows ("Memory, flash and time") this phase measured:

| What | The design's estimate | Measured | How |
|---|---|---|---|
| Glyph buffer per call | 62 B a slot, 2n + 16 slots, plus the `Buffer` (its `matchStack` 1,152 B): about 6.3 KB for n = 32, 10.4 KB for n = 64 | a slot 62 B and the `Buffer` 1,248 B on the C3 (upstream's, four `std::vector`s): 6,208 B (6.1 KB) for n = 32, 10,176 B (9.9 KB) for n = 64 | C3 sizes; the totals derived |
| A face per style | about 120 B | 120 B on the C3 (224 B on the host) | C3 sizes |
| A plan (face x script x language) | 8 B a planned lookup plus stages: about 1.5 KB for Devanagari (at most 185 lookups), 2.1 KB for Bengali (260) | with `CPac`: 1,000 B (Devanagari fixture, 69 lookups planned) and 1,328 B (Bengali, 110) on the host, so about 856 B and 1,184 B on the C3; a build peaks 2,289 B higher on the host (the planner's scratch) | probe; the C3 figures derived |
| Runtime lookup filters, avoided by `CPac` | up to 17.5 KB (Devanagari), 27.1 KB (Bengali), 1.0 KB (Tamil), 3.5 KB (Sinhala) a face | 12,708 B, 17,168 B, 616 B, 1,844 B (the fixtures); on the 40 TTFs the TrueType route's whole plan holds 1,204-24,448 B | probe |
| `CPac` | about 16 KB (Devanagari), 25 KB (Bengali), 0.9 KB (Tamil), 3.0 KB (Sinhala) a style | the fixtures: 16,044 B, 25,056 B, 892 B, 3,056 B; the 44 merged styles (refined recipe): 2,008-37,368 B (Noto Sans Devanagari 18,244, Noto Sans Bengali 37,368, Noto Sans Tamil 2,424, Noto Sans Sinhala 4,432), the Latin font's lookups counted too | fixtures' tables; merged fonts counted |
| The 22 hosted families | file sizes of the merged fonts unknown | 112,068-293,268 B a style (22 families, refined recipe); every one fits the X3's store in flash mode | merge + fit |
| Flash | 55-75 KB for PRs 1-3 | the engine alone (upstream's, with the runtime planner, linked by a never-run call): 52,268 B (51.0 KB) on `default`, 52,000 B on `gh_release`; with A, 67,628 B (66.0 KB) | scratch build |
| Static RAM | upstream +224 B | +24 B on `default` and `gh_release` | scratch build |
