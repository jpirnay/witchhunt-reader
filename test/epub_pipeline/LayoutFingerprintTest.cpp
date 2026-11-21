// Section::layoutFingerprint(): what a section's page numbers are counted in. A starred page keeps it with its page
// number (BookmarkStore), and a page number saved under one fingerprint is only good for a section with the same.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

constexpr int kSpine = 1;
const std::string kFixture = std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub";

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = TextScale();
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  return p;
}

struct LayoutFingerprint : testing::Test {
  fs::path work;
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() / ("layout_fingerprint_" + std::to_string(::getpid()));
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(kFixture, (work / "cache").string());
    ASSERT_TRUE(epub->load(true));
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work);
  }
};

}  // namespace

TEST_F(LayoutFingerprint, ABuildAndALoadOfOneVariantAgreeWithTheParams) {
  Section built(epub, kSpine, renderer);
  EXPECT_EQ(built.layoutFingerprint(), 0u) << "nothing loaded or built yet";
  ASSERT_TRUE(built.createSectionFile(params(), {}, /*skipEviction=*/true));
  EXPECT_NE(built.layoutFingerprint(), 0u);
  EXPECT_EQ(built.layoutFingerprint(), Section::layoutFingerprintFor(params()));

  Section loaded(epub, kSpine, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(params()));
  EXPECT_EQ(loaded.layoutFingerprint(), built.layoutFingerprint());
}

TEST_F(LayoutFingerprint, EveryLayoutInputGivesAnotherFingerprint) {
  const uint32_t base = Section::layoutFingerprintFor(params());
  Section::BuildParams font = params();
  font.fontId = 1;
  Section::BuildParams margins = params();
  margins.viewportWidth = 400;
  Section::BuildParams spacing = params();
  spacing.lineCompression = TextScale::percent(120);
  EXPECT_NE(Section::layoutFingerprintFor(font), base);
  EXPECT_NE(Section::layoutFingerprintFor(margins), base);
  EXPECT_NE(Section::layoutFingerprintFor(spacing), base);
}

// A load that falls back to the no-CSS cache has that variant's pages, not the requested one's.
TEST_F(LayoutFingerprint, TheNoCssFallbackIsTheFallbacksLayout) {
  Section::BuildParams withCss = params();
  withCss.embeddedStyle = true;
  const Section::BuildParams noCss = params();
  {
    Section built(epub, kSpine, renderer);
    ASSERT_TRUE(built.createSectionFile(noCss, {}, /*skipEviction=*/true));
  }
  Section loaded(epub, kSpine, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(withCss));
  ASSERT_TRUE(loaded.isEmbeddedStyleFallback());
  EXPECT_EQ(loaded.layoutFingerprint(), Section::layoutFingerprintFor(noCss));
  EXPECT_NE(loaded.layoutFingerprint(), Section::layoutFingerprintFor(withCss));
}

TEST_F(LayoutFingerprint, ASectionWithoutPagesHasNone) {
  Section section(epub, kSpine, renderer);
  EXPECT_FALSE(section.loadSectionFile(params())) << "nothing built yet";
  EXPECT_EQ(section.layoutFingerprint(), 0u);
  ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.clearCache());
  EXPECT_EQ(section.layoutFingerprint(), 0u);
}

// A layout's fingerprint carries the format; formatVersion() names it on its own, for what outlives a layout.
TEST(SectionFormatVersion, IsNeverZero) { EXPECT_NE(Section::formatVersion(), 0u); }
