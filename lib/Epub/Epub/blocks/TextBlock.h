#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page.
//
// All per-word data lives in ONE flat heap allocation (the arena) instead of
// four parallel vectors (words, xpos, styles, sizes). A resident page holds
// ~25-30 of these blocks, and the vector-of-string layout cost ~250 throwing
// allocations per page load -- the primary driver of heap fragmentation on the
// ESP32-C3 (~380 KB heap, no compaction).
//
// Ported from the sibling crosspoint-reader project's PR #2547 ("Flatten
// TextBlock word storage into single allocation"), which flattens the same
// structure but carries focus-reading annotations; adapted here to carry this
// fork's optional per-word inline font-size array instead.
//
// Arena layout, in order (2-byte alignment holds by construction: the two 16-bit
// arrays come first and the arena base is allocator-aligned; RISC-V faults on
// unaligned multi-byte access):
//   uint16_t textOff[wordCount]   byte offset of word i's text within text[]
//   int16_t  xpos[wordCount]
//   uint8_t  styles[wordCount]
//   uint8_t  sizes[wordCount]     present only when sizesPresent
//   char     text[textBytes]      all words back to back, each NUL-terminated
//
// Each word is stored NUL-terminated so render() can hand `text + textOff[i]`
// straight to C APIs (drawText) with no std::string materialization.
//
// Per-word sizes are the inline CSS font-size percent of the block size. The
// array is omitted from the arena entirely when every word is at 100% (the
// overwhelmingly common case), so ordinary lines pay zero per-word RAM or
// section-cache cost.
// The slice of BlockStyle a laid-out line still needs once it is resident.
//
// A TextBlock is the *product* of layout: the parser has already consumed the
// margins, padding, indent and float zones to compute each word's xpos and the
// line's y, and baked the results into the arena. Only the font selection is
// still needed, because rendering resolves glyphs lazily at draw time.
// `alignment` is not read by render() either, but it is cheap and the pipeline
// golden dump reports it, so it stays as layout provenance.
//
// Keeping the full 52-byte BlockStyle here cost ~1.2 KB per resident page
// (~28 lines/page) in fields nothing on the reader path could read --
// getBlockStyle() had no callers outside the test dump. The same fields were
// also 19 dead bytes per line in the section cache, so they are no longer
// serialized either. See BlockStyle for the parse-time-only fields (floatZones,
// fromBrElement, fontResolved) that are deliberately absent.
//
// RTL / CJK note: this slice is not a barrier to either. CJK already works and
// needs nothing here -- its line breaking is a parse-time concern
// (ChapterHtmlSlimParser, "primary word-breaking mechanism" for spaceless text),
// so a CJK line reaches TextBlock already broken and positioned like any other.
// For RTL, the direction state the compiled-content design reserves is per-word
// bidiLevel and a base-direction flag on the block (see CompiledContent.h), not
// a BlockStyle field -- BlockStyle has no direction member at all. Mirroring
// margins/padding/floats is an *input* to positioning, done upstream in the
// parser where the full BlockStyle still lives; a resident block only ever holds
// the resulting xpos/y. `alignment` is kept partly because RTL text is
// right-aligned. Nothing here is lost: the dropped fields remain in BlockStyle
// and on disk, so widening this struct later is a local change.
struct RenderStyle {
  float fontSizeMultiplier = 1.0f;
  int32_t headingFontId = 0;  // 0 = body font scaled by fontSizeMultiplier
  CssTextAlign alignment = CssTextAlign::Justify;
};

class TextBlock final : public Block {
 private:
  RenderStyle renderStyle;
  uint16_t numWords = 0;
  uint16_t textBytes = 0;  // total size of the text region, including one NUL per word
  bool sizesPresent = false;
  bool isValid = true;
  // The ONLY allocation: makeUniqueNoThrow, so OOM yields an invalid block
  // instead of abort() (bare new is not nothrow with -fno-exceptions).
  std::unique_ptr<uint8_t[]> arena;
  // Typed views into the arena, bound once after the arena is filled. All
  // 16-bit bases sit at even offsets, so direct dereference is alignment-safe.
  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  const uint8_t* stylesArr = nullptr;
  const uint8_t* sizesArr = nullptr;  // null when !sizesPresent
  const char* textArr = nullptr;

  // Byte offsets of each arena region from the arena base. Single source of
  // truth for the layout, shared by the fill path, bindArenaPointers() and
  // arenaSize() so the in-RAM and on-disk layouts can never drift apart.
  struct ArenaOffsets {
    size_t xpos, styles, sizes, text;
  };
  static ArenaOffsets arenaOffsets(uint16_t wordCount, bool hasSizes);

  // Process-wide render option (see setGuideDots): set by the reader activity
  // from settings before pages render. Not per-block state -- blocks are cached
  // and shared across renders, and the aid applies uniformly to every line.
  static bool guideDotsEnabled;

  TextBlock() = default;  // deserialize() fills the fields directly
  static size_t arenaSize(uint16_t wordCount, bool hasSizes, uint16_t textBytes);
  void bindArenaPointers();

  // Effective render scale of word i: block multiplier x the word's size percent.
  float wordScale(const uint16_t i) const {
    return sizesPresent ? renderStyle.fontSizeMultiplier * (sizesArr[i] / 100.0f) : renderStyle.fontSizeMultiplier;
  }

 public:
  // Flatten-on-construct: copies the layout-time vectors into the arena; the
  // vectors die with the caller. An all-100% sizes vector is normalized to "no
  // sizes". On arena OOM the block is empty and valid() is false -- callers must
  // check and drop the line instead of using it.
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<uint8_t> word_sizes = {});
  ~TextBlock() override = default;
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  const RenderStyle& getRenderStyle() const { return renderStyle; }
  bool isEmpty() override { return numWords == 0; }
  bool valid() const { return isValid; }
  uint16_t wordCount() const { return numWords; }
  // NUL-terminated by construction; safe to pass to C APIs directly.
  const char* wordText(const uint16_t i) const { return textArr + textOffArr[i]; }
  uint16_t wordTextLen(const uint16_t i) const {
    const uint16_t end = (i + 1 < numWords) ? textOffArr[i + 1] : textBytes;
    return end - textOffArr[i] - 1;  // exclude the NUL
  }
  int16_t wordXpos(const uint16_t i) const { return xposArr[i]; }
  EpdFontFamily::Style wordStyle(const uint16_t i) const { return static_cast<EpdFontFamily::Style>(stylesArr[i]); }
  bool hasWordSizes() const { return sizesPresent; }
  uint8_t wordSizePct(const uint16_t i) const { return sizesPresent ? sizesArr[i] : 100; }
  // Diagnostic/test helper: materializes the per-word size vector (empty when uniform).
  std::vector<uint8_t> getWordSizes() const {
    return sizesPresent ? std::vector<uint8_t>(sizesArr, sizesArr + numWords) : std::vector<uint8_t>{};
  }
  // Largest per-word size percent on this line (100 when uniform). The line's
  // vertical advance scales with this so an enlarged inline word doesn't collide
  // with the next line.
  uint8_t maxSizePct() const;

  // Guide dots reading aid: when enabled, render() draws a small dot centered in
  // the empty space between adjacent words of a line -- never before the first
  // word or after the last. Idea from CrossInk (https://github.com/uxjulia/CrossInk),
  // reimplemented clean-room from its user-facing description. Render-time only:
  // word layout is untouched, so the flag is NOT part of the section cache key
  // and toggling it never triggers a section rebuild.
  static void setGuideDots(const bool enabled) { guideDotsEnabled = enabled; }

  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);
};
