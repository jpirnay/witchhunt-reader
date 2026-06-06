#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "lib/Epub/Epub/converters/ImageDimensionsGuard.h"

// ============================================================================
// Mock SdCardFont
//
// Tracks calls to unloadMetadata() and reloadMetadata() so tests can assert
// the correct call sequence during phase transitions without any hardware.
// ============================================================================

struct MockSdCardFont {
  std::vector<std::string> callLog;
  bool metadataLoaded = true;

  void unloadMetadata() {
    callLog.push_back("unload");
    metadataLoaded = false;
  }

  void reloadMetadata() {
    callLog.push_back("reload");
    metadataLoaded = true;
  }

  void clearLog() { callLog.clear(); }
};

// ============================================================================
// Mock GfxRenderer
//
// Holds a list of registered SD fonts and delegates drop/restore to them.
// ============================================================================

struct MockGfxRenderer {
  std::vector<MockSdCardFont*> sdFonts;

  void registerSdFont(MockSdCardFont* f) { sdFonts.push_back(f); }

  void dropSdCardFontMetadata() {
    for (auto* f : sdFonts) f->unloadMetadata();
  }

  void restoreSdCardFontMetadata() {
    for (auto* f : sdFonts) f->reloadMetadata();
  }

  bool hasAnySdFont() const { return !sdFonts.empty(); }
};

// ============================================================================
// Minimal mock allocator: tracks live "layout" vs "draw" allocations.
//
// layout allocs  = fullIntervals + kern/lig  (present during createSectionFile)
// draw allocs    = miniData FULL             (present during page render)
//
// The key invariant: these two sets must NEVER overlap in time.
// ============================================================================

struct MockAllocator {
  bool layoutAllocLive = false;
  bool drawAllocLive = false;
  bool overlapDetected = false;

  void allocateLayout() {
    if (drawAllocLive) overlapDetected = true;
    layoutAllocLive = true;
  }

  void freeLayout() { layoutAllocLive = false; }

  void allocateDraw() {
    if (layoutAllocLive) overlapDetected = true;
    drawAllocLive = true;
  }

  void freeDraw() { drawAllocLive = false; }
};

// ============================================================================
// Mock ReaderPhase enum + controller — simulates EpubReaderActivity behaviour
// ============================================================================

enum class ReaderPhase { IDLE, READING, PRECOMPILING };

struct MockReaderController {
  ReaderPhase phase = ReaderPhase::IDLE;
  MockGfxRenderer& renderer;
  MockAllocator& alloc;
  std::vector<std::string> eventLog;

  MockReaderController(MockGfxRenderer& r, MockAllocator& a) : renderer(r), alloc(a) {}

  // Simulates the sequence on entering PRECOMPILING
  void enterPrecompiling() {
    phase = ReaderPhase::PRECOMPILING;
    if (renderer.hasAnySdFont()) {
      renderer.dropSdCardFontMetadata();
      eventLog.push_back("metadata_dropped");
    }
    // Simulate createSectionFile: allocates layout data
    alloc.allocateLayout();
    eventLog.push_back("create_section_file");
    alloc.freeLayout();
    // Restore metadata
    if (renderer.hasAnySdFont()) {
      renderer.restoreSdCardFontMetadata();
      eventLog.push_back("metadata_restored");
    }
    phase = ReaderPhase::READING;
  }

  // Simulates a normal page render: allocates draw data
  void renderPage() {
    alloc.allocateDraw();
    eventLog.push_back("render_page");
    alloc.freeDraw();
  }
};

// ============================================================================
// Image validation regression coverage
// ============================================================================

TEST(MemoryModelTest, ImageDimensionGuardRejectsOverflowingProduct) {
  EXPECT_TRUE(imageDimensionsWouldOverflow(65535, 65535, 3145728));
}

TEST(MemoryModelTest, ImageDimensionGuardAcceptsReasonableSource) {
  EXPECT_FALSE(imageDimensionsWouldOverflow(2048, 1536, 3145728));
}

// ============================================================================
// Tests
// ============================================================================

TEST(MemoryModelTest, MetadataUnloadedBeforeCreateSectionFile) {
  MockSdCardFont font;
  MockGfxRenderer renderer;
  MockAllocator alloc;
  renderer.registerSdFont(&font);
  MockReaderController ctrl(renderer, alloc);

  ASSERT_TRUE(font.metadataLoaded);
  ctrl.enterPrecompiling();

  // Sequence must be: unload → createSectionFile → reload
  ASSERT_GE(font.callLog.size(), 2u);
  EXPECT_EQ(font.callLog[0], "unload");

  // metadata_dropped must appear in event log before create_section_file
  auto dropIt = std::find(ctrl.eventLog.begin(), ctrl.eventLog.end(), std::string{"metadata_dropped"});
  auto createIt = std::find(ctrl.eventLog.begin(), ctrl.eventLog.end(), std::string{"create_section_file"});
  ASSERT_NE(dropIt, ctrl.eventLog.end());
  ASSERT_NE(createIt, ctrl.eventLog.end());
  EXPECT_LT(dropIt, createIt);
}

TEST(MemoryModelTest, MetadataReloadedAfterCreateSectionFile) {
  MockSdCardFont font;
  MockGfxRenderer renderer;
  MockAllocator alloc;
  renderer.registerSdFont(&font);
  MockReaderController ctrl(renderer, alloc);

  ctrl.enterPrecompiling();

  // After transition, metadata must be loaded again
  EXPECT_TRUE(font.metadataLoaded);

  // create_section_file must appear before metadata_restored
  auto createIt = std::find(ctrl.eventLog.begin(), ctrl.eventLog.end(), std::string{"create_section_file"});
  auto restoreIt = std::find(ctrl.eventLog.begin(), ctrl.eventLog.end(), std::string{"metadata_restored"});
  ASSERT_NE(createIt, ctrl.eventLog.end());
  ASSERT_NE(restoreIt, ctrl.eventLog.end());
  EXPECT_LT(createIt, restoreIt);
}

TEST(MemoryModelTest, LayoutAndDrawPeaksNonOverlapping) {
  MockSdCardFont font;
  MockGfxRenderer renderer;
  MockAllocator alloc;
  renderer.registerSdFont(&font);
  MockReaderController ctrl(renderer, alloc);

  // Interleave: render a page (draw), then chapter transition (layout), then render again
  ctrl.renderPage();
  ctrl.enterPrecompiling();
  ctrl.renderPage();

  EXPECT_FALSE(alloc.overlapDetected) << "Layout and draw allocations overlapped — phase model not enforced";
}

TEST(MemoryModelTest, MetadataNotUnloadedForBuiltinFont) {
  // No SD font registered — drop/restore should not be called
  MockGfxRenderer renderer;
  MockAllocator alloc;
  MockReaderController ctrl(renderer, alloc);

  ctrl.enterPrecompiling();

  // No metadata events
  auto dropIt = std::find(ctrl.eventLog.begin(), ctrl.eventLog.end(), std::string{"metadata_dropped"});
  auto restoreIt = std::find(ctrl.eventLog.begin(), ctrl.eventLog.end(), std::string{"metadata_restored"});
  EXPECT_EQ(dropIt, ctrl.eventLog.end()) << "metadata_dropped should not fire with no SD font";
  EXPECT_EQ(restoreIt, ctrl.eventLog.end()) << "metadata_restored should not fire with no SD font";
}

TEST(MemoryModelTest, PhaseTransitionLeavesReadingPhase) {
  MockGfxRenderer renderer;
  MockAllocator alloc;
  MockReaderController ctrl(renderer, alloc);

  EXPECT_EQ(ctrl.phase, ReaderPhase::IDLE);
  ctrl.phase = ReaderPhase::READING;
  ctrl.enterPrecompiling();
  EXPECT_EQ(ctrl.phase, ReaderPhase::READING);
}

TEST(MemoryModelTest, MultipleChapterTransitionsNeverOverlap) {
  MockSdCardFont font;
  MockGfxRenderer renderer;
  MockAllocator alloc;
  renderer.registerSdFont(&font);
  MockReaderController ctrl(renderer, alloc);

  // Simulate reading 5 chapters
  for (int i = 0; i < 5; ++i) {
    ctrl.renderPage();
    ctrl.renderPage();
    ctrl.enterPrecompiling();
  }

  EXPECT_FALSE(alloc.overlapDetected) << "Layout/draw overlap detected over multiple chapter transitions";
}
