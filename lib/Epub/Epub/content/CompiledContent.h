#pragma once
// Compiled content format (content.bin, magic "WBC1"). This header is its definition.
//
// This is the SETTINGS-INDEPENDENT product of a one-time Stage-1 compile: parsed,
// CSS-resolved blocks with text runs and image refs, keyed only by the book's ZIP
// content fingerprint. Stage-2 pagination (word measurement + line/page breaking)
// reads this and produces the per-settings section caches, so a font/margin change
// never re-runs ZIP/XML/CSS.
//
// This header + CompiledContent.cpp are sub-step 2a: the in-memory model and the
// content.bin writer/reader with a host round-trip test. The rendererless Stage-1
// pass that *fills* this model (from ChapterHtmlSlimParser) and the anchor/chapter/
// char-offset tables land in later sub-steps; header flags are reserved for them.
// Nothing here is wired into a build yet (guarded by EPUB_STAGE1 at the call sites).

#include <HalStorage.h>  // FsFile

#include <cstdint>
#include <string>
#include <vector>

#include "Epub/css/CssStyle.h"

namespace compiled {

inline constexpr char kMagic[4] = {'W', 'B', 'C', '1'};
inline constexpr uint8_t kVersion = 1;

// Per-word settings-independent data — the slice of today's TextBlock that survives
// a relayout. No xpos: Stage-2 measurement computes it. bidiLevel is 0 for LTR books
// and reserved for RTL.
struct Word {
  uint32_t textOff = 0;   // byte offset of the word's text within Block::text
  uint8_t styleSpan = 0;  // inline bold/italic/underline/super/sub/smallcaps bitmask
  uint8_t sizePct = 100;  // per-word font-size percent; 100 = inherit block size
  uint8_t bidiLevel = 0;  // Unicode embedding level; 0 = LTR
};

enum class BlockType : uint8_t { Text = 0, Image = 1 };

// Block::flags bits.
enum BlockFlags : uint8_t {
  kStartsChapter = 1 << 0,
  kPageBreakBefore = 1 << 1,
  kPageBreakAfter = 1 << 2,
  // bits 3-4: base direction (0 auto, 1 LTR, 2 RTL) — reserved for RTL.
  kDirectionShift = 3,
  kDirectionMask = 0b11 << 3,
};

// One content block. Text fields are used when type==Text, image fields when
// type==Image; the unused set stays empty/zero.
struct Block {
  BlockType type = BlockType::Text;
  uint16_t styleId = 0;  // index into CompiledContent::stylePool
  uint8_t flags = 0;
  uint32_t charOffset = 0;  // absolute char offset of this block's first char (reading progress)

  // Text block:
  std::vector<Word> words;
  std::string text;  // words back-to-back, each NUL-terminated

  // Image block:
  std::string entryPath;  // EPUB-internal path (e.g. OEBPS/images/x.jpg)
  int16_t width = 0;      // intrinsic dimensions, pre-probed at compile
  int16_t height = 0;
  uint8_t floatSide = 0;  // 0 none / 1 left / 2 right
  std::string alt;
};

// Named position for anchor navigation (TOC targets, in-book links). Resolves to a
// (block, char-offset) pair; Stage-2 maps that to a page. Id stored as a string
// (not a hash) so a lookup can never resolve the wrong target.
struct Anchor {
  std::string id;           // element id / fragment
  uint32_t blockIndex = 0;  // block within this spine
  uint32_t charOffsetInBlock = 0;
};

// Book-level chapter/heading entry (drives the TOC and heading navigation).
struct Chapter {
  uint16_t spineIndex = 0;
  uint32_t blockIndex = 0;
  uint8_t level = 0;  // heading level 1..6; 0 = non-heading chapter boundary
  std::string title;
};

// Per-spine content, in document order.
struct SpineContent {
  std::vector<Block> blocks;
  std::vector<Anchor> anchors;
  uint32_t firstCharOffset = 0;  // absolute char offset of the spine's first char (progress)
};

// A whole book's compiled content.
struct CompiledContent {
  std::vector<CssStyle> stylePool;  // deduped block styles; blocks reference by index
  std::vector<SpineContent> spines;
  std::vector<Chapter> chapters;
};

// Whether two block styles are identical for pooling purposes (all rendering-relevant
// fields + the explicit-set flags). Two blocks that resolve to equal styles share a pool id.
bool styleEquals(const CssStyle& a, const CssStyle& b);

// Return the pool id for `style`, appending it to `content.stylePool` if not already
// present (dedup by value). Linear scan — the distinct-style set per book is small
// (tens), and blocks vastly outnumber styles.
uint16_t internStyle(CompiledContent& content, const CssStyle& style);

// Serialize/deserialize the WBC1 container. Return false on I/O error or a
// version/magic mismatch (caller treats a mismatch like a stale cache: recompile).
bool writeContentBin(FsFile& out, const CompiledContent& content);
bool readContentBin(FsFile& in, CompiledContent& content);

}  // namespace compiled
