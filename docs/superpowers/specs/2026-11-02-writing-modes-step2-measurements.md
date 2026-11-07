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

## PR 2: TrueType faces that shape (item 4)

Measured on the X3 (COM6) on 2026-11-05, on the maintainer's card: 18 font families, the merged Noto Sans Devanagari
and Noto Sans Bengali among them (uploaded to `/.crosspoint/fonts/` with `devctl put:` over the no-reset open). The
baseline is master at the branch's merge base `2755ef1c9`; the branch is `e8862d563`. Both `default`, flashed with
esptool. The X4 was not available, so the before/after pages ran on the X3 as well, and the X4's `SHAPE` figures
(the plan's Step 5) are not measured.

**Before and after.** `/test/ePubTest.epub@3:2` in BookerlyTTF (flash mode) and `/test/test_cjk_zh_honglou.epub@1:1`
in NotoSansTC (SD mode), at 14 pt: each page after the update is pixel-identical to the baseline's below the status
bar (the clock differs).

**The reinstall and `CPac`.** Each family installs once on its first open after the update (`kRenderVersion` 4).
`CPac` only for a font whose GSUB or GPOS names a script the shaper handles (the maintainer's ruling on the final
review's I1).

| Family | Styles | Mode | `CPac` a style, written (B) | Item 3's count (B) | Install (ms) |
|---|---|---|---|---|---|
| BookerlyTTF | 4 | flash | none (no shaped script) | - | 8,404 |
| NotoSansTC | 1 | SD | none (no shaped script) | - | 9,117 |
| NotoSansDevanagari (merged) | 2 | flash | 18,244 | 18,244 | 4,497 |
| NotoSansBengali (merged) | 2 | flash | 37,368 | 37,368 | 6,358 |

**One word shaped** (`CMD:TTF:SHAPE`, the body family's Regular at 14 pt, three runs each: min / median / max in
microseconds). `init` is the face's first `current()` (`Face::init` with `CPac` validated), `plan` the plan's build
with its filters stored, `shape` one `ot::shape`; `plan bytes` the plan's heap.

| Word | init | plan | plan bytes | shape | glyphs | width (px) | stored |
|---|---|---|---|---|---|---|---|
| हिन्दी (hi) | 717 / 717 / 719 | 1,669 / 1,674 / 1,677 | 980 | 3,727 / 4,168 / 4,199 | 5 | 57 | 5 |
| प्रार्थना (hi) | 717 / 717 / 717 | 1,669 / 1,670 / 2,014 | 980 | 2,436 / 2,872 / 2,872 | 6 | 68 | 5 |
| শ্রীঈশ্বরচন্দ্র (bn) | 1,303 / 1,304 / 1,305 | 2,078 / 2,081 / 2,426 | 2,028 | 6,479 / 6,826 / 6,944 | 13 | 127 | 13 |
| কর্ত্তৃক (bn) | 1,303 / 1,303 / 1,631 | 2,074 / 2,074 / 2,075 | 2,028 | 4,938 / 5,400 / 5,470 | 5 | 69 | 4 |

`repoint_us` was -1 in every run: after the Bold style's block was mapped, the Regular's `current()` found its block
at the same base, so the face had nothing to re-point. The C3 maps one flash-mode style at a time and gave the
Regular back the address it had. Deviation 12's re-point is proven on the host (`MovingNorFlash`, which never reuses
an address); these runs did not need it, so its device cost is not measured.

**Discovery** (from `Display initialized` to `Discovery complete`, 18 families, the same card): 470 ms on the
baseline, 552 ms on the branch (+82 ms). A font without a shaped script now takes two reads more to probe (its
GSUB and GPOS script lists: Bookerly-Regular 6 -> 8 read calls, host count); a font with one also counts its
lookups in coalesced reads (NotoSans-Regular: 44 calls, with a budget of 512 a table).

**Flash and RAM** against the baseline `2755ef1c9`:

| Env | Flash, baseline | Flash, branch | Delta | RAM, baseline | RAM, branch | Delta |
|---|---|---|---|---|---|---|
| `gh_release` | 6,074,863 | 6,085,251 | +10,388 | 57,368 | 57,376 | +8 |
| `default` | 6,198,283 | 6,252,223 | +53,940 | 57,872 | 57,880 | +8 |

- `default`'s delta less `gh_release`'s, 43,552 B, is the shaping engine and `TTF:SHAPE`, which only dev builds link
  (PR 1 measured the whole engine at 47,576 B). The release build now links the engine's face, table and coverage
  code for the install's `CPac`.
- The gate: `gh_release`'s +10,388 B stays under A = 15,360 B, leaving 4,972 B of A for PR 3. The running total for
  PRs 1-3 on the release build is 10,388 B so far; with E = 47,576 B and the rest of A it is at most 62,936 B, under
  the 92,160 B pass line.

**Estimate -> measured** for the design's rows this PR measures:

| What | The design's estimate | Measured |
|---|---|---|
| `CPac` | about 16 KB (Devanagari), 25 KB (Bengali) a style, flash | 18,244 B and 37,368 B a style, written at install (the merged families' Latin lookups count too); fonts without a shaped script carry none |
| The reinstall | every installed TrueType family copied again once, one boot after the update; install times not measured | on each family's first open: 8.4 s (BookerlyTTF, 4 styles, flash), 9.1 s (NotoSansTC, SD), 4.5 s and 6.4 s (the merged Devanagari and Bengali, 2 styles each); pages pixel-identical afterwards |

## PR 3: shaped text on the page (items 5 and 6)

Measured on the X3 (COM6) on 2026-11-06, on the maintainer's card, in the merged Noto Sans Devanagari and Noto Sans
Bengali families from PR 2. The baseline is master `50b8156b8` (the branch's merge base). The branch was measured
twice:
- `c3597cfbd`, before the word cache and the engine memos;
- `a0733646e`, with them (Tasks 12a and 12b).

All runs used `default`, flashed with esptool, except the stack figures, which come from `default` built with
`-DENABLE_BOOT_HEAP_DIAGNOSTICS`. The X4 was not available, and the maintainer waived its runs for this PR on
2026-11-06: its rows are not measured, as in PR 2.

**Item 5 on the host** (Task 12; heap-only harness; every compared run shares one book path and one cache path,
because allocation counts depend on the paths' lengths):

| Book | Pages | Allocations | A page | `page_bytes` mean / max | Glyph / #3787-token bytes |
|---|---|---|---|---|---|
| test_shaped_devanagari | 2 | 1,238 | 619 | 5,196 / 6,691 | 3,752 / 7,504 |
| test_shaped_devanagari, no face | 2 | 1,214 | 607 | 3,228 / 4,122 | - |
| test_shaped_bengali | 2 | 1,256 | 628 | 4,846 / 5,924 | 4,048 / 8,096 |
| test_shaped_tamil | 3 | 1,351 | 450 | 3,599 / 5,748 | 4,236 / 8,472 |
| test_shaped_malayalam | 3 | 1,583 | 528 | 5,377 / 6,052 | 6,772 / 13,544 |
| test_shaped_long_hi | 10 | 2,583 | 258 | 10,420 / 12,288 | 38,440 / 76,880 |
| test_shaped_long_hi, no face | 11 | 2,591 | 236 | 5,904 / 7,278 | - |
| test_shaped_long_en | 13 | 2,033 | 156 | 2,176 / 2,519 | - |

- Shaping adds 24 allocations (+2%) on the short Hindi book, and none (-8) on the long one.
- The gap to a Latin book comes from the book's size and from the parser's per-word `std::string` past the 15-byte
  SSO. That is 746 of the long Hindi book's allocations against 12 of the English one's.

The per-call cap of 32 code points (Task 6) is set by two real books:
- Indulekha (Malayalam): 55,149 tokens, 17 past 32 code points (0.031%), none past 48.
- Ponniyin Selvan (Tamil): 418,663 tokens, 2 past 32 (0.0005%), none past 48.

**The build-time gate.** Each timing book was opened at `@2:1` in NotoSansDevanagari at 14 pt, from a cold section
cache, three times each after one uncounted warm-up. Times are `createSectionFile` totals, min / median / max, in
ms:

| Firmware | Hindi chapter (2,483 words) | English chapter (same words and paragraphs) | Ratio of medians |
|---|---|---|---|
| master `50b8156b8`, unshaped | 3,371 (one run; parse 1,826) | - | - |
| `c3597cfbd`, shaped | 10,751 / 10,753 / 10,762 (parse ~9,400) | 2,556 / 2,560 / 2,576 | 4.20: **missed** |
| `a0733646e`, cache and memos | 4,925 / 4,931 / 4,933 (parse ~3,605) | 2,523 / 2,529 / 2,535 | **1.95: pass** |

- **The miss, profiled** (`OT_PROFILE` build, the long Hindi chapter): `ot::shape` was 5,739 ms of the 7,566 ms that
  shaping added, 2.34 ms a word. Its shares:

  | Part | Share |
  |---|---|
  | GSUB lookups | 41.6% |
  | Indic initial reordering | 13.0% |
  | The GSUB loop's own digest and settings reads | 10.3% |
  | GPOS, advances and the tail | 14.5% |
  | The rest | 20.6% |

  A word shaped twice in a row cost the same cycles both times, to the cycle: one word's working set overflows the
  16 KB flash cache. The cost is reads of the flash-mapped GSUB, not instructions; the host is about 300x faster.
- **The word cache** (Task 12b; 16 KB in the build arena's free tail, 2-way, 64 B ways) hit 1,943 of 2,478 lookups
  (78.4%) with no clear. The memos (Task 12a) take the per-consonant `wouldSubstitute` walks and the planned lookups'
  digest reads off the flash.
- **The gate's book flatters the cache.** Its chapter 2 repeats the declaration's paragraphs: 136 distinct words in
  2,483. On real prose (host dump tool, the X3's arena, the merged fonts) the cache hits:
  - 24.3% of 52,646 lookups on Indulekha (Malayalam, 26 spines);
  - 17.8% of 415,715 on Ponniyin Selvan (Tamil, 298 spines).

  Both languages are agglutinative.
- **Real Hindi prose on the X3** (`082c9e539`, the final review's fixes): Premchand's *Gaban* (1931; Premchand
  Rachnavali vol. 5 on Hindi Wikisource), chapters 1-8 cut at the first paragraph past 2,500 words: 62 paragraphs,
  2,593 words (852 distinct of 2,587 Devanagari tokens, so no cache could hit more than 67.1%). Its Latin twin has the
  same paragraphs' word counts, built by the fixture generator's `long_chapters`. Three cold builds each at `@2:1`, in
  ms:

  | Book | Chapter 2 total, min / median / max | Word cache |
  |---|---|---|
  | Gaban, Hindi | 6,908 / 6,908 / 6,920 (parse ~5,640) | 1,325 hits, 1,279 misses (50.9%), no clear |
  | its Latin twin | 2,644 / 2,648 / 2,673 | - |

  The ratio of medians is **2.61x**: real prose misses the 2x line. A word the cache misses still costs about 2.5 ms,
  and halving that is engine work (the conjunct ligature walks: HarfBuzz's second-glyph and ligature digests, never
  ported). A larger tail alone would reach about 2.2x. The maintainer accepted the miss on 2026-11-06 and moved the
  engine's performance work to a later PR, with this book as its benchmark. The Malayalam and Tamil build times wait
  for PR 3b, which installs those scripts' families.

**Item 6 on the X3** (`a0733646e`):

| What | master | Branch | Pass line |
|---|---|---|---|
| Background-B's lowest heap during the look-ahead build of the long Hindi chapter | - | 35,592 B (`Background-B: ... lowest`; no `Watermark DROP`) | >= 10,240 B: **pass** |
| `Min Free` since boot, after a cold Hindi build | 15,416 B | 15,432 B | - |
| The loop task's stack, after a cold Hindi build (diagnostics build) | 10,256 B free | 10,480 B free | >= 1,536 B free: **pass** |
| Reading heap after five turns, Hindi chapter | - | heap 60,384, maxblock 47,092 | - |
| Reading heap after five turns, English chapter | - | heap 60,388, maxblock 47,092 | - |
| After `CMD:TTF:FULL` and three turns forward and back, Hindi / English | - | 53,980 / 53,984 | within 2 KB: **pass** |
| The arena during the cold Hindi build | cap 52,272, high water 43,108 | cap 52,272, high water 43,116, parse lane 11,437, `failedAlloc=0` | - |

- No build logged `shaping degraded`.
- **Ring mode:** each page after `FULL` matched its screenshot from before, above the status bar. But the glyph store
  had one live segment (48 KB of 1.3 MB), so `FULL` resumed writes at the next idle step, before the turns were drawn.
  Drawing in ring mode itself was not exercised. That needs a store with two live segments.

**Before and after** (the images in the PR):
- `/test/test_hindi_shaping.epub@1:1` at 14 pt and 18 pt;
- `/test/test_shaped_bengali.epub@1:1` at 14 pt;
- the dictionary's word selection on the Hindi page.

On the branch, conjuncts are formed, vowel signs and the reph sit in place, and no dotted circle or stray virama
appears. The dictionary's box draws the word shaped. The branch's pages are identical, above the status bar, between
`c3597cfbd` and `a0733646e`: the cache and the memos change no output on the device. The status bar's chapter title
stays unshaped: UI strings wait for step 5 (the design's "Shaped UI strings").

**Flash and RAM** against master `50b8156b8`:

| Env | Flash, master | Flash, branch | Delta | RAM, master | RAM, branch | Delta |
|---|---|---|---|---|---|---|
| `gh_release` | 6,092,055 | 6,161,987 | +69,932 | 57,384 | 57,408 | +24 |
| `default` | 6,258,991 | 6,288,889 | +29,898 | 57,888 | 57,912 | +24 |

- `gh_release`'s delta, split from the map and the symbols:
  - **the engine, about +41.4 KB:** `lib/OtShaper`'s input sections are 45,834 B in the branch's map, against
    4,424 B of `ot::` symbols on master;
  - **the rest, about +28.5 KB:** TextShaping, the Indic module, the paragraph store and the word cache (their
    symbols grew from 456 B to 14,509 B), plus ParsedText, the parser, the section and padding.
- `default` already linked the engine through `TTF:SHAPE`, so its delta is the layout side alone.
- The gate (Global Constraints): `gh_release`'s +69,932 B is within +81,772 B, and `default`'s 6,288,889 B is within
  6,297,600 B. That leaves 264,711 B of the app slot free (>= 256,000 B).
- After merging master `17e2ba942` (#634-#636) and the final review's fixes (`33b1cf0e7`): `default` 6,294,083 B,
  3,517 B under the gate, with 259,517 B of the slot free; RAM 57,592 B (master moved its parser tables to flash).

**Estimate -> measured** for the design's rows ("Memory, flash and time") that this PR measures:

| What | The design's estimate | Measured |
|---|---|---|
| Glyph buffer per call | about 6.3 KB for n = 32 | one `IndicShaper::CallMemory`: about 6.3 KB plus 7 B a token byte (about 6,450 B for an 18-byte token), a block of the lent arena released at the call's end, else the build's heap block |
| A plan | about 1.5 KB (Devanagari), 2.1 KB (Bengali) | 980 B on the X3 before Task 12a (merged Devanagari, `TTF:SHAPE`). With the digest copy and the consonant memo, on the host: 3,328 B (merged Devanagari), 2,592 / 4,128 / 1,056 / 1,184 B (the four fixtures). 24 B a planned lookup plus 320 B; heap, the build |
| Paragraph glyph store | about 8 B a glyph while laid out | `ShapedStore`: heap chunks kept across paragraphs; the long Hindi chapter's records are 38,440 B of glyphs over the chapter (host), held only while their paragraph is laid out |
| Resident page | 4-6 B a glyph of form, against 8-12 B for #3787's tokens | `page_bytes` mean / max: 5,196 / 6,691 B (test_shaped_devanagari) and 10,420 / 12,288 B (the long Hindi chapter's host pages) against 3,228 / 4,122 B and 5,904 / 7,278 B unshaped; the glyph records are half #3787's token bytes (38,440 against 76,880 B) |

## The gate

| Item | Pass line | Measured | Result |
|---|---|---|---|
| 1 | 0 differences from HarfBuzz on upstream's corpus under our compiler and flags | 216 of 216 runs N/N; 3,436,140 lines, 0 mismatches | **pass** |
| 2 | E + A <= 92,160 B | E = 52,268 B; E + A = 67,628 B | **pass** |
| 2 | 6,553,600 - (`default` with the call + A + P2) >= 256,000 B | 303,587 B free (with P2 = 20,480 B, provisional: step 1 PR 2 has no figure yet) | **pass (provisional)** |
| 3 | all 22 families merged | 22 of 22 (decision 6 refined: `vhea`/`vmtx` dropped) | **pass** |
| 3 | both checks hold | equal against the script's own font: 1,531,416 of 1,531,416 lines (112 runs); Latin kept whole: 792 of 792 letter runs equal against Noto Sans/Serif | **pass** |
| 3 | every family fits, in flash or SD mode | 22 of 22, all in flash mode | **pass** |
| 2 (PRs 1-3) | the release build's running total <= 92,160 B | PR 2 +10,388 B, PR 3 +69,932 B: 80,320 B | **pass** |
| 6 | a shaped chapter builds in at most twice the time of a Latin chapter of the same length (X3) | the timing book (repeated text): 1.95x (4,931 against 2,529 ms) with the word cache and the memos, 4.20x without; real Hindi prose (Gaban): 2.61x (6,908 against 2,648 ms) | **pass on the timing book; missed on real prose**, accepted by the maintainer (2026-11-06), the engine's speed moved to a later PR |
| 6 | Background-B's lowest heap during an Indic build >= 10,240 B (X3) | 35,592 B | **pass** |

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

## PR 3b: the shipped families against HarfBuzz

Task 4 of PR 3b. `test/ot_shaper/oracle_shipped_fonts.py` shapes `generate_corpus.py`'s full corpus for each script
(11,203 to 20,522 lines a script, 152,064 across the ten) with HarfBuzz 14.5.0 (uharfbuzz 0.56.2, fontTools
4.63.0) and with `ot_shape_cli ... cpac`, which writes the font's `CPac` with `ot::FilterTableWriter` as the install
does and shapes through it. It runs every one of the 40 shipped files (20 families, Regular and Bold) at (2133, 33)
and (1195, 19), with no language and with every language written in the script (Devanagari 13, Bengali 3, one for
each of the other eight).

**Result: 272 runs, every one N/N; 0 differences in 4,640,768 line comparisons** (the bar is 0).
Every run covers the whole corpus of its script:

| Family | Style | Runs | Lines a run | Identical |
|---|---|---|---|---|
| NotoSansDevanagari | Regular | 28 (2 sizes x 14 language settings) | 20,522 | all |
| NotoSansDevanagari | Bold | 28 (2 sizes x 14 language settings) | 20,522 | all |
| NotoSerifDevanagari | Regular | 28 (2 sizes x 14 language settings) | 20,522 | all |
| NotoSerifDevanagari | Bold | 28 (2 sizes x 14 language settings) | 20,522 | all |
| NotoSansBengali | Regular | 8 (2 sizes x 4 language settings) | 14,852 | all |
| NotoSansBengali | Bold | 8 (2 sizes x 4 language settings) | 14,852 | all |
| NotoSerifBengali | Regular | 8 (2 sizes x 4 language settings) | 14,852 | all |
| NotoSerifBengali | Bold | 8 (2 sizes x 4 language settings) | 14,852 | all |
| NotoSansGurmukhi | Regular | 4 (2 sizes x 2 language settings) | 14,545 | all |
| NotoSansGurmukhi | Bold | 4 (2 sizes x 2 language settings) | 14,545 | all |
| NotoSerifGurmukhi | Regular | 4 (2 sizes x 2 language settings) | 14,545 | all |
| NotoSerifGurmukhi | Bold | 4 (2 sizes x 2 language settings) | 14,545 | all |
| NotoSansGujarati | Regular | 4 (2 sizes x 2 language settings) | 14,668 | all |
| NotoSansGujarati | Bold | 4 (2 sizes x 2 language settings) | 14,668 | all |
| NotoSerifGujarati | Regular | 4 (2 sizes x 2 language settings) | 14,668 | all |
| NotoSerifGujarati | Bold | 4 (2 sizes x 2 language settings) | 14,668 | all |
| NotoSansOriya | Regular | 4 (2 sizes x 2 language settings) | 14,891 | all |
| NotoSansOriya | Bold | 4 (2 sizes x 2 language settings) | 14,891 | all |
| NotoSerifOriya | Regular | 4 (2 sizes x 2 language settings) | 14,891 | all |
| NotoSerifOriya | Bold | 4 (2 sizes x 2 language settings) | 14,891 | all |
| NotoSansTamil | Regular | 4 (2 sizes x 2 language settings) | 11,203 | all |
| NotoSansTamil | Bold | 4 (2 sizes x 2 language settings) | 11,203 | all |
| NotoSerifTamil | Regular | 4 (2 sizes x 2 language settings) | 11,203 | all |
| NotoSerifTamil | Bold | 4 (2 sizes x 2 language settings) | 11,203 | all |
| NotoSansTelugu | Regular | 4 (2 sizes x 2 language settings) | 15,680 | all |
| NotoSansTelugu | Bold | 4 (2 sizes x 2 language settings) | 15,680 | all |
| NotoSerifTelugu | Regular | 4 (2 sizes x 2 language settings) | 15,680 | all |
| NotoSerifTelugu | Bold | 4 (2 sizes x 2 language settings) | 15,680 | all |
| NotoSansKannada | Regular | 4 (2 sizes x 2 language settings) | 14,916 | all |
| NotoSansKannada | Bold | 4 (2 sizes x 2 language settings) | 14,916 | all |
| NotoSerifKannada | Regular | 4 (2 sizes x 2 language settings) | 14,916 | all |
| NotoSerifKannada | Bold | 4 (2 sizes x 2 language settings) | 14,916 | all |
| NotoSansMalayalam | Regular | 4 (2 sizes x 2 language settings) | 17,630 | all |
| NotoSansMalayalam | Bold | 4 (2 sizes x 2 language settings) | 17,630 | all |
| NotoSerifMalayalam | Regular | 4 (2 sizes x 2 language settings) | 17,630 | all |
| NotoSerifMalayalam | Bold | 4 (2 sizes x 2 language settings) | 17,630 | all |
| NotoSansSinhala | Regular | 4 (2 sizes x 2 language settings) | 13,157 | all |
| NotoSansSinhala | Bold | 4 (2 sizes x 2 language settings) | 13,157 | all |
| NotoSerifSinhala | Regular | 4 (2 sizes x 2 language settings) | 13,157 | all |
| NotoSerifSinhala | Bold | 4 (2 sizes x 2 language settings) | 13,157 | all |

Re-run on 2026-11-07 after the 40 files gained the IAST letters (24 precomposed letters and the 6 combining marks
their decomposed forms use; decision 6 refined): the same 272 runs, every one N/N, 0 differences, and the output is
line for line the output above. The corpus has no Latin, so the Indic shaping cannot move; the build's Latin check
covers the new letters. The 40 files grew from 8,102,572 B to 8,368,280 B in all (the largest,
NotoSerifDevanagari-Regular.ttf, from 293,268 B to 302,032 B).


`ShippedFamilies.EachIndicFamilyShapesAlikeThroughItsCpac` is the CI form of the same check: the Regular file of each of
the 20 families, the first 200 lines of its script's corpus (a committed sample in `test/ttf_font/data`, cut by
`test/ot_shaper/make_corpus_sample.py` with the format characters as backslash-u escapes), at both sizes, shaped on a
`CPac` face and on a face that computes its filters: glyphs, advances and offsets equal, about 0.15 s.

## Item 7: the Arabic shaper and the two Arabic families

Run on 2026-11-07, before any of PR 4's code, in the worktree of `feat/writing-modes-step2-pr4` at master `3ae95c07b`
(PR #645 merged). Only this section and deviation 11 of the design are committed: the HarfBuzz files, the scratch
scripts, the probe and the merged fonts stay in the session's scratch directory.

### Sources

**HarfBuzz 14.5.0.** The tag `14.5.0` (tag object `77a9ee80`, commit `863d3f7787c6df18d20e4535c5906bf3eb803bd5`, tagged
2026-09-20). Each file was fetched once from `https://raw.githubusercontent.com/harfbuzz/harfbuzz/14.5.0/src/<file>` and
read, never built or run:

| File | Bytes | sha256 (first 16) | Read for |
|---|---:|---|---|
| `hb-ot-shaper-arabic.cc` | 25,779 | `f58d84e3715daa1c` | the shaper: its features, the joining, `reorder_marks`, `stch` |
| `hb-ot-shaper-arabic-table.hh` | 21,921 | `8fef6413088130a7` | the joining types (Unicode 18.0); the fallback's shaping and ligature tables |
| `hb-ot-shaper-arabic-fallback.hh` | 13,076 | `7d2c1db44996591c` | the fallback shaping |
| `hb-ot-shaper-arabic-pua.hh` | 7,590 | `09dbda72b045984b` | the fallback's Windows-1256 private-use mapping |
| `hb-ot-shaper-arabic-joining-list.hh` | 1,090 | `cd80914c08f1be9c` | the scripts USE gives Arabic joining |
| `hb-ot-shaper-arabic.hh` | 1,648 | `080c66f82955bf2a` | `setup_masks_arabic_plan`, which USE shares |
| `hb-ot-shaper.hh` | 12,102 | `3eb84b0a89a8c4db` | the shaper struct, `hb_ot_shaper_categorize` |
| `hb-ot-shape.cc` | 39,900 | `c50cabe89c03daf9` | the default, common and horizontal features; every step that reads the direction |
| `hb-ot-map.cc`, `hb-ot-map.hh` | 12,733, 9,787 | `6b4b92b7f6e7af90`, `101d1b7eaa171a39` | feature merging, stages, `has_feature` |
| `hb-ot-shape-normalize.cc`, `hb-ot-shape-normalize.hh` | 15,660, 3,365 | `5cf41f87b8717f36`, `381d0a03e6051977` | the normalization modes, the `reorder_marks` hook |
| `hb-ot-layout.cc`, `hb-ot-layout.hh` | 94,185, 20,651 | `aaa35961fc106462`, `aaa549c0f07213cf` | the apply loop and its messages, the Unicode properties |
| `OT/Layout/GPOS/GPOS.hh` | 6,491 | `8752522c54b06c2d` | `propagate_attachment_offsets`, `position_finish_offsets` |
| `OT/Layout/GPOS/CursivePosFormat1.hh` | 11,278 | `c1eb9ea317dd9ab9` | cursive attachment |
| `hb-unicode.cc` | 17,370 | `2c4825a021548152` | the modified combining classes |
| `hb-ot-tag-table.hh` | 170,865 | `fe80a969cc25ddf2` | the language system tags |
| `gen-arabic-table.py` | 11,670 | `0e9196d232d31ea9` | how the joining table is generated |

`hb-ot-layout-gpos-table.hh` only includes `OT/Layout/GPOS/GPOS.hh`. The other GPOS headers (`MarkArray.hh`,
`ValueFormat.hh`, the mark attachment formats) were read for the direction: they tell horizontal from vertical only.

**Fonts.** The pinned files of "Sources" above (`C:/tmp/wm2-measure/dl/fonts`), their sha256 checked again: Noto Naskh
Arabic, Noto Sans Arabic, Noto Serif and Noto Sans, Regular and Bold. **Tools** as above: fontTools 4.63.0, uharfbuzz
0.56.2 (HarfBuzz 14.5.0), g++ 16.2.0, riscv32-esp-elf-g++ 14.2.0.

### The shaper, as `hb-ot-shaper-arabic.cc` has it

`_hb_ot_shaper_arabic`:

| Field | 14.5.0 |
|---|---|
| `collect_features` | `collect_features_arabic` (below) |
| `override_features` | none |
| `data_create`, `data_destroy` | the seven forms' masks, whether the plan has `stch`, whether the fallback shaping runs |
| `preprocess_text` | none |
| `postprocess_glyphs` | `apply_stch` |
| `decompose`, `compose` | none: Unicode's |
| `setup_masks` | `setup_masks_arabic`: the joining state machine |
| `reorder_marks` | `reorder_marks_arabic` |
| `gpos_tag` | none |
| `normalization_preference` | `DEFAULT` (`AUTO`: composed diacritics, with the short circuit, as the default shaper) |
| `zero_width_marks` | `BY_GDEF_LATE` |
| `fallback_position` | true |

`hb_ot_shaper_categorize` gives Arabic text in a horizontal direction this shaper whatever script tag the font's GSUB
has, `DFLT` included ("we do fallback shaping for Arabic script"); Syriac only with a script tag of its own. Mongolian,
N'Ko and Phags-pa are USE's in 14.5.0, which calls `setup_masks_arabic_plan` for them (`has_arabic_joining`).

**The features and stages.** `hb_ot_shape_collect_features` requests the default features, then the shaper's, then the
common and horizontal ones. GSUB stages count from 0, for a language system without `rclt` (both Arabic families').
"Masked" is `add_feature` (a mask bit the shaper or the text sets), "global" `enable_feature`. A lookup skips ZWJ and
ZWNJ ("auto") unless a flag makes it manual.

| GSUB stage | Feature | Requested by | Flags as requested | Then |
|---:|---|---|---|---|
| 0 | `rvrn` | default | global | pause |
| 1 | `rtla` | default, right to left (left to right: `ltra`) | global | |
| 1 | `rtlm` | default, right to left (left to right: `ltrm`, global) | masked | |
| 1 | `frac`, `numr`, `dnom` | default | masked | |
| 1 | `rand` | default | global, random, value 255 | |
| 1 | `Harf`, `HARF` | default | global | |
| 1 | `stch` | Arabic | global | pause, `record_stch` |
| 2 | `ccmp`, `locl` | Arabic | global, manual ZWJ | pause |
| 3 | `isol` | Arabic | masked, manual ZWJ, has fallback | pause |
| 4 | `fina` | Arabic | masked, manual ZWJ, has fallback | pause |
| 5 | `fin2` | Arabic | masked, manual ZWJ | pause |
| 6 | `fin3` | Arabic | masked, manual ZWJ | pause |
| 7 | `medi` | Arabic | masked, manual ZWJ, has fallback | pause |
| 8 | `med2` | Arabic | masked, manual ZWJ | pause |
| 9 | `init` | Arabic | masked, manual ZWJ, has fallback | pause |
| 10 | none | | | pause, `deallocate_buffer_var` |
| 11 | `rlig` | Arabic | global, manual ZWJ, has fallback | pause, `arabic_fallback_shape` (Arabic only, not Syriac) |
| 12 | `calt` | Arabic | global, manual ZWJ | pause, unless the language system has `rclt` (`has_feature`, HarfBuzz #1573) |
| 13 | `liga`, `clig`, `mset` | Arabic | global, manual ZWJ | |
| 13 | `Buzz`, `BUZZ` | default | global | |
| 13 | `abvm`, `blwm`, `ccmp`, `locl`, `rlig` | common | global | |
| 13 | `mark`, `mkmk` | common | global, manual ZWJ and ZWNJ | |
| 13 | `calt`, `clig`, `curs`, `dist`, `liga`, `rclt` | horizontal | global | |
| 13 | `kern` | horizontal | global, has fallback | |
| | | `hb_ot_map_builder_t::compile` | | pause: 14 GSUB stages (13 with `rclt`), 1 GPOS stage |

- Has fallback (`F_HAS_FALLBACK`): the feature gets its mask bit even when the font lacks it. The seven forms have it in
  an Arabic run, except `fin2`, `fin3` and `med2` (`FEATURE_IS_SYRIAC`).
- A feature requested twice is merged (`compile`): the earliest stage wins, global and has-fallback are combined, and
  the joiner flags are the first request's. So `ccmp` and `locl` run in stage 2, `rlig` in 11, `calt` in 12, `liga` and
  `clig` in 13, all with manual ZWJ. `rclt`, requested only as a horizontal feature, keeps auto ZWJ and runs with
  `liga`: in stage 12, as a language system that has `rclt` gets no pause after `calt`. `abvm`, `blwm`, `mark`, `mkmk`,
  `curs`, `dist` and `kern` are in GPOS's one stage. `cswh` is commented out ("Off by default").
- The pauses: `record_stch` marks each glyph that `stch` multiplied as a fixed or a repeating tile, when the plan has
  `stch`; `deallocate_buffer_var` frees the shaping action's byte; `arabic_fallback_shape` runs the fallback shaping
  when the plan says so. All three return false, so none refreshes the buffer's digest.

**The joining state machine** (`arabic_joining`, run by `setup_masks`): a 7 x 6 table of (the action for the previous
glyph, the action for this one, the next state), over the joining classes U, L, R, D (C counts as D), Alaph and
Dalath-Rish (the last two Syriac's).
- Each character's class is `joining_type()`; a code point the table does not list (X) is T when its general category is
  Mn, Me or Cf, else U. T characters are skipped, with no action.
- The first non-T character of the pre-context sets the starting state, and the first of the post-context can still
  give the run's last glyph a joining form. Neither is shaped.
- The action (`isol`, `fina`, `fin2`, `fin3`, `medi`, `med2`, `init` or none) picks the mask the glyph gets
  (`mask_array[action]`, that feature's 1-mask). Mongolian free variation selectors copy their base's action (Mongolian
  only).
- The table is `hb-ot-shaper-arabic-table.hh`, written by `gen-arabic-table.py ArabicShaping.txt UnicodeData.txt
  Blocks.txt` (headers ArabicShaping-18.0.0, Blocks-18.0.0) with packTab: for each code point ArabicShaping.txt lists,
  its joining type (field 2), or its joining group when that is ALAPH or DALATH RISH; X elsewhere and past U+1E94B. The
  same file holds the fallback's shaping table (U+0621-06D3 to their presentation forms) and ligature tables, read from
  UnicodeData.txt's decompositions.

**`reorder_marks_arabic`** (UTR #53), which the normalizer calls on each run of marks once it is sorted by combining
class (32 marks at most): for class 220, then 230, the run of modifier combining marks of that class (U+0654, 0655,
0658, 06DC, 06E3, 06E7, 06E8, 08CA, 08CB, 08CD, 08CE, 08CF, 08D3, 08F3) where the class starts moves to the front of
the mark run, their clusters merged. The moved marks are renumbered to the modified classes 25
(`HB_MODIFIED_COMBINING_CLASS_CCC22`) and 26 (`CCC26`), below every Arabic class, so the run stays sorted for later
passes. Those classes are HarfBuzz's modified ones: shadda is 27, before fatha's 31.

**What is left out, and what it does.**
- `stch` (`record_stch` in its pause, `apply_stch` as `postprocess_glyphs`): a font's `stch` lookup splits a glyph (the
  Syriac Abbreviation Mark, the Arabic subtending marks of U+0600-0605) into tiles. After positioning, the repeating
  tiles are copied until they span the rest of the word, at most 256 glyphs, overlapping to fit. Neither Arabic family
  has `stch` (below).
- The fallback shaping (`arabic_fallback_shape`, `hb-ot-shaper-arabic-fallback.hh`): when an Arabic run's font has none
  of `isol`, `fina`, `medi` and `init`, the plan builds seven lookups (single substitutions to the cmap's presentation
  forms for `init`, `medi`, `fina` and `isol`, and three ligature lookups for `rlig`: three letters, two letters with
  lam-alef among them, and marks), or the Windows-1256 private-use set for a font that looks like one, and applies them
  in the pause after `rlig`. Both families have the features, so HarfBuzz runs no fallback on them either.
- `fallback_position`: for a font without GPOS (or with no GPOS `kern` in the language system), a legacy `kern` table
  is applied, and without GPOS marks are placed from their glyph extents and combining classes
  (`_hb_ot_shape_fallback_mark_position`). Both families have GPOS and no `kern` table.
- The glyph flags (`unsafe_to_concat`, `safe_to_insert_tatweel`): output flags the engine does not produce.

### Direction: what `hb_ot_shape` does differently for a right-to-left run

| Step (14.5.0) | Right to left | `lib/OtShaper` today |
|---|---|---|
| `hb_ot_shape_collect_features` | `rtla` (global) and `rtlm` (masked) in place of `ltra` and `ltrm` | `ltra` and `ltrm` always (`collectDefaultFeatures`) |
| `hb_ensure_native_direction` | an Arabic run given left to right is reversed by graphemes and shaped right to left, unless it holds digits and no letter | none: the caller gives the direction |
| `hb_ot_rotate_chars` | each character's Unicode mirror when the font maps it, else the `rtlm` mask | none |
| `hb_ot_shape_setup_masks_fraction` | the numerator and denominator masks change sides of U+2044 | no fraction masks (no U+2044 in a run) |
| GSUB | the same: lookups run in logical order (type 8 backward, in either direction) | the same |
| GPOS value records, pair and mark attachment | the same: they read horizontal against vertical only | the same |
| `CursivePosFormat1::apply` | `RTL`: the previous glyph's advance and x offset shrink by its exit anchor plus its x offset, and the current glyph's advance becomes its entry anchor plus its x offset | the `LTR` branch only (`applyCursivePos`) |
| `zero_mark_widths_by_gdef` without GPOS | the marks' offsets are not shifted by their advance (`adjust_offsets_when_zeroing` is forward only) | shifted in either direction |
| `GPOS::position_finish_offsets` | `propagate_attachment_offsets` walks the glyphs from the last to the first (HarfBuzz #5514); a mark after its base adds the advances of the glyphs after the base, up to and including the mark, where forward subtracts those from the base up to the mark | the forward walk and branches only (`propagateAttachmentOffsets`) |
| `hb_ot_position` | the glyphs and their positions reversed at the end, into visual order | none (`Buffer::reverseRange` swaps the glyph infos, not the positions) |
| `hb_ot_substitute_post` | default-ignorables hidden after the reverse | the same order |
| `hb_insert_dotted_circle` | a dotted circle before a leading mark, only with `HB_BUFFER_FLAG_BOT`, in either direction | none; the oracle sets no flags |

### Deviation 11 against the source

| Deviation 11 as drafted (from memory) | HarfBuzz 14.5.0 | Verdict |
|---|---|---|
| the seven joining forms each in its own stage | yes: each masked, with manual ZWJ, a pause after each and one more after `init`; `isol`, `fina`, `medi` and `init` have fallback | right; flags added |
| `rlig`, `rclt` and `calt` with manual ZWJ | `rlig` (manual ZWJ, has fallback) in a stage of its own, then `calt` (manual ZWJ) and a pause unless the font has `rclt`; `rclt` is not the shaper's: a horizontal feature, with auto ZWJ | `rclt` wrong |
| `mset` | `mset` with `liga` and `clig`, all with manual ZWJ | `liga`, `clig` missing |
| (none) | `stch` and a pause; `ccmp` and `locl` with manual ZWJ in a stage of their own before the forms | missing |
| the common features and GPOS as for every script | yes, after `rtla` and `rtlm` in place of `ltra` and `ltrm` | `rtla`, `rtlm` missing |
| marks zeroed late by GDEF | `BY_GDEF_LATE` | right |
| the reordering of modifier combining marks | yes: 14 marks, class 220 then 230, renumbered 25 and 26 | right; details added |
| a buffer direction, `propagateAttachmentOffsets`' backward branches, the final reverse | yes, and `position_finish_offsets`' backward walk, cursive attachment's RTL branch, the `rtlm` mask, no offset shift when zeroing | incomplete |
| the Unicode tables extended (categories, combining classes, decompositions, joining types); `ar`, `fa`, `ur`, `ps`, `sd`, `ug`, `ku` | yes; the classes are HarfBuzz's modified ones, and default-ignorables and compositions are needed too; the tags ARA, FAR, URD, PAS, SND, UYG and KUR | right; details added |
| not ported: the fallback shaping, `stch`, Syriac, Mongolian, N'Ko, Phags-pa | the fallback and `stch` as said; also the mark placement and legacy `kern` that `fallback_position` gives a font without GPOS; Mongolian, N'Ko and Phags-pa are USE's | one item added |
| (none) | normalization `DEFAULT`, `fallback_position` true, the shaper whatever the script tag | added |

Deviation 11 of the design now carries the list as read.

**The engine changes it takes** (the engine is left to right only today):
- `OtPlan.h`, `OtPlan.cpp`: `ShaperKind::Arabic`, `Script::Arabic` (`arab`), the plan's direction, and the masks of the
  seven forms and `rtlm` kept outside `MaskedFeature`, whose order is the `CPpl` format's;
- `OtPlanBuild.cpp`: the direction's default features, `collect_features_arabic`, `has_feature` for `rclt`, the Arabic
  shaper whatever the script tag;
- `OtShaper.cpp`: no vowel constraints for Arabic, the `rtlm` mask, the joining masks, marks zeroed late, the direction
  to GPOS, and the final reverse of `info` and `pos`;
- a new `OtArabic.cpp`: the state table, `arabic_joining`, `reorder_marks_arabic`;
- `OtNormalize.cpp`: the default shaper's short circuit and Unicode's composition for Arabic, the `reorder_marks` hook,
  tables of several ranges;
- `OtGpos.cpp`: cursive attachment's RTL branch, `propagateAttachmentOffsets`' backward walk and branches;
- `OtBuffer.h`: a direction, a setter for the modified combining class, a reverse of glyphs with their positions. The
  shaping action can live in `GlyphInfo::position`: HarfBuzz keeps Indic's position and Arabic's action in the same
  byte (`ot_shaper_var_u8_auxiliary`);
- `scripts/gen_ot_shaper_tables.cc` and `OtUnicodeData.h`: the Arabic blocks (U+0600-06FF, 0750-077F, 0870-089F,
  08A0-08FF, FB50-FDFF, FE70-FEFF), their joining types in a table of their own (the packed properties have 2 bits
  free, a joining type takes 3), ZWJ's joining type (C) and ZWNJ's (U), and the Arabic languages.

### The two families with the current recipe

**How.** `merge_noto_latin.merge()` as it is at `3ae95c07b` (the IAST letters included): Noto Naskh Arabic with Noto
Serif and Noto Sans Arabic with Noto Sans, Regular with Regular and Bold with Bold, by a scratch script on the pinned
fontTools and uharfbuzz.
- The Latin check: the module's `check()` with no corpus, so only its "Latin kept whole" half runs. Its script half
  shapes left to right as an Indic script, which does not apply to Arabic.
- The script check: there is no Arabic corpus yet (upstream's `generate_corpus.py` is Indic). Shaped instead: item 3's
  `corpus/arabic.txt` (577 strings: 141 letters in four positions, the lam-alef pairs, the harakat), and 22 lines of
  Arabic, Persian, Urdu, Pashto, Sindhi, Uyghur and Kurdish words written by code point (vocalised words, tatweel,
  lam-alef with marks, the hamza compositions, the modifier marks, ZWJ and ZWNJ, digits and punctuation). HarfBuzz
  14.5.0 at (2133, 33), right to left, `Arab`, with no language and with each of `ar`, `fa`, `ur`, `ps`, `sd`, `ug` and
  `ku`: the merged font against the script font, glyph IDs, advances and offsets.
- The sizes: a host probe linking `lib/OtShaper` and `lib/EpdFont/ttf` (the library files of `TtfFontTest`), g++ 16.2.0
  with `-std=c++20 -O1 -fno-exceptions`. `CPac` as `ot::FilterTableWriter` writes it (and `Face::init` took it), what
  `ttf::probeFile` reserves, and `ttf::fitsStore` in flash mode on the X3's font area (3,538,944 - 65,536 B, 32 free map
  pages), as `ShippedTtfFamilies.EachFileLoadsAndEachFamilyFitsTheX3Store` does.

| Family | Style | bytes | glyphs | GDEF | GSUB lookups / subtables | GPOS lookups / subtables | `CPac` (writer) | probe `cpacBytes` | flash bytes | SD bytes | IAST mapped | script check | Latin check |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|
| NotoNaskhArabic | Regular | 261,600 | 2,315 | 742 | 67 / 79 | 32 / 34 | 5,500 | 5,500 | 267,100 | 71,516 | 30 of 30 | 4,792 of 4,792 (599 lines x 8) | 62 of 62 runs equal |
| NotoNaskhArabic | Bold | 261,352 | 2,315 | 724 | 67 / 79 | 32 / 34 | 5,500 | 5,500 | 266,852 | 71,536 | 30 of 30 | 4,792 of 4,792 | 62 of 62 runs equal |
| NotoSansArabic | Regular | 232,704 | 2,322 | 608 | 64 / 74 | 32 / 34 | 5,296 | 5,296 | 238,000 | 68,568 | 30 of 30 | 4,792 of 4,792 | 62 of 62 runs equal |
| NotoSansArabic | Bold | 233,044 | 2,322 | 630 | 64 / 74 | 32 / 34 | 5,296 | 5,296 | 238,340 | 69,144 | 30 of 30 | 4,792 of 4,792 | 62 of 62 runs equal |

- The merged files' sha256: Naskh Regular `21acf0334143ff85a709ef1762694d083f9c70fb521fea9fae0e04cefd297f14`, Naskh Bold
  `8c7a340fd635c06f558696d6fde63ab557b43eae8de568151213a968cfe09137`, Sans Regular
  `13e9cea7a74e6288791f5927c848150b5ac5c24676bef6170a40e20073c1904e`, Sans Bold
  `a2bfc7d16c4077c41957ff81d6ed78de2084d021cedff7a2cc4773bd7bc8bdfb`.
- The install already gives these files a `CPac`: `ttf::filterTableBytes` counts one for a font whose script list
  names `arab` (`isShapedTag`, PR 2), and the probe's `cpacBytes` equals what the writer writes.
- Against item 3's merges (the recipe before the IAST letters), the files grew by 8,944, 8,908, 4,956 and 5,068 B, and
  `CPac` from 5,084 to 5,500 B (Naskh) and from 5,036 to 5,296 B (Sans): Noto's Latin brings 2 GSUB and 6 GPOS lookups
  more into Noto Naskh Arabic and 5 GPOS lookups into Noto Sans Arabic. GDEF grew from 652 to 742 B (Naskh Regular).

**The fit:**

| Family | flash bytes, Regular + Bold | `fitsStore`, flash mode | `planLayout` | log (B) |
|---|---:|---|---|---:|
| NotoNaskhArabic | 267,100 + 266,852 | true | flash | 1,376,256 |
| NotoSansArabic | 238,000 + 238,340 | true | flash | 1,441,792 |

### The lookups the Arabic features select, and the plan

**How.** HarfBuzz's apply loop sends `start lookup <index> feature '<tag>'` for every planned lookup of every stage, in
plan order, before it tests the lookup's digest (`hb_ot_map_t::apply`, `hb-ot-layout.cc`). So one shaping of any text
lists a plan: uharfbuzz's `Buffer.set_message_func`, the text U+0628 U+0628, right to left, `Arab`, each language. A
lookup planned in two stages is listed twice, as our plan holds it twice.
- Checked on the shipped Noto Sans Devanagari, both styles, with no language and `hi`, `mr`, `ne` and `sa`: HarfBuzz
  lists 53 / 24, 53 / 24, 58 / 24, 57 / 24 and 57 / 24 GSUB / GPOS lookups, and our `Plan::build` holds as many in each.
- The plan's bytes as `Plan::memoryBytes` counts them with a `CPac` (no computed filters): `sizeof(Plan)`, then 24 B a
  planned lookup (its digest, deviation 13) and 8 B (`PlannedLookup`), and 4 B a stage. `sizeof(Plan)` is 792 B on the
  host and 600 B on the C3 (riscv32-esp-elf-g++ 14.2.0, `-Os`). The probe measured 3,328 B for Devanagari on the host,
  as the formula gives. An Arabic plan holds 14 GSUB stages (neither font has `rclt`) and 1 GPOS stage. The masks of the
  seven forms and `rtlm` are not in `Plan` yet: at most 32 B more.

| Font (Regular and Bold alike) | Language system | Languages | GSUB lookups planned (by feature) | GPOS lookups planned (by feature) | Plan, host (B) | Plan, C3 (B) |
|---|---|---|---|---|---:|---:|
| NotoNaskhArabic | default | none, `ar`, `ps`, `ug` | 20 (`rtlm` 1, `ccmp` 4, `fina` 1, `medi` 2, `init` 2, `rlig` 9, `liga` 1) | 24 (`mark` 18, `mkmk` 6) | 2,260 | 2,068 |
| NotoNaskhArabic | FAR | `fa` | 21 (and `locl` 1) | 24 | 2,292 | 2,100 |
| NotoNaskhArabic | URD | `ur` | 21 (and `locl` 1) | 24 | 2,292 | 2,100 |
| NotoNaskhArabic | SND | `sd` | 22 (and `locl` 1, `rlig` 10) | 24 | 2,324 | 2,132 |
| NotoNaskhArabic | KUR | `ku` | 21 (`rlig` 10) | 24 | 2,292 | 2,100 |
| NotoSansArabic | default | none, `ar`, `fa`, `ps`, `ug` | 19 (`rtlm` 1, `ccmp` 4, `fina` 1, `medi` 2, `init` 2, `rlig` 8, `liga` 1) | 22 (`kern` 1, `mark` 16, `mkmk` 5) | 2,164 | 1,972 |
| NotoSansArabic | URD | `ur` | 20 (and `locl` 1) | 22 | 2,196 | 2,004 |
| NotoSansArabic | SND | `sd` | 21 (and `locl` 1, `rlig` 9) | 22 | 2,228 | 2,036 |
| NotoSansArabic | KUR | `ku` | 20 (`rlig` 9) | 22 | 2,196 | 2,004 |
| NotoSansDevanagari (shipped) | default | none, `hi` | 53 | 24 | 3,328 | 3,136 |
| NotoSansDevanagari (shipped) | MAR; NEP, SAN | `mr`; `ne`, `sa` | 58; 57 | 24 | 3,488; 3,456 | 3,296; 3,264 |

- One lookup serves both `medi` and `init` in each font, so it is planned twice: Noto Naskh Arabic's 20 GSUB entries
  are 19 lookups.
- The fonts' `arab` language systems: Noto Naskh Arabic has FAR, KSH, KUR, RHG, SND and URD; Noto Sans Arabic the same
  without FAR. ARA (`ar`), PAS (`ps`) and UYG (`ug`) are in neither, and Persian in Noto Sans Arabic takes the default
  system, as in HarfBuzz. HarfBuzz also maps `ks` to KSH and `ckb`, `kmr` and `sdh` to KUR.
- Of the features `collect_features_arabic` asks for, the fonts' GSUB has `ccmp`, `locl`, `fina`, `medi`, `init`,
  `rlig`, `liga` and `rtlm`, and no `stch`, `isol`, `fin2`, `fin3`, `med2`, `calt`, `rclt`, `clig` or `mset`. Their
  `rtlm` lookup maps one glyph, U+221A's. GPOS has `mark` and `mkmk` (and `kern` in Noto Sans Arabic), no `curs`, and
  neither file has a `kern` table. So no shipped font runs cursive attachment or a fallback: those paths of the port
  need tests of their own.
- The merge changes no Arabic plan: the script files plan the same lookups under `arab` (their lookups keep their
  indices in the merge).

**How far the glyphs move.** HarfBuzz at each font's upem over the same 599 lines, right to left, no language. The
largest offset is 1,035 units, an x offset in Noto Sans Arabic Bold (a lone U+0636: its dot is a mark of its own after
`ccmp`). So the records' whole-pixel `i8` dx and dy (decision 4) hold every offset up to 123 px a em; the largest native
size is 120 px (`TtfFont::kMaxNativePx`).

| Font | largest x offset (units) | largest y offset (units) | largest advance (units) | every offset fits `i8` up to (px a em) |
|---|---:|---:|---:|---:|
| NotoNaskhArabic-Regular | 655 | 809 | 1,099 | 157 |
| NotoNaskhArabic-Bold | 670 | 815 | 1,187 | 156 |
| NotoSansArabic-Regular | 979 | 480 | 1,352 | 130 |
| NotoSansArabic-Bold | 1,035 | 530 | 1,442 | 123 |

### Verdict

- **Each merged family fits the font area: pass.** Both families, in flash mode, with their `CPac`: item 3's pass line,
  which the PR 4 families fall under. The in-place clause has nothing to measure in the fonts: the gate keys on the
  book's language (`InPlaceBuildGate`, `TypographyProfile::shapedScript`), which PR 4 extends to the Arabic languages.
- The merges hold: the script check on 19,168 of 19,168 line comparisons over the four files, the Latin check on 248 of
  248 runs. The engine's own oracle on Arabic waits for PR 4's engine and an Arabic corpus.
- The feature list and stages are read off HarfBuzz 14.5.0, and deviation 11 is corrected to them. An Arabic plan is
  1,972-2,132 B on the C3 (and the masks), against Devanagari's 3,136-3,296 B; `CPac` is 5,296-5,500 B a style.

## PR 4a: the Arabic shaper against HarfBuzz

The engine shapes Arabic right to left (Tasks 1-4); this is the large-scale proof, the same method as the Indic oracle of
PR 3b: HarfBuzz 14.5.0 (uharfbuzz 0.56.2, direction `rtl`, script `Arab`) against `ot_shape_cli` through the font's
`CPac` (the `cpac` argument), glyph IDs, advances and offsets, line by line.

### The corpus

`test/ot_shaper/generate_corpus_arabic.py` writes `arabic.txt`, deterministic (`random.Random(1757)`): **6,333 distinct
lines**. Every assigned code point of U+0600-06FF, 0750-077F, 0870-089F and 08A0-08FF alone; each letter isolated,
initial, medial and final beside beh; 40 base letters with each of U+064B-0652 and 0670 and with shadda plus each vowel in
both orders; the 14 modifier marks after beh with fatha and kasra in both orders; lam with the eight alefs, alone, after
beh, with a harakat, shadda, tatweel or a joiner between; joiner variants; Persian and Urdu letters inside words; 2,000
random sequences of 2-8 code points from the letters, marks, joiners and tatweel.

### The fonts

The four Arabic families as the Font Manager builds them (`merge_noto_latin.merge()` from the sources pinned at
notofonts.github.io 5c82cef, unhinted TTFs; the whole files, not the fixture subsets), and one real font with cursive
attachment.

| File | Source sha256 | Merged sha256 |
|---|---|---|
| NotoNaskhArabic-Regular + NotoSerif-Regular | c34fdbd98af4dbc45ca192a23d2eeb77032add83086f5fe32957d23b3f36b221, a15cfbbc1539d707115111d672d590a3d70d4f74b4c0a315956da20ae19a14e1 | 21acf0334143ff85a709ef1762694d083f9c70fb521fea9fae0e04cefd297f14 |
| NotoNaskhArabic-Bold + NotoSerif-Bold | 380ac34b249d336e1a43c21053df440594e1e3db8f75fabd4d85b649ca1ea4c6, 24ad531e6b05ddad8c3d89572d2c93eb86a6b74e652ce7ee3c3e171de68e84c3 | 8c7a340fd635c06f558696d6fde63ab557b43eae8de568151213a968cfe09137 |
| NotoSansArabic-Regular + NotoSans-Regular | bd86ca02f087d7f3c3788ba458fb6b73744c7639ed276b8d870dba6def6c40d0, f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8 | 13e9cea7a74e6288791f5927c848150b5ac5c24676bef6170a40e20073c1904e |
| NotoSansArabic-Bold + NotoSans-Bold | f4cb79f842a07d1bcf597879574358fb7010f73761396d1d0f2445dfedb4d2ca, 87cb2d84472a7d66da659ee47b6cdb9552326e8c128245231f191b6ac72529d9 | a2bfc7d16c4077c41957ff81d6ed78de2084d021cedff7a2cc4773bd7bc8bdfb |

NotoNastaliqUrdu-Regular.ttf (OFL; GPOS `curs`, `dist`, `kern`, `mark`, `mkmk`; 6 GSUB features), used as downloaded:
`https://raw.githubusercontent.com/notofonts/notofonts.github.io/5c82cef0a8f08806f932062e18c02be62be278f0/fonts/NotoNastaliqUrdu/unhinted/ttf/NotoNastaliqUrdu-Regular.ttf`,
176,600 B, sha256 `6decda3b03f6366dbd75e3a22544d248ca686b582f897fae8a7dc027302812e3`. All eight Noto sources are pinned by
sha256 in `make_arabic_fixtures.py` (it merges two fixture fonts from four of them and checks the other four when they are present); the merges of
the other four are not byte-compared to anything.

### The result

`oracle_arabic.py`: each font at (2133, 33) and (1195, 19), with no language and with `ar`, `fa`, `ur`, `ps`, `sd`, `ug`,
`ku`, `ckb`, `ks`: 5 fonts x 2 sizes x 10 languages = **100 runs, every one 6,333/6,333, 0 differences**.

| Font | 33 ppem, all 10 languages | 19 ppem, all 10 languages |
|---|---|---|
| NotoNaskhArabic Regular | 6,333/6,333 | 6,333/6,333 |
| NotoNaskhArabic Bold | 6,333/6,333 | 6,333/6,333 |
| NotoSansArabic Regular | 6,333/6,333 | 6,333/6,333 |
| NotoSansArabic Bold | 6,333/6,333 | 6,333/6,333 |
| NotoNastaliqUrdu Regular | 6,333/6,333 | 6,333/6,333 |

### The Indic re-run

`oracle_shipped_fonts.py` on the 40 shipped Indic files (corpus regenerated by `generate_corpus.py`): **272 runs, every
one N/N, 0 differences**: the Arabic plan, the CLI's script detection and `compare_harfbuzz.py` change nothing for Indic.

### Flash and RAM

Master `3ae95c07b` against this branch, `pio run` (the floor: `default` at most 6,318,600 B, 235,000 B free of the
6,553,600 B app partition).

| Env | Master Flash | Branch Flash | Delta | Master RAM | Branch RAM |
|---|---:|---:|---:|---:|---:|
| `default` | 6,297,547 B (PR 3b's final build) | 6,301,117 B | +3,570 B | 57,616 B | 57,616 B |
| `gh_release` | 6,170,745 B | 6,174,077 B | +3,332 B | 57,096 B | 57,096 B |

`default` has 252,483 B free: the gate holds with 17,483 B to spare. RAM is unchanged. The bytes (`nm --size-sort` on the
`default` ELF against master's `gh_release` ELF, symbols new in the branch): the Arabic joining table (`ucd::JOINING`,
224 B, over the 448 code points of the Arabic blocks), `reorderMarksArabic` (750 B), `arabicSetupMasks` (228 B), `Buffer::reverse` (130 B), the joining state table
(126 B), `joiningType` (66 B), the modifier-mark list (56 B) and the plan's form tags; about 2.2 KB in 17 new symbols (the 2.8 KB of the first measurement less the 640 B the narrower joining table saves),
the rest in code the Arabic shaper added to (plan, shaping, position); the comparison is approximate, as `default` also holds the dev commands.

**On the device (X4, 2026-11-07).** The X3 was busy, so the X4 ran it: same ESP32-C3, same `default` firmware, built
from this branch merged with master `9331cbfc1`. `CMD:TTF:SHAPE` shaped ten words of each language, each word once,
16 pt, with the body family's regular face:
- merged Noto Naskh Arabic for Arabic, Persian and Urdu;
- the shipped Noto Sans Devanagari for Hindi.

| Words | Family | `cpac` | `plan_bytes` | `plan_us` (median) | `shape_us` median (min-max) | glyphs (median) |
|---|---|---|---|---|---|---|
| Arabic (`ar`) | Noto Naskh Arabic | 5,500 | 2,108 | 1,608 | 1,187 (915-1,954) | 6 |
| Persian (`fa`) | Noto Naskh Arabic | 5,500 | 2,140 | 1,642 | 1,298 (928-1,812) | 6 |
| Urdu (`ur`) | Noto Naskh Arabic | 5,500 | 2,140 | 1,647 | 1,118 (587-1,411) | 5 |
| Hindi (`hi`) | Noto Sans Devanagari | 18,400 | 3,176 | 2,048 | 2,455 (1,775-4,038) | 5.5 |

- **An Arabic word costs about half a Hindi one.** The Arabic plans are smaller (41-46 planned lookups against 77)
  and no Indic reordering runs. PR 4b's 2x build-time line is therefore less at risk for Arabic than it was for
  Hindi, which ran at 2.61x on real prose.
- **A plan built on the device is 2,108-2,140 B,** within item 7's estimate of 2,068-2,132 B for the C3 plus up to
  32 B for the masks.
- **The face's first `current()`** (`init_us`) takes 433 us for Naskh and 717 us for Devanagari.
- The words, with the firmware, the fonts and the restore, are in the session's scratchpad (`x4a/`). The X4 was
  restored from a full flash backup read before the run, so it is back on its own firmware with its settings
  unchanged.
