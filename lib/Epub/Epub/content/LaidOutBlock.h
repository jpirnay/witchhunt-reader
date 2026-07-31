#pragma once
// LaidOutBlock — the pull core's per-block cache slot (P1, text-only), microreader's LaidOutParagraph
// analog (docs/pull-core-plan-microreader-guided-2026-07-27.md §1). A logical Text block is laid out
// ONCE into its page-INDEPENDENT line set (via ParsedText::layoutAndExtractLines, the same call
// LayoutSink makes), plus the settings-resolved BlockStyle and the vertical-spacing scalars the
// forward collect loop needs to reproduce LayoutSink's Y math (makePages/addLineToPage) exactly. A
// page that ends on this block AND the page that starts on it both reuse this slot — the property
// that makes live one-page-from-cursor layout cheap.
//
// P2 adds ATOMIC block Images + HRs: our block images never split across pages (clamped to the
// viewport by computeImageDisplaySize; placeBlockImage places them whole), so an Image/Hr block is a
// single indivisible item that either fits the remaining page or forces a break — no line set, no
// pixel-row offset. Tables (P4) remain a scaffold fallback for now.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Epub/blocks/BlockStyle.h"

class TextBlock;
class ImageBlock;
class ParsedText;

namespace compiled {

// A laid-out logical block: a run of text lines, a standalone block image, or a horizontal rule. The
// preprocessor (PullDriver::prepareBlock) resolves the settings-dependent layout ONCE (line-breaking,
// image display size, wrapper-spacing merge); the collect loop then only does the vertical accounting
// + page-break decision, so a page that ends on a block and the page that starts on it reuse this slot.
struct LaidOutBlock {
  enum class Kind { Text, Image, Hr };
  Kind kind = Kind::Text;

  // --- Text (kind == Text) ---
  // The resolved, post-merge block style actually laid out (headings folded, empty-block merge
  // already applied by the preprocessor). resolveBlockFont has NOT been run yet — the collect loop
  // runs it (seeded from the carried auxFontId) exactly where makePages does.
  BlockStyle style;
  // `lines` are the exact std::shared_ptr<TextBlock> objects ParsedText emits (per-word xpos already
  // baked), so a placed PageLine is byte-identical to LayoutSink's.
  std::vector<std::shared_ptr<TextBlock>> lines;

  bool isEmptyBlock = false;     // <br>/empty wrapper: no lines, spacing-only (folded into next)
  bool isContinuation = false;   // kContinuation split record: makePages skips top-margin/fold
  bool preformatted = false;     // inside <pre>: suppress extra-paragraph spacing
  bool pageBreakBefore = false;  // kPageBreakBefore: forces a fresh page when the page has content
  bool flushedMidBlock = false;  // >96-word mid-block flush fired: LayoutSink drops this block's top
                                 // spacing (flush bypasses makePages' margin; tail is a continuation)

  // Word count per emitted line (prefix-summable), for reproducing addLineToPage's footnote
  // assignment (a footnote lands on the page whose lines have covered its anchor word index).
  std::vector<uint16_t> lineWordCounts;

  // --- Image (kind == Image): a centered block image, placed whole (mirrors placeBlockImage) ---
  // The ImageBlock (and its per-spine cache path) is built at PLACEMENT time, not prepare, so an
  // over-read image that spills to the next page does not consume an image-counter slot.
  std::string imageEntryPath;  // EPUB-internal image path; the cache path is resolved at placement
  std::string imageAlt;
  int16_t imageWidth = 0;          // display width
  int16_t imageX = 0;              // centered x
  int16_t imageHeight = 0;         // display height (Y advance for the image itself)
  int16_t imageSpacingTop = 0;     // wrapper (pending-merge) spacing consumed above the image
  int16_t imageSpacingBottom = 0;  // wrapper spacing consumed below the image

  // --- Hr (kind == Hr): a centered rule (mirrors placeHr) ---
  int16_t hrX = 0;
  int16_t hrWidth = 0;
  int16_t hrMarginV = 0;  // half-line margin above and below the 1px rule
};

}  // namespace compiled
