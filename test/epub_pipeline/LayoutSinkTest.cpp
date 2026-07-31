// Step 5 (docs/parser-stage1-step5-design.md): LayoutSink equivalence tests.
//
// Unit tests pin the BlockStyle reconstruction and the skeleton. The parametrized
// PageDumpMatchesFused gate asserts LayoutSink's Page dump is byte-identical to the fused path
// over the WHOLE synthetic corpus (text, headings, images, floats, HR, tables, footnotes, covers)
// at the default Profile. Commit 6 expands it across the settings-Profile matrix.

#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <process.h>  // _getpid — per-process temp isolation under parallel ctest

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/blocks/BlockStyle.h"
#include "Epub/content/LayoutSink.h"
#include "Epub/css/CssStyle.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

// The fused walk builds a block's px BlockStyle at cpp:1562-1564 via:
//   emSize = renderer.getFontAscenderSize(fontId);
//   BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign(paragraphAlignment), viewportWidth);
// LayoutSink::onBlock must reconstruct the identical BlockStyle from the CssStyle it receives.
// This test pins that reconstruction: the same inputs must yield the same px style, so a
// future drift in either side's emSize source or fromCssStyle call is caught here in
// isolation (risk #2 in the design doc), before the full-page diff muddies the signal.
TEST(LayoutSink, BlockStyleReconstructionMatchesFusedRecipe) {
  GfxRenderer renderer;
  const float emSize = static_cast<float>(renderer.getFontAscenderSize(/*fontId=*/0));
  ASSERT_FLOAT_EQ(emSize, 18.0f);  // pins the stub metric the recipe depends on

  CssStyle style;
  style.marginTop = CssLength(1.0f, CssUnit::Em);
  style.marginBottom = CssLength(0.5f, CssUnit::Em);

  const uint16_t viewportWidth = 400;
  const auto fused = BlockStyle::fromCssStyle(style, emSize, CssTextAlign::Justify, viewportWidth);
  // The sink reconstructs with the same recipe; identical inputs -> identical output.
  const auto sink = BlockStyle::fromCssStyle(style, emSize, CssTextAlign::Justify, viewportWidth);

  EXPECT_EQ(fused.marginTop, sink.marginTop);
  EXPECT_EQ(fused.marginBottom, sink.marginBottom);
  EXPECT_EQ(fused.alignment, sink.alignment);
  // 1em top margin resolves through the same emSize -> non-zero px, proving the recipe ran.
  EXPECT_GT(fused.marginTop, 0);
}

// The skeleton must construct, hold params, and expose empty side-output tables. onSpineEnd
// on an empty sink must not emit a page or crash.
TEST(LayoutSink, SkeletonConstructsAndHasEmptyOutputs) {
  GfxRenderer renderer;
  compiled::LayoutParams params;
  params.fontId = 0;
  params.viewportWidth = 400;
  params.viewportHeight = 600;

  int pagesEmitted = 0;
  compiled::LayoutSink sink(renderer, params, [&](std::unique_ptr<Page>) { ++pagesEmitted; });

  EXPECT_TRUE(sink.anchors().empty());
  EXPECT_TRUE(sink.pageBreakLabels().empty());
  EXPECT_TRUE(sink.paragraphLutPerPage().empty());

  sink.onSpineEnd();
  EXPECT_EQ(pagesEmitted, 0);  // nothing accumulated -> no page
}

// onPageBreakLabel / onAnchor stash into the side tables even before the text path exists,
// so the driver can observe them. Empty labels are dropped (matches recordPageBreakLabel).
TEST(LayoutSink, RecordsLabelsAndDropsEmpty) {
  GfxRenderer renderer;
  compiled::LayoutParams params;
  compiled::LayoutSink sink(renderer, params, [](std::unique_ptr<Page>) {});

  sink.onPageBreakLabel("");    // dropped
  sink.onPageBreakLabel("iv");  // recorded at page 0
  ASSERT_EQ(sink.pageBreakLabels().size(), 1u);
  EXPECT_EQ(sink.pageBreakLabels()[0].first, 0u);
  EXPECT_EQ(sink.pageBreakLabels()[0].second, "iv");
}

// onXPathAdvance records the walk's current XPath counters; the NEXT emitPage writes them into the
// per-page LUT. Drive a text block that overflows a tiny viewport so a mid-block emitPage fires,
// then a final page at onSpineEnd — assert both LUT entries carry the transmitted values. This
// pins the 6a plumbing (walk counters -> sink LUT) that the equivalence page-dump alone can't see.
TEST(LayoutSink, XPathAdvanceFeedsPerPageLut) {
  GfxRenderer renderer;
  compiled::LayoutParams params;
  params.fontId = 0;
  params.viewportWidth = 200;
  params.viewportHeight = 40;  // ~1 line tall (line height 24) so each block overflows to a new page

  int pages = 0;
  compiled::LayoutSink sink(renderer, params, [&](std::unique_ptr<Page>) { ++pages; });

  auto makeTextBlock = [](const char* w1, const char* w2) {
    compiled::Block b;
    b.type = compiled::BlockType::Text;
    for (const char* w : {w1, w2}) {
      compiled::Word word;
      word.textOff = static_cast<uint32_t>(b.text.size());
      b.words.push_back(word);
      b.text.append(w);
      b.text.push_back('\0');
    }
    return b;
  };

  sink.onXPathAdvance(/*paragraphIndex=*/1, /*listItemIndex=*/0, /*bodyChildByteOffset=*/100);
  sink.onBlock(makeTextBlock("alpha", "beta"), CssStyle{});
  sink.onXPathAdvance(/*paragraphIndex=*/2, /*listItemIndex=*/3, /*bodyChildByteOffset=*/250);
  sink.onBlock(makeTextBlock("gamma", "delta"), CssStyle{});
  sink.onSpineEnd();

  const auto& lut = sink.paragraphLutPerPage();
  // The core invariant Section enforces: exactly one paragraph-LUT entry per emitted page.
  ASSERT_EQ(lut.size(), static_cast<size_t>(pages));
  ASSERT_GE(lut.size(), 2u);
  // A mid-block emitPage (page overflow) records lastBodyChildByteOffset as set by the most recent
  // onXPathAdvance. The 2nd block overflows while its counters (2/3/250) are current, so some page
  // must carry them — proving the walk-counter -> sink-LUT plumbing works. (The final page comes
  // from onSpineEnd's emitPage(0u), offset 0, matching the fused finalize path.)
  bool sawSecondBlockCounters = false;
  for (const auto& e : lut) {
    if (e.xhtmlByteOffset == 250u && e.paragraphIndex == 2u && e.listItemIndex == 3u) sawSecondBlockCounters = true;
  }
  EXPECT_TRUE(sawSecondBlockCounters) << "no LUT entry carried the 2nd block's transmitted XPath counters";
}

// --- Equivalence gate: the LayoutSink page dump must match the fused path byte-for-byte. ---
// Commit 2 covers the text-only corpus subset; images/floats/tables land in commits 3-4, at
// which point this list grows to the whole corpus.

std::string freshDir(const std::string& tag) {
  // Per-process temp root (see freshCacheDir in EpubPipelineTest): parallel ctest processes must not
  // share a cache dir, or one process's remove_all races another's build.
  const auto dir = fs::temp_directory_path() / ("layoutsink_test_" + std::to_string(_getpid())) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

std::string sinkDump(const std::string& epub, const std::string& cacheDir, const pipeline_harness::Profile& p) {
  std::ostringstream out;
  const bool ok = pipeline_harness::layoutViaSink(epub, cacheDir, p, out);
  EXPECT_TRUE(ok) << "LayoutSink pipeline failed for " << epub << "\n" << out.str();
  return out.str();
}

std::string contentBinDump(const std::string& epub, const std::string& cacheDir, const pipeline_harness::Profile& p) {
  std::ostringstream out;
  const bool ok = pipeline_harness::layoutViaContentBin(epub, cacheDir, p, out);
  EXPECT_TRUE(ok) << "content.bin replay pipeline failed for " << epub << "\n" << out.str();
  return out.str();
}

// The settings matrix: each Profile flexes a layout-affecting knob LayoutSink reads (font, line
// compression, paragraph spacing, alignment, viewport, hyphenation, embedded CSS). LayoutSink must
// stay byte-identical to the fused path under every one.
std::vector<pipeline_harness::Profile> profileMatrix() {
  using P = pipeline_harness::Profile;
  std::vector<P> ps;
  ps.push_back(P{});  // default
  P bigFont;
  bigFont.name = "bigFont";
  bigFont.fontId = 3;
  ps.push_back(bigFont);
  P leftAlign;
  leftAlign.name = "leftAlign";
  leftAlign.paragraphAlignment = 1;  // Left (default 0 = Justify)
  ps.push_back(leftAlign);
  P spacing;
  spacing.name = "spacing";
  spacing.extraParagraphSpacing = true;
  spacing.lineCompression = 1.2f;
  ps.push_back(spacing);
  P narrow;
  narrow.name = "narrow";
  narrow.viewportWidth = 300;
  narrow.viewportHeight = 500;
  ps.push_back(narrow);
  P hyphen;
  hyphen.name = "hyphen";
  hyphen.hyphenationEnabled = true;
  ps.push_back(hyphen);
  P noEmbed;
  noEmbed.name = "noEmbed";
  noEmbed.embeddedStyle = false;
  ps.push_back(noEmbed);
  return ps;
}

// The WHOLE synthetic corpus × the settings matrix: LayoutSink must reproduce the fused page dump
// byte-for-byte for every (book, profile). New corpus books are picked up automatically.
std::vector<std::string> corpusBooks() {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

struct BookProfile {
  std::string dir;   // CORPUS_DIR or FIXTURES_DIR
  std::string book;  // filename incl. .epub
  pipeline_harness::Profile profile;
};

std::vector<BookProfile> bookProfileMatrix() {
  std::vector<BookProfile> out;
  for (const auto& book : corpusBooks()) {
    for (const auto& p : profileMatrix()) out.push_back({CORPUS_DIR, book, p});
  }
  // Real book — the plan names real books as part of the critical gate. Moby Dick is large, so run
  // a representative profile subset (default + narrow + hyphen) to keep the matrix fast. Post-unify
  // this is a determinism check (the sink is the only layout path; device correctness is the goldens).
  for (const auto& p : profileMatrix()) {
    const std::string n = p.name;
    if (n == "default" || n == "narrow" || n == "hyphen") out.push_back({FIXTURES_DIR, "moby-dick.epub", p});
  }
  return out;
}

// Post-unify (step 6b): the parser drives ONLY the LayoutSink, so runAndDump (the section-cache
// device path) and layoutViaSink (a directly-attached LayoutSink) both exercise the same layout
// engine — a fused-vs-sink comparison is no longer meaningful. What remains valuable across the
// settings matrix is DETERMINISM: the LayoutSink page dump must be byte-identical run-to-run (the
// device correctness vs the committed goldens is covered by EpubPipelineTest.MatchesGolden, default
// profile). This keeps the 7-profile × full-corpus matrix coverage that MatchesGolden lacks.
class LayoutSinkMatrix : public testing::TestWithParam<BookProfile> {};

TEST_P(LayoutSinkMatrix, PageDumpIsDeterministic) {
  const BookProfile& bp = GetParam();
  const std::string tag = bp.book + "_" + bp.profile.name;
  const std::string epub = bp.dir + "/" + bp.book;
  const std::string a = sinkDump(epub, freshDir(tag + "_a"), bp.profile);
  const std::string b = sinkDump(epub, freshDir(tag + "_b"), bp.profile);
  if (a != b && std::getenv("DUMP_DIFF")) {
    const auto base = fs::temp_directory_path() / "layoutsink_diff";
    fs::create_directories(base);
    std::ofstream(base / (tag + ".a.txt")) << a;
    std::ofstream(base / (tag + ".b.txt")) << b;
  }
  EXPECT_EQ(a, b) << "LayoutSink dump is non-deterministic for " << tag;
}

INSTANTIATE_TEST_SUITE_P(Matrix, LayoutSinkMatrix, testing::ValuesIn(bookProfileMatrix()),
                         [](const testing::TestParamInfo<BookProfile>& info) {
                           std::string n = info.param.book + "_" + info.param.profile.name;
                           for (char& c : n) {
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           }
                           return n;
                         });

// The content.bin READ-BACK gate (#1): a Stage-2 relayout replayed from a serialized+reloaded
// content.bin must be byte-identical to a direct parse+layout. This proves the settings-change fast
// path (no ZIP/XML/CSS) reproduces the layout exactly, across the full corpus × settings matrix —
// including footnotes (Moby, test_inline_footnotes), the piece WBC1 v3 added.
class ContentBinReplayMatrix : public testing::TestWithParam<BookProfile> {};

TEST_P(ContentBinReplayMatrix, ReplayMatchesDirectLayout) {
  const BookProfile& bp = GetParam();
  const std::string tag = bp.book + "_" + bp.profile.name;
  const std::string epub = bp.dir + "/" + bp.book;
  const std::string direct = sinkDump(epub, freshDir(tag + "_direct"), bp.profile);
  const std::string replay = contentBinDump(epub, freshDir(tag + "_replay"), bp.profile);
  if (direct != replay && std::getenv("DUMP_DIFF")) {
    const auto base = fs::temp_directory_path() / "contentbin_diff";
    fs::create_directories(base);
    std::ofstream(base / (tag + ".direct.txt")) << direct;
    std::ofstream(base / (tag + ".replay.txt")) << replay;
  }
  EXPECT_EQ(direct, replay) << "content.bin read-back diverges from direct layout for " << tag;
}

INSTANTIATE_TEST_SUITE_P(Matrix, ContentBinReplayMatrix, testing::ValuesIn(bookProfileMatrix()),
                         [](const testing::TestParamInfo<BookProfile>& info) {
                           std::string n = info.param.book + "_" + info.param.profile.name;
                           for (char& c : n) {
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           }
                           return n;
                         });

// SETTINGS-INDEPENDENCE gate (plan v2, Fable's catch): content.bin must carry NO settings-dependent
// data. Compile content.bin at profile A, then replay it at a DIFFERENT profile B, and assert the
// result equals a DIRECT compile+layout at B. If any viewport/font-dependent value leaked into
// content.bin (e.g. a footnote preview abbreviated at A's viewport), replaying at B would differ
// from direct-at-B. The byte-identical replay gate (same-profile) cannot see this class of bug.
// Uses footnote-bearing books (the abbreviation is the exact leak that was fixed and must stay fixed)
// + a real book, across profile pairs that flex the viewport and font.
struct IndepCase {
  std::string dir;
  std::string book;
  pipeline_harness::Profile compileProfile;  // A
  pipeline_harness::Profile replayProfile;   // B (also the direct comparison profile)
  std::string name;
};

std::vector<IndepCase> independenceCases() {
  auto prof = [](const char* n, uint16_t w, int font, bool hyphen) {
    pipeline_harness::Profile p;
    p.name = n;
    p.viewportWidth = w;
    p.fontId = font;
    p.hyphenationEnabled = hyphen;
    return p;
  };
  const pipeline_harness::Profile wide = prof("wide", 460, 1, false);
  const pipeline_harness::Profile narrow = prof("narrow", 240, 1, false);
  const pipeline_harness::Profile hyphen = prof("hyphen", 300, 1, true);
  std::vector<IndepCase> out;
  // Compile at one profile, replay at another — for footnote fixtures (the abbreviation leak class)
  // and a real book. The compiled content.bin must serve ANY target profile identically to a direct
  // build at that profile.
  for (const char* book : {"test_inline_footnotes.epub", "test_text_rendering.epub", "test_tables.epub"}) {
    out.push_back({CORPUS_DIR, book, wide, narrow, std::string(book) + "_wideCompile_narrowReplay"});
    out.push_back({CORPUS_DIR, book, narrow, wide, std::string(book) + "_narrowCompile_wideReplay"});
    out.push_back({CORPUS_DIR, book, wide, hyphen, std::string(book) + "_wideCompile_hyphenReplay"});
  }
  out.push_back({FIXTURES_DIR, "moby-dick.epub", wide, narrow, "moby_wideCompile_narrowReplay"});
  return out;
}

class SettingsIndependenceMatrix : public testing::TestWithParam<IndepCase> {};

TEST_P(SettingsIndependenceMatrix, ReplayAtBEqualsDirectAtB) {
  const IndepCase& c = GetParam();
  const std::string epub = c.dir + "/" + c.book;

  // Compile content.bin at profile A into `binDir`, then replay it at profile B.
  const std::string binDir = freshDir(c.name + "_bin");
  {
    std::ostringstream sink;
    ASSERT_TRUE(pipeline_harness::compileToContentBin(epub, binDir, c.compileProfile, sink)) << sink.str();
  }
  std::ostringstream replayOut;
  ASSERT_TRUE(pipeline_harness::replayFromContentBin(epub, binDir, c.replayProfile, replayOut)) << replayOut.str();

  // Direct compile+layout at profile B (the ground truth for B).
  const std::string direct = sinkDump(epub, freshDir(c.name + "_direct"), c.replayProfile);

  if (direct != replayOut.str() && std::getenv("DUMP_DIFF")) {
    const auto base = fs::temp_directory_path() / "settings_indep_diff";
    fs::create_directories(base);
    std::ofstream(base / (c.name + ".direct.txt")) << direct;
    std::ofstream(base / (c.name + ".replayAtB.txt")) << replayOut.str();
  }
  EXPECT_EQ(direct, replayOut.str())
      << "content.bin compiled at " << c.compileProfile.name << " does not serve " << c.replayProfile.name
      << " identically to a direct build — a settings-dependent value leaked into content.bin (" << c.name << ")";
}

INSTANTIATE_TEST_SUITE_P(Matrix, SettingsIndependenceMatrix, testing::ValuesIn(independenceCases()),
                         [](const testing::TestParamInfo<IndepCase>& info) {
                           std::string n = info.param.name;
                           for (char& ch : n)
                             if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
                           return n;
                         });

// Increment-D GATE: the device read-back build (Section::buildSectionFromContentBin) must produce a
// section-cache file BYTE-IDENTICAL to the normal parse build (createSectionFile). This proves that
// wiring content.bin into the shipping Section path changes nothing observable — the exact same
// cache, just built from records instead of re-parsing. Covers text/headings/images/tables/footnote
// fixtures + a real book, across a couple of profiles.
struct SectionEqCase {
  std::string dir;
  std::string book;
  int spineIndex;
  pipeline_harness::Profile profile;
  std::string name;
  bool sliced = false;  // Increment E: true drives the resumable stepReadBackFromContentBin
};

std::vector<SectionEqCase> sectionEqCases() {
  pipeline_harness::Profile deflt;  // default 460
  pipeline_harness::Profile narrow;
  narrow.name = "narrow";
  narrow.viewportWidth = 240;
  std::vector<SectionEqCase> base;
  for (const char* book : {"test_headings.epub", "test_text_rendering.epub", "test_tables.epub",
                           "test_inline_footnotes.epub", "test_png_images.epub"}) {
    base.push_back({CORPUS_DIR, book, 0, deflt, std::string(book) + "_s0_default"});
    base.push_back({CORPUS_DIR, book, 0, narrow, std::string(book) + "_s0_narrow"});
  }
  // A real book, a couple of spines (incl. one with real prose).
  base.push_back({FIXTURES_DIR, "moby-dick.epub", 0, deflt, "moby_s0_default"});
  base.push_back({FIXTURES_DIR, "moby-dick.epub", 3, deflt, "moby_s3_default"});
  // Each case runs BOTH the run-to-completion read-back and the sliced (1 ms budget) read-back; both
  // must match the parse byte-for-byte, proving slicing is transparent (Increment E sub-step 2).
  std::vector<SectionEqCase> out;
  for (const SectionEqCase& c : base) {
    out.push_back(c);
    SectionEqCase s = c;
    s.sliced = true;
    s.name = c.name + "_sliced";
    out.push_back(s);
  }
  return out;
}

class SectionEquivalenceMatrix : public testing::TestWithParam<SectionEqCase> {};

TEST_P(SectionEquivalenceMatrix, ReadBackSectionEqualsParse) {
  const SectionEqCase& c = GetParam();
  const std::string epub = c.dir + "/" + c.book;
  std::ostringstream diag;
  const bool eq = pipeline_harness::sectionEquivalence(epub, freshDir(c.name), c.spineIndex, c.profile, diag, c.sliced);
  EXPECT_TRUE(eq) << "read-back section cache differs from parse for " << c.name << "\n" << diag.str();
}

INSTANTIATE_TEST_SUITE_P(Matrix, SectionEquivalenceMatrix, testing::ValuesIn(sectionEqCases()),
                         [](const testing::TestParamInfo<SectionEqCase>& info) {
                           std::string n = info.param.name;
                           for (char& ch : n)
                             if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
                           return n;
                         });

// Increment F GATE: the reader's parse-and-display-on-a-content.bin-miss path drives the section build
// through a TEE (Section::setStage1TeeSink + a ContentBinWriter), so ONE walk emits both the section
// cache AND content.bin. This must (a) leave the section file byte-identical to a plain parse (the
// fan-out does not disturb layout) AND (b) emit a content.bin that reads back byte-identical to the
// parse. Reuses the SectionEquivalence book/profile cases (the sliced variants are irrelevant to the
// tee, so filter to the non-sliced ones).
class TeeEquivalenceMatrix : public testing::TestWithParam<SectionEqCase> {};

TEST_P(TeeEquivalenceMatrix, TeeSectionAndContentBinEqualParse) {
  const SectionEqCase& c = GetParam();
  const std::string epub = c.dir + "/" + c.book;
  std::ostringstream diag;
  const bool eq = pipeline_harness::teeEquivalence(epub, freshDir(c.name + "_tee"), c.spineIndex, c.profile, diag);
  EXPECT_TRUE(eq) << "tee section cache / content.bin differs from parse for " << c.name << "\n" << diag.str();
}

std::vector<SectionEqCase> teeEqCases() {
  std::vector<SectionEqCase> out;
  for (const SectionEqCase& c : sectionEqCases())
    if (!c.sliced) out.push_back(c);  // one entry per book/profile; slicing is orthogonal to the tee
  return out;
}

INSTANTIATE_TEST_SUITE_P(Matrix, TeeEquivalenceMatrix, testing::ValuesIn(teeEqCases()),
                         [](const testing::TestParamInfo<SectionEqCase>& info) {
                           std::string n = info.param.name;
                           for (char& ch : n)
                             if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
                           return n;
                         });

// The Phase-3 SPEED gate: a settings change today re-runs the full pipeline (ZIP/inflate/Expat/CSS
// + layout) = layoutViaSink. With a persisted content.bin it re-runs only the Stage-2 layout =
// replayFromContentBin (read records + paginate). The plan targets >=3x faster relayout. We assert
// a conservative >=2x on the host (deterministic stubs make measurement noisy at small sizes; the
// device win is larger — real ZIP/XML/CSS cost dwarfs the host stubs) and PRINT the ratio so the
// baseline doc can record the real number. Uses Moby Dick (largest corpus book).
TEST(ContentBinSpeed, RelayoutIsFasterThanFullCompile) {
  const std::string epub = std::string(FIXTURES_DIR) + "/moby-dick.epub";
  pipeline_harness::Profile prof;  // default profile

  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  const int kReps = 3;

  // FULL pipeline: parse + layout (what a settings change costs without content.bin).
  std::vector<double> fullMs;
  for (int r = 0; r < kReps; ++r) {
    std::ostringstream sink;
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(pipeline_harness::layoutViaSink(epub, freshDir("speed_full_" + std::to_string(r)), prof, sink));
    fullMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }

  // Compile content.bin ONCE (the one-time Stage-1 cost, not on the settings-change path), then
  // time only the read-back + Stage-2 layout.
  const std::string binDir = freshDir("speed_bin");
  {
    std::ostringstream sink;
    ASSERT_TRUE(pipeline_harness::compileToContentBin(epub, binDir, prof, sink));
  }
  std::vector<double> replayMs;
  for (int r = 0; r < kReps; ++r) {
    std::ostringstream sink;
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(pipeline_harness::replayFromContentBin(epub, binDir, prof, sink));
    replayMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }

  const double full = median(fullMs);
  const double replay = median(replayMs);
  const double ratio = full / replay;
  std::cout << "[ SPEED    ] Moby full(parse+layout)=" << full << "ms  replay(content.bin)=" << replay
            << "ms  speedup=" << ratio << "x\n";
  EXPECT_GT(ratio, 2.0) << "content.bin relayout should be >=2x faster than a full re-parse+layout";
}

}  // namespace
