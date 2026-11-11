#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "BidiLine.h"
#include "CjkPunctuation.h"
#include "ParagraphIndent.h"
#include "TokenBoundary.h"
#include "TypographyProfile.h"
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;
class BuildArena;
class ShapingFace;
class WordShapeCache;
namespace IndicShaper {
enum class Outcome : uint8_t;
}

class ParsedText {
 public:
  enum class LineProcessResult {
    Accepted,
    RetryWithoutHyphenation,
  };

  // Where the line being laid out sits in its paragraph (linePlace()): set before beforeLine_
  // and processLine run, for the parser's widow/orphan and keep-with-next decisions.
  struct LinePlace {
    bool paragraphStart = false;  // the paragraph's first line (not a continuation pass)
    // Lines from this one to the paragraph's last; 0 when that is not known yet (the pass
    // stops short of the paragraph's end, see includeLastLine).
    uint16_t linesLeft = 0;
  };

 public:
  // Per-word font-size bounds, percent of the block's font size (microreader-style).
  // Inline CSS font-size composes multiplicatively through nested elements; anything
  // outside this range is clamped so a uint8_t always holds the value.
  static constexpr uint8_t MIN_WORD_SIZE_PCT = 30;
  static constexpr uint8_t MAX_WORD_SIZE_PCT = 250;
  static constexpr uint8_t DEFAULT_WORD_SIZE_PCT = 100;
  // The smallest scale a combined word is drawn at to fit one em: a run wider than two ems stays sideways.
  static constexpr float kMinCombineScale = 0.5f;

 private:
  // Start and End (text-align: start / end, kept for the layout while the direction is unset or auto) become Left or
  // Right: Start is the right side in a right-to-left paragraph, End the left. Idempotent for one `rtl`: a block
  // resolves it once, with its decided direction.
  void resolveAlignment(bool rtl);

  std::vector<std::string> words;
  // Per-line x positions, reused across every line this block produces. Deliberately a member and
  // not a local: as a local it was a fresh heap allocation for EVERY rendered line, whose whole
  // life was reserve/fill/hand-over/free -- the line's words were already flattened into
  // TextBlock's single arena, and this handed the x positions over separately. Host profile of
  // one book: 16258 allocations at extractLine against 14459 rendered lines, essentially one
  // each, in the 16-32 byte classes that dominate the allocation count.
  std::vector<int16_t> lineXPosScratch_;
  // Layout scratch kept across paragraphs (the parser reuses one ParsedText, see reset()).
  // Each of these was a per-block local before: one allocation per paragraph apiece, ~30,000
  // per book on the host census once the word vectors stopped regrowing (memory audit
  // 2026-09, R2). clear() keeps their capacity, so a chapter allocates them once.
  std::vector<uint16_t> wordWidths_;
  // Each token's CjkPunctuation::Class when it is one full-width mark, and how much wider (> 0) or narrower (< 0) it
  // is at the line edge its mark acts at than mid-line, in pixels: where a line starts at an opening mark, where it
  // ends after any other. Then what push-in (spec 5.3) may take off the mark, in pixels: mid-line, its width there
  // less its minimum beside the same neighbour (shrink_); at its line edge, its line-edge width less its line-edge
  // minimum (edgeShrink_); 0 for other tokens. Four bytes a token. Layout scratch in step with wordWidths_:
  // calculateWordWidths fills all four, hyphenateWordAtIndex inserts into all four.
  std::vector<uint8_t> markClass_;
  // In a paragraph that needs bidi, 1 for each token whose width the width pass measured of the very form the line
  // pass places (a plain token, BidiLine::tokenForm): placeBidiLine takes that width instead of measuring the piece
  // again. A byte a token, in step with wordWidths_ like markClass_; empty in a paragraph without bidi.
  std::vector<uint8_t> plainForm_;
  std::vector<int8_t> edgeTrim_;
  std::vector<uint8_t> shrink_;
  std::vector<uint8_t> edgeShrink_;
  // Whether each token combines (combines()): 1 for a token set upright as one block down its column, else 0. Layout
  // scratch in step with wordWidths_ like markClass_; empty on a horizontal line and in a block with no token that
  // asks to combine.
  std::vector<uint8_t> combined_;
  // How a ruby group's tokens sit in their widths (applyRubyWidths), in pixels: `start` on the leader and `end` on
  // the group's last token are the overhang a line edge must hold when the group starts or ends a line; `lead` is
  // how far a base character widened by a longer ruby is drawn into its share (1:2:1 spreading).
  struct RubyEdge {
    int16_t start = 0;
    int16_t end = 0;
    int16_t lead = 0;
  };
  // A RubyEdge per token, in step with wordWidths_ like markClass_; empty when the block has no ruby. 6 B a token.
  std::vector<RubyEdge> rubyEdge_;
  // How far the block's lines sit in from the left: half of what rounding the width down to whole ems took (the CJK
  // grid, spec section 5.5). Set by each layout pass.
  int16_t gridInset_ = 0;
  std::vector<size_t> lineBreakIndices_;
  std::vector<bool> lineEndsWithHyphenatedWord_;
  std::vector<int> splitPrefixWordIndexes_;
  std::vector<bool> splitInsertedHyphen_;
  std::vector<size_t> suffixBreaks_;
  std::vector<bool> suffixLineEndsWithHyphenatedWord_;
  std::vector<int> suffixSplitPrefixWordIndexes_;
  std::vector<bool> suffixSplitInsertedHyphen_;
  std::vector<int> interWordGaps_;
  std::vector<int> lineIndexForWord_;  // computeLineBreaks' DP tables
  std::vector<int> dp_;
  std::vector<size_t> ans_;
  std::string allText_;
  // Where each line's TextBlock takes its bytes: the build's lent region (a page-scoped block
  // the parser opens), or the heap when null. The parser sets it on its main text block (with
  // beforeLine_, see there for why the two go together) and on table cells laid out as a grid
  // (without the hook: the row is placed as a whole inside a block of its own). A cell laid out
  // as a fallback paragraph keeps the heap.
  BuildArena* lineArena_ = nullptr;
  // Called with the line's largest word-size percent, and whether the line carries ruby, just before the line is
  // materialised. The parser uses it to run its page-fit test BEFORE the allocation: a line that does not
  // fit is then allocated from the page it lands on, not from the block of the page it
  // overflowed -- with page-scoped arena blocks, allocating first would leave page N+1 holding
  // bytes that die with page N.
  std::function<void(uint8_t maxSizePct, bool annotated)> beforeLine_;
  std::vector<EpdFontFamily::Style> wordStyles;
  // How each word joins the one before it (TokenBoundary.h): a space, glued, or a breakable CJK gap.
  // In lockstep with `words` through every insert/erase, like the per-word vectors below.
  std::vector<TokenBoundary::Boundary> wordBoundaries;
  // Per-word font size, percent of the block font size (100 = block size), and font slot. Kept in
  // lockstep with `words` through every insert/erase; the sizes are handed to each
  // TextBlock line so inline font-size spans survive into the page cache. The slot indexes
  // fontSlots_: a word in another of the book's families than the block's (Publisher Fonts)
  // is measured and drawn in that family's face, which the parser puts in its slot; slot 0
  // (every word, without families) is the block's font until the parser says otherwise.
  // Per-word slots follow crosspoint-reader PR #3934 (Justin Mitchell), its wordSizeSlots.
  std::vector<TextBlock::WordSize> wordSizes;
  TextBlock::WordFont fontSlots_[TextBlock::kMaxWordFonts] = {};
  uint8_t fontSlotsUsed_ = 0;  // a bit per slot a word of the block has had
  // Where each word begins in the chapter's visible text (VisibleText.h's rule), in lockstep with
  // `words` through every insert/erase. The section cache records the first word of each page from
  // it (KOReader sync, content offsets). 4 B per word; grown with the other word vectors in one
  // step and counted in addWord's heap gate.
  std::vector<uint32_t> wordVisibleOffsets;
  // The first word's offset of the line most recently handed to processLine. Read by the parser's
  // line callbacks, which only get the TextBlock.
  uint32_t lastLineVisibleOffset_ = 0;
  // The tokens the line most recently handed to processLine consumed: its token range, plus those of controls-only
  // lines before it that never reached processLine. Counted as the parser added them: a merged token counts as its
  // parts (TokenBoundary::Boundary::merged). A bidi line stores pieces, so wordCount() is not this.
  uint32_t lastLineTokenCount_ = 0;
  uint32_t controlsOnlyTokens_ = 0;  // tokens of controls-only lines not yet charged to a handed-over line
  // The ruby text of each Leader token (TokenBoundary::RubyRole) of the block, in token order: the n-th Leader owns the
  // n-th text. Moves with the tokens through every erase, snapshot and reset.
  std::vector<std::string> rubyTexts_;
  // The annotations of the line being extracted, handed to its TextBlock; reused across lines.
  std::vector<TextBlock::Annotation> lineAnnotations_;

  // A paragraph that needs bidi is laid out one UAX #9 pass per line (decision 5, BidiLine.h). It needs it for a
  // right-to-left direction, or a token with a code point of class R, AL, RLE, RLO, RLI or FSI; once a pass of the
  // paragraph needs it, every later pass takes it too. A pass that does not drops the bidi controls from its tokens.
  // All of it is the paragraph's: reset() and a pass that lays the last line out forget it (forgetBidiParagraph).
  bool bidi_ = false;
  uint8_t level_ = 0;          // the paragraph's level: 0 left to right, 1 right to left
  bool levelDecided_ = false;  // level_ is decided on the paragraph's first pass and kept
  // What only a paragraph that needs bidi uses. Boxed, so a paragraph or table cell without bidi carries one pointer
  // instead (a 64-bit host's ParsedText was 360 B larger with these inline). decideBidi() makes it for the first pass
  // that needs bidi, and bidi_ is never set without it. reset() keeps it for the next paragraph; releaseLayoutScratch()
  // frees it when no paragraph in progress needs bidi.
  struct BidiState {
    // The embeddings, overrides and isolates the text before the next line leaves open. Steps over each accepted
    // line.
    BidiLine::OpenControls openControls;
    // The line being laid out: its tokens, its pieces in visual order, and what its TextBlock is made from. The texts
    // are views into `words`, the forms into line.display: TextBlock copies both into its arena, so a line builds no
    // string. All keep their capacity across lines.
    BidiLine::Line line;
    // The line's tokens live in the arena workspace for the pass (layOutBidiLine); this vector holds them only when
    // the arena lends none. What the materialisation needs of them afterwards is kept apart: their count, and each
    // one's cut count (tokenCutCount, 2 B a token, the line's tokens from the first; cutShapedTokens fills it).
    std::vector<BidiLine::Token> tokens;
    size_t lineTokens = 0;
    std::vector<uint16_t> tokenCutCount;
    std::vector<std::string_view> pieceWords;
    std::vector<std::string_view> pieceForms;
    std::vector<EpdFontFamily::Style> pieceStyles;
    std::vector<TextBlock::WordSize> pieceSizes;
    std::vector<TokenBoundary::Boundary> pieceBoundaries;
    std::vector<uint8_t> pieceOrder;
    // In a paragraph that shapes: each piece's glyph run (count 0: none), and the line's cut points, which its tokens'
    // BidiLine::Token::cuts point into.
    std::vector<GlyphRun> pieceGlyphs;
    std::vector<uint16_t> tokenCuts;
    std::string tokenForm;  // the one string every BidiLine::tokenForm call writes into
  };
  std::unique_ptr<BidiState> bidiState_;
  bool rtl() const { return bidi_ && level_ == 1; }
  // Whether this pass needs bidi, and the paragraph's level; a pass that does not drops the controls
  // (dropBidiControls).
  void decideBidi();
  // Forgets the paragraph's decision and its open controls: the block's next words are a new paragraph.
  void forgetBidiParagraph();
  // Drops the bidi controls from every token, and the tokens left empty (but a ruby group's).
  void dropBidiControls();
  // The scratch for one pass: the lent arena's (`workspace`), else the build's one BidiLine::heapScratch(). Null,
  // logged, when neither can be had.
  BidiLine::Scratch* bidiScratchFor(const BidiLine::ArenaWorkspace& workspace);
  // Runs the line pass over tokens [first, first + count) into bidiState_->line, with the workspace borrowed from
  // lineArena_ for the pass alone. A token with shaped pieces is cut at their edges (cutShapedTokens). False, logged,
  // when it could not run: the line is then laid out without bidi.
  bool layOutBidiLine(const GfxRenderer& renderer, int fontId, size_t first, size_t count);
  // Gives each of the line's tokens with more than one shaped piece its cut points (BidiLine::Token::cuts): where each
  // piece after the first begins, in bidiState_->tokenCuts, and records each token's cut count (tokenCutCount) for
  // the materialisation, which runs after the tokens are gone. A token whose cuts the heap has no room for, or that
  // break their contract, gets none (logged, latched): its pieces are then drawn as the line pass's code points.
  void cutShapedTokens(const GfxRenderer& renderer, int fontId, size_t first, size_t count, BidiLine::Token* tokens);
  // What each of the line's pieces is drawn as, in bidiState_'s pieceForms and pieceGlyphs, and its width, in
  // lineXPosScratch_: the line pass's display form and its measure, unless the piece draws a shaped token's store piece
  // that is not Text (a glyph run, or a Reordered form, at the store's width). An empty run (a letter its glued
  // neighbour's ligature took) is kept for leaving off only where that ligature is drawn from its glyphs, else drawn
  // as its code points. Returns whether any piece has a glyph run or is left off.
  bool formBidiPieces(const GfxRenderer& renderer, int fontId, size_t first);
  // Takes the pieces formBidiPieces left off out of bidiState_->line and the vectors it filled: the gap before each
  // goes to the piece after it, and the reading order is numbered again.
  void dropEmptyPieces();
  // Whether bidiState_->line is the line's tokens as they stand: in order, one piece each, each drawn as its text. Such
  // a line of a left-to-right paragraph is laid out by the ordinary path.
  bool bidiLineIsPlain(size_t first, size_t count) const;
  // Places bidiState_->line's pieces of the line that starts at token `first` in visual order, as upstream #1700's
  // reordered loop does: fills lineXPosScratch_, the piece vectors and `range`. Returns the line's largest word size
  // percent.
  uint8_t placeBidiLine(const GfxRenderer& renderer, int fontId, size_t first, int effectivePageWidth, int lineIndent,
                        bool isLastLine, TextBlock::WordRange& range);
  // Points `range` (set up for tokens [first, first + count)) at those tokens without their bidi controls, as a pass
  // that needs no bidi has them: for a line the line pass could not run on. Views, with the copies in
  // bidiState_->line.display.
  void viewWithoutControls(size_t first, size_t count, TextBlock::WordRange& range);
  // Steps bidiState_->openControls over the tokens [first, end) of a line that was accepted.
  void stepOpenControls(size_t first, size_t end);

  // A paragraph whose tokens hold Indic text, or Arabic text in a font that can shape it, forms them (writing modes
  // step 2, "How shaping runs" 2-10; PR 4b for Arabic and for paragraphs that need bidi): each such token is shaped
  // once into a paragraph store, in step with the word vectors, whose widths break the lines and whose pieces the line
  // is drawn as. Boxed, and defined in ParsedText.cpp, so this header includes nothing of the shaper (it is included
  // far beyond layout). Made by the first pass that shapes; reset() empties it for the next paragraph;
  // releaseLayoutScratch() frees it when no paragraph in progress shapes. Nothing in it holds a face or a plan: they
  // are the renderer's, for one build.
  struct ShapingState;
  std::unique_ptr<ShapingState> shapingState_;
  // This pass forms its tokens: one holds U+0900-0DFF, or one that holds Arabic text has a face that draws Arabic
  // (decideShaping).
  bool shapes_ = false;
  bool* shapingLatch_ = nullptr;
  // The build's cache of shaped words (WordShapeCache.h), the parser's; null: every token is shaped. An unglued token
  // that shapes Formed is looked up there before it is shaped, and stored once it is.
  WordShapeCache* wordCache_ = nullptr;
  uint32_t tokensShaped_ = 0;
  // The first token the next pass may not merge (keepUnmergedFrom); SIZE_MAX: none. The pass clears it.
  size_t unmergedFrom_ = SIZE_MAX;
  // Const: the latch is the parser's, and a const lookup (shapingFaceFor) may be the one to find a refusal.
  void latchShapingDegraded() const {
    if (shapingLatch_ != nullptr) *shapingLatch_ = true;
  }
  // Whether this pass shapes, decided after decideBidi: a token holds Indic text, or a token that holds Arabic text in
  // a paragraph that needs bidi has a face that maps beh, or a token that holds Myanmar text has a face that maps ka
  // (IndicShaper::draws), so an Arabic or Myanmar paragraph in a bitmap or built-in font, or in a TrueType face without
  // its script, is laid out as without shaping, with no ShapingState.
  // Makes ShapingState for it (refused: the pass is laid out unshaped, and latched), or empties the store of a pass
  // that does not shape. A kept token whose glued neighbour has just arrived is shaped again, with it as context.
  void decideShaping(const GfxRenderer& renderer, int fontId);
  // Whether a pass that shapes shapes `text`: it holds Indic text, Arabic text in a paragraph that needs bidi, or
  // Myanmar text. An Arabic run needs an AL letter, which makes the paragraph need bidi; arabic::holdsArabic alone also
  // passes the Arabic-Indic digits and the Arabic comma, which no run takes.
  bool shapedText(std::string_view text) const;
  // Joins each glued group's tokens that TextShaping::mergesWithPrevious merges (deviation 7, and the Hebrew name)
  // before anything is measured: a merged token keeps its first part's style, size, boundary and visible offset. Ruby
  // tokens, combine requests and no-break spaces are never merged; nor are tokens whose merged text the heap has no
  // room for (latched).
  void mergeGluedGroups(int fontId);
  // Whether tokens a and b are in one font, style, size and emphasis mark: a span split of one style.
  bool sameStyle(int fontId, size_t a, size_t b) const;
  // Token i's entry made stale, so it is shaped again: a glued neighbour it was shaped with has changed.
  void forgetShaping(size_t i);
  // The store's room for this pass, reserved from its estimate, and an entry for every token. False (logged, latched,
  // the store emptied) when the entries cannot grow: the pass is then laid out unshaped.
  bool readyShapedStore();
  // The face token i is shaped with, and the ppem (26.6) its glyphs are drawn at: the face's, times the font's base
  // scale, times the word's. Null (and 0) on a vertical line, which reorders instead, and for a font that cannot shape.
  // The renderer is asked once a pass for each font and style (ShapingState's face answers), so every key and every
  // call of a pass reads one answer. Asked only for a token that holds Indic, Arabic or Myanmar text: a face the heap
  // refused is null too, and latches shaping degraded, since the token is then laid out as without a face.
  ShapingFace* shapingFaceFor(const GfxRenderer& renderer, int fontId, size_t i, uint32_t* drawnPpem) const;
  // Forgets the pass's face answers: a face lives for the build, and a font set reload between passes frees it.
  void forgetFaceAnswers();
  // What token i's entry is valid for besides its text: its font, style bits, size and scale, the font's base scale,
  // and the drawn ppem (0 without a face). Records shaped at one size are never reused at another.
  uint32_t shapeKey(const GfxRenderer& renderer, int fontId, size_t i) const;
  // `text`, as token i's font, style and size draw it, shaped with `before` and `after` as context into the store's
  // open entry, in a call block of lineArena_'s (else the build's heap block) released before it returns.
  IndicShaper::Outcome shapeInto(const GfxRenderer& renderer, int fontId, size_t i, std::string_view text,
                                 std::string_view before, std::string_view after);
  // Token i's pieces, found in the store, else replayed from the word cache or shaped now, their glyphs warmed and
  // their widths set, and *width their sum; false: it is laid out as its code points (and latched when it could not be
  // shaped, or its pieces measured for want of memory).
  bool shapeToken(const GfxRenderer& renderer, int fontId, size_t i, uint16_t* width);
  // *width, the sum of token i's stored pieces' widths, and true; false when the store holds none for it. Nothing is
  // shaped: a paragraph that needs bidi shapes its tokens before the line pass's workspace is borrowed.
  bool storedWidth(const GfxRenderer& renderer, int fontId, size_t i, uint16_t* width) const;
  // The width of words[i]'s first `bytes` bytes and a hyphen when `hyphen`: a candidate the breaker measures, shaped in
  // the store's scratch entry (nothing is kept), or measured as code points when it is not shaped (and latched when it
  // could not be, or its pieces measured). The candidate's string was reserved by hyphenateWordAtIndex.
  int shapedPrefixWidth(const GfxRenderer& renderer, int fontId, size_t i, size_t bytes, bool hyphen);
  // Whether token i's first piece (its last, when `last`) is a glyph run: a glued gap there takes no kerning (step 8).
  bool glyphEdge(const GfxRenderer& renderer, int fontId, size_t i, bool last) const;
  // A line of a paragraph that shapes, tokens [first, first + count) placed: when a token has pieces, each token
  // becomes its pieces, in order (step 7), and true. The first piece keeps the token's boundary, the others are glued;
  // each takes the token's style and size; a Glyphs piece is drawn as its run, a Reordered piece as its form (without
  // its soft hyphens), and a piece that draws nothing (a Text piece of soft hyphens alone, an empty glyph run: a letter
  // its glued neighbour's ligature took, where that ligature is drawn) is left out. lineXPosScratch_ then holds the
  // pieces' positions (the token's x plus the widths before it), the annotations name pieces, and `range` is pointed at
  // ShapingState's vectors. False, nothing changed, when no token has pieces: the line keeps its bytes. So does a line
  // the heap has no room to lay out as pieces, drawn as its tokens' code points (latched). `withoutControls`: a line of
  // a paragraph that needs bidi, laid out without the line pass: its texts are copies without their bidi controls (a
  // Text piece's without its joiners too, as it was measured), in bidiState_->line.display, which the pass that did not
  // run leaves free.
  bool viewShapedPieces(const GfxRenderer& renderer, int fontId, size_t first, size_t count,
                        TextBlock::WordRange& range, bool withoutControls);

  LinePlace linePlace_;
  BlockStyle blockStyle;
  // The book's typography (TypographyProfile.h): Latin rules until the parser sets the book's.
  TypographyProfile typography_;
  bool vertical_ = false;  // see setVertical
  // The reader's Paragraph Indent (ParagraphIndent): see setParagraphIndent.
  uint8_t paragraphIndent_ = static_cast<uint8_t>(ParagraphIndent::FromBook);
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool bionicReadingEnabled;
  bool isContinuation_ = false;  ///< true after an intermediate flush; suppresses re-applying paragraph indent
  // Set (and never cleared) when addWord could not grow the word vectors within the largest
  // free heap block: the word was dropped and the parser must abort the parse at its next
  // layout gate. std::vector growth cannot fail gracefully under -fno-exceptions, so the
  // check has to happen before the reserve.
  bool wordGrowthRefused_ = false;
  // The most tokens the parser lets the block hold before its long-block split (setSplitCapacity); 0: no split.
  // reserveWords grows the word arrays no further than it: a block of 97 tokens holds 97 entries, not 128.
  size_t splitCapacity_ = 0;
  size_t bionicTransformedUpTo_ = 0;  ///< words[0..bionicTransformedUpTo_) have already been bionic-transformed

  // How many tokens before tokenIndex lead a ruby group: the index of the ruby text of the first group at or after it.
  size_t leadersBefore(size_t tokenIndex) const;
  void applyParagraphIndent(const GfxRenderer& renderer, int fontId);
  void applyBionicReadingTransform();
  // Effective measurement scale of words[i]: the block-level multiplier (a word font's own scale for a word in
  // another family) combined with the word's own inline size percentage.
  float wordScale(const size_t i) const {
    const TextBlock::WordFont& font = fontSlots_[wordSizes[i].fontSlot];
    return (font.fontId != 0 ? font.scale : blockStyle.fontSizeMultiplier) * (wordSizes[i].pct / 100.0f);
  }
  // The font words[i] is measured in: its slot's, else the block's (`fontId`).
  int wordFontId(const size_t i, const int fontId) const {
    const int32_t slotFont = fontSlots_[wordSizes[i].fontSlot].fontId;
    return slotFont != 0 ? slotFont : fontId;
  }
  // The word whose font, size and style the space before words[j] (j > 0) takes: the word before it, unless the word
  // after it is in the block's font and the one before is not. A word in another family ends where its run does
  // ("Ia<span class='hand'>n</span> Carmichael"): the space is in the text around it, the block's.
  size_t spaceSource(const int fontId, const size_t j) const {
    const int right = wordFontId(j, fontId);
    return right == fontId && wordFontId(j - 1, fontId) != right ? j : j - 1;
  }
  // The space before words[j] (j > 0), unscaled (its scale is spaceSource()'s word's), in spaceSource()'s font; and
  // the kerning across a glued boundary there, none between words in two fonts.
  int spaceBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
  int kernBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
  // Returns the available line width at a given 0-based line index, accounting
  // for any active float zones in blockStyle.  lineHeight==0 is a fast path
  // (no float zones active) that returns pageWidth unchanged.
  int widthForLine(int lineIndex, int lineHeight, int16_t blockStartY, int pageWidth) const;
  // The natural gap before words[j] (j > 0), unscaled: kerning across a glued boundary, nothing at a
  // CJK gap but the CJK-Latin gap, which is a quarter of the CJK side's scaled em, else the space advance.
  // Every breaker measures gaps through this.
  int naturalGapBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
  // The CJK-Latin gap before words[j] (j > 0) in pixels, already scaled to the CJK side: callers must not
  // multiply it by wordScale. 0 unless the profile sets it and the boundary is a breakable CJK gap between
  // Han or kana and a Latin letter or digit.
  int cjkLatinGapBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
  // Whether justification stretches the gap before words[j]: not before closing punctuation (. , ) » and the like),
  // which keeps its natural width, nor after a run of kept spaces. Both line paths ask it.
  bool stretchesGapBefore(size_t j) const;
  // Whether words[i] is a run of kept spaces (see addWord).
  bool isKeptSpaceRun(size_t i) const;
  // Room in all five per-word vectors for `additional` more tokens; false (and wordGrowthRefused_)
  // when the heap cannot hold the growth.
  bool reserveWords(size_t additional);
  // The line pass's kept buffers back to the heap (bidiState_'s line, tokens and piece arrays, and lineXPosScratch_);
  // the paragraph's direction and open controls stay.
  void releaseLineBuffers();
  void pushToken(std::string token, EpdFontFamily::Style style, TokenBoundary::Boundary boundary, uint8_t sizePct,
                 uint32_t visibleOffset, uint8_t fontSlot);

  // The three breakers fill `lineBreakIndices` (cleared first) rather than returning a vector,
  // so the caller's scratch member is reused instead of reallocated per paragraph.
  // `boundaries` is always wordBoundaries itself: hyphenateWordAtIndex inserts into the member during
  // the call, and the reference sees the insert. Passing a copy would desynchronize the indices.
  void computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, std::vector<uint16_t>& wordWidths,
                         const std::vector<TokenBoundary::Boundary>& boundaries, int firstLineIndent,
                         int16_t blockStartY, int lineHeight, std::vector<size_t>& lineBreakIndices);
  void computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                   std::vector<uint16_t>& wordWidths,
                                   const std::vector<TokenBoundary::Boundary>& boundaries,
                                   std::vector<bool>& lineEndsWithHyphenatedWord,
                                   std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen,
                                   int firstLineIndent, int16_t blockStartY, int lineHeight,
                                   std::vector<size_t>& lineBreakIndices);
  // Recompute hyphenated breaks for a suffix that starts at startIndex.
  // Used after a single-line retry so later lines keep normal hyphenation.
  void computeHyphenatedLineBreaksFromIndex(const GfxRenderer& renderer, int fontId, int pageWidth,
                                            std::vector<uint16_t>& wordWidths,
                                            const std::vector<TokenBoundary::Boundary>& boundaries, size_t startIndex,
                                            std::vector<bool>& lineEndsWithHyphenatedWord,
                                            std::vector<int>& splitPrefixWordIndexes,
                                            std::vector<bool>& splitInsertedHyphen, int16_t blockStartY, int lineHeight,
                                            int startLineIdx, std::vector<size_t>& lineBreakIndices);
  // Compute exactly one line break without hyphenating words.
  // Used only for the page-boundary retry line.
  size_t computeSingleLineBreakNoHyphen(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        const std::vector<uint16_t>& wordWidths,
                                        const std::vector<TokenBoundary::Boundary>& boundaries, size_t lineStartIndex,
                                        int firstLineIndent, int16_t blockStartY = 0, int lineHeight = 0) const;
  // Splits words[wordIndex] at the widest legal break whose prefix fits `availableWidth`. *outLineEndCut: the cut is
  // how such a line ends rather than a hyphenation, which the page-bottom retry leaves: between Myanmar syllables, or
  // in text that keeps its spaces, which is never hyphenated and is cut, without a hyphen, only where a token is wider
  // than the line (allowFallbackBreaks).
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks,
                            bool* outInsertedHyphen = nullptr, bool* outLineEndCut = nullptr);
  LineProcessResult extractLine(
      size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
      const std::vector<TokenBoundary::Boundary>& boundaries, const std::vector<size_t>& lineBreakIndices,
      const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
      const GfxRenderer& renderer, int fontId, bool lineEndsWithHyphenatedWord, bool suppressHyphenationRetry,
      int firstLineIndent, int16_t blockStartY = 0, int lineHeight = 0);
  // Hands a laid-out line of tokens [first, end) over: the page-fit hook first (beforeLine_, with the line's largest
  // word size and whether it takes the annotation band), then its TextBlock, then processLine. A line whose block
  // cannot be had is dropped and counts as accepted. An accepted line steps bidiState_->openControls.
  LineProcessResult emitLine(
      const TextBlock::WordRange& range, uint8_t maxSizePct, bool band, size_t first, size_t end,
      const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
      bool lineEndsWithHyphenatedWord, bool suppressHyphenationRetry);
  // Fills `out` (cleared first); uses blockStyle.fontSizeMultiplier internally. `passControls`: the bidi controls open
  // at the first token, the pass's start (bidiState_->openControls steps on as the pass's lines are accepted).
  void calculateWordWidths(const GfxRenderer& renderer, int fontId, std::vector<uint16_t>& out,
                           const BidiLine::OpenControls& passControls);
  // Fills combined_ for a vertical line (see combines()); clears it otherwise.
  void decideCombined(const GfxRenderer& renderer, int fontId);
  // Whether token i, which asks to combine, may: see combines().
  bool fitsCombined(const GfxRenderer& renderer, int fontId, size_t i) const;
  // Narrows each CJK mark that is a token to its mid-line width and fills markClass_, edgeTrim_, shrink_ and
  // edgeShrink_. A run of kept spaces hangs at a line's end: its edgeTrim_ takes its width off there.
  void applyPunctuationWidths(const GfxRenderer& renderer, int fontId, std::vector<uint16_t>& widths);
  // Widens the base of each ruby longer than what its neighbours let it overhang, and fills rubyEdge_; a mark in a
  // group, or between overhangs, gives push-in only what the rubies leave it (shrink_).
  void applyRubyWidths(const GfxRenderer& renderer, int fontId, std::vector<uint16_t>& widths);
  // The advance of a ruby text over the group led by token `leader`: half the leader's size, its font, regular style,
  // rounded once as TextBlock draws it.
  int rubyWidth(const GfxRenderer& renderer, int fontId, size_t leader, const std::string& text) const;
  // How token i's width changes when a line starts at it, and when a line ends after it: a CJK mark's line-edge
  // width, and the overhang of a ruby group's ruby that the line edge has to hold.
  int lineStartTrim(const size_t i) const {
    return (opensLine(i) ? edgeTrim_[i] : 0) + (rubyEdge_.empty() ? 0 : rubyEdge_[i].start);
  }
  int lineEndTrim(const size_t i) const {
    return (opensLine(i) ? 0 : edgeTrim_[i]) + (rubyEdge_.empty() ? 0 : rubyEdge_[i].end);
  }
  bool opensLine(const size_t i) const { return markClass_[i] == static_cast<uint8_t>(CjkPunctuation::Class::Opening); }
  // A line first..last as drawn: its first and last token at their line-edge widths.
  int withEdgeTrims(const int naturalWidth, const size_t first, const size_t last) const {
    return naturalWidth + lineStartTrim(first) + lineEndTrim(last);
  }
  // A mark that is not an opening one: a line's end edge acts on it. An ideographic space or an ambiguous quote passes
  // too, with nothing to give.
  bool closesLine(const size_t i) const {
    return markClass_[i] != static_cast<uint8_t>(CjkPunctuation::Class::None) && !opensLine(i);
  }
  // What the gap of `gapBefore` px before token j may give under push-in: at a breakable CJK gap (where only a
  // CJK-Latin gap has a width) all but half of it, rounded down, as JLREQ lets a quarter-em gap shrink to 1/8 em; a
  // word space or a glued seam nothing.
  int gapShrinkBefore(const size_t j, const int gapBefore) const {
    const TokenBoundary::Boundary& boundary = wordBoundaries[j];
    return !boundary.continues && boundary.noSpaceBefore ? gapBefore - gapBefore / 2 : 0;
  }
  // A line's push-in total with its first and last token giving their line-edge shrinks: `shrinkSum` is the line's
  // mid-line sum, each token's shrink_ and each gap's gapShrinkBefore.
  int withEdgeShrinks(const int shrinkSum, const size_t first, const size_t last) const {
    int sum = shrinkSum;
    if (opensLine(first)) sum += edgeShrink_[first] - shrink_[first];
    if (closesLine(last)) sum += edgeShrink_[last] - shrink_[last];
    return sum;
  }
  // The line first..last with everything that can give way at its minimum, what a breaker holds against the width
  // (spec 5.3): `naturalSum` is its mid-line widths and gaps, `shrinkSum` what they may give.
  int pushedInWidth(const int naturalSum, const int shrinkSum, const size_t first, const size_t last) const {
    return withEdgeTrims(naturalSum, first, last) - withEdgeShrinks(shrinkSum, first, last);
  }

 public:
  // Out of line, as the destructor and the moves are: ShapingState is complete only in ParsedText.cpp.
  explicit ParsedText(bool extraParagraphSpacing, bool hyphenationEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), bool bionicReadingEnabled = false);
  ~ParsedText();
  // Movable, not copyable: it owns its word vectors and buffers. The line pass's scratch is the build's
  // (BidiLine::heapScratch), not its own.
  ParsedText(ParsedText&&) noexcept;
  ParsedText& operator=(ParsedText&&) noexcept;

  // Start a new, empty block in this object under `blockStyle`, keeping the word vectors'
  // capacity. The parser reuses one ParsedText across a chapter's paragraphs: a fresh object
  // per paragraph regrew four vectors from 16 to 128 entries each time -- ~16 allocations and
  // ~3 KB of heap traffic per paragraph, 15,000 allocations per book on the host census, the
  // single largest churn of a section build (memory audit 2026-09, R2).
  void reset(const BlockStyle& blockStyle);
  // Give the layout scratch (and the word vectors, when the block is empty) back to the heap.
  // The parser calls this when an incremental build yields a slice: that is the one moment a
  // mid-build page draw can run, and its ~10.5 KB Page sits on the heap next to whatever the
  // parse holds. Keeping ~8 KB of scratch resident across that moment cost the X3 5 KB of
  // minimum free heap (watermark 11.3 KB -> 6.4 KB, device run 7); dropping it at the yield
  // keeps the no-churn behaviour within a slice and the old floor across it.
  void releaseLayoutScratch();
  // At a paragraph's end, what the paragraph grew past a prose paragraph's needs goes back to the heap: the line
  // pass's kept buffers when a long line took them past kKeptLineBytes (a prose line leaves about 1.5 KB), and the
  // shaped store when it holds more than kKeptStoreBytes (a prose paragraph's is 4 to 6 KB). Kept, they would sit in
  // front of whatever follows until the slice's yield: the table row after test_shaped_arabic's span-per-word
  // paragraph met a heap some 18 KB lower for them. Below the thresholds nothing changes, so prose regrows nothing.
  void releaseOversizedLayoutBuffers();
  static constexpr size_t kKeptLineBytes = 2 * 1024;
  static constexpr size_t kKeptStoreBytes = 8 * 1024;
  size_t keptLineBytes() const;   // the line pass's kept buffers (capacity, in bytes)
  size_t keptStoreBytes() const;  // the shaped store's index and chunks

  // Adds a word from the source: a Latin word as one token, a word with CJK characters as one token per
  // break the CJK rules allow (CjkLineBreak.h). attachToPrevious: no whitespace separated it from the
  // previous word in the source.
  // fontSlot: the word's font slot (see setFontSlot), below TextBlock::kMaxWordFonts.
  // textEmphasis: the word's CSS text-emphasis mark (TextEmphasis), 0 for none; every token of the word carries it.
  // textCombine: the word's CSS text-combine-upright (a CssTextCombine). Under `all` every token of the word asks to
  // combine, under `digits N` a token of 1 to N ASCII digits and nothing else; see combines().
  // keptSpaces: the word is text whose spaces the book keeps (CSS white-space pre, pre-wrap, break-spaces; <pre>). Its
  // tokens are never hyphenated. A word of nothing but spaces is then a run of kept spaces: one token, as wide as its
  // spaces, glued to the token before it, that a line may end after (where it hangs: it takes no room at the line's
  // end) but not before. The word after a run starts at no gap of its own, and no gap a run makes is stretched.
  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint8_t sizePct = DEFAULT_WORD_SIZE_PCT, uint32_t visibleOffset = 0, uint8_t fontSlot = 0,
               uint8_t textEmphasis = 0, uint8_t textCombine = 0, bool keptSpaces = false);
  // Tokens [firstToken, firstToken + tokenCount) become one ruby group: the first keeps its boundary (a break may
  // come before the group), the rest are glued to it. False when the range is empty, runs past size(), or a token
  // in it already has a role.
  bool addRubyGroup(size_t firstToken, size_t tokenCount, std::string text);
  // An <rt> with no new base: appended to the block's last group. False when the block has none.
  bool appendRubyText(std::string_view text);
  // A bidi control the parser puts in after the word it ends had gone out (a closer at an end tag after a nested
  // element's), appended to the last token. It counts nowhere, so the token's offset stays. False, nothing appended,
  // when there is no token or the last is a no-break space (" ", which must stay that token).
  bool appendControlToLastToken(std::string_view control);
  // The face the words in `slot` are laid out in; a fontId of 0 (every slot after reset()) is the block's font. The
  // parser fills the slots its block's words use once it knows the block's font (see fontSlotsUsed).
  void setFontSlot(const uint8_t slot, const TextBlock::WordFont& font) {
    if (slot < TextBlock::kMaxWordFonts) fontSlots_[slot] = font;
  }
  // The slots the block's words have been in, a bit per slot.
  uint8_t fontSlotsUsed() const { return fontSlotsUsed_; }
  // True once addWord had to drop a word because the word vectors could not grow (see
  // wordGrowthRefused_). ChapterHtmlSlimParser::ensureHeapForTextLayout turns it into a
  // partial-cache abort.
  bool wordGrowthRefused() const { return wordGrowthRefused_; }
  // If every word shares one non-100% size (a span wrapping the whole paragraph, e.g.
  // Alice's mouse-tale lines), fold that percent into the block-level fontSizeMultiplier
  // and reset the per-word sizes to 100. This routes whole-paragraph spans through the
  // block font-resolution path (size ladder) so they render with a real smaller font
  // instead of per-word glyph scaling. Returns true when a fold happened. Must be called
  // before the first layout pass of the block; callers skip continuations.
  bool foldUniformWordSizes();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  // Survives reset(): it is the book's, not the block's.
  void setTypography(const TypographyProfile& typography) { typography_ = typography; }
  // Measure words as a column runs them (a vertical section's). Survives reset(): it is the section's.
  void setVertical(bool vertical) { vertical_ = vertical; }
  // The reader's Paragraph Indent, a stored ParagraphIndent. Survives reset(): it is the section's. A table cell's
  // text never takes it, so a cell keeps Book's Style.
  void setParagraphIndent(const uint8_t indent) { paragraphIndent_ = indent; }
  // See lineArena_ / beforeLine_. Both survive reset().
  void setLineArena(BuildArena* arena) { lineArena_ = arena; }
  // A run laid out as its code points because it could not be shaped (no memory, or the font failed it) sets *latch
  // (Section's kStatusShapingDegraded). Survives reset().
  void setShapingLatch(bool* latch) { shapingLatch_ = latch; }
  // See wordCache_. The parser sets it beside setLineArena; it survives reset().
  void setWordCache(WordShapeCache* cache) { wordCache_ = cache; }
  bool shapingStateHeld() const { return shapingState_ != nullptr; }  // for tests
  // Tokens handed to IndicShaper since construction (a candidate prefix the breaker measures is not one): for tests
  // and the build's log.
  uint32_t tokensShaped() const { return tokensShaped_; }
  // Tokens from `first` on are not merged by the next layout pass: the bases of a ruby still open, which the parser
  // counts until their reading makes them a group (a ruby token is never merged). The pass clears it.
  void keepUnmergedFrom(const size_t first) { unmergedFrom_ = first; }
  // The tokens as the parser added them: size(), with each merged token counted as its parts. A footnote's anchor is
  // placed by it, as lastLineTokenCount() counts the lines.
  size_t sourceTokenCount() const;
  void setBeforeLineHook(std::function<void(uint8_t maxSizePct, bool annotated)> hook) {
    beforeLine_ = std::move(hook);
  }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  // The most tokens the block holds before the parser splits it (ChapterHtmlSlimParser::flushPartWordBuffer): the
  // word arrays grow to it and no further, and past it by 16s (a ruby group may hold a split up a few tokens). 0: the
  // arrays double as they grow, as a table cell's do.
  void setSplitCapacity(const size_t tokens) { splitCapacity_ = tokens; }
  size_t wordCapacity() const { return words.capacity(); }
  uint32_t lastLineVisibleOffset() const { return lastLineVisibleOffset_; }
  uint32_t lastLineTokenCount() const { return lastLineTokenCount_; }
  const LinePlace& linePlace() const { return linePlace_; }
  std::optional<uint32_t> firstWordVisibleOffset() const {
    if (wordVisibleOffsets.empty()) return std::nullopt;
    return wordVisibleOffsets.front();
  }
  bool isEmpty() const { return words.empty(); }
  bool isContinuation() const { return isContinuation_; }
  // Whether words[wordIdx] is set upright as one block, one em down its column (tate-chu-yoko). On a vertical line
  // only, for a token that asks to (addWord's textCombine) and is the whole of its flagged run (no flagged neighbour
  // without a space between), of at most 4 code points with none drawn upright, and no wider than the em at
  // kMinCombineScale. Decided by each layout pass where it measures the words; false on a horizontal line.
  bool combines(const size_t wordIdx) const { return wordIdx < combined_.size() && combined_[wordIdx] != 0; }

  // CONSUMES ITS SOURCE by default: every word it lays out is erased from `words` (and the
  // parallel per-word vectors) before returning, so the vector shrinks as the paragraph drains.
  // A caller that needs to lay the SAME text out twice must pass preserveSource.
  //
  // This was previously documented as preserving its source, which was wrong and cost content: the
  // table grid path laid a cell out to discover it needed more lines than the grid can carry, then
  // handed the same ParsedText to emitTableAsParagraphs — by which point every word of that cell,
  // and of every cell laid out before it, had already been erased. The whole cell rendered as
  // nothing. Fixture: test_table_grid_edges.epub ch3.
  //
  // `preserveSource` snapshots the words on entry and restores them on return, so the caller gets
  // back exactly what it passed in. Suppressing the erase alone would not be enough: layout also
  // force-splits any word too wide for the line (inserting a hyphen), and applies the paragraph
  // indent and bionic transform in place. It costs one copy of the word vectors for the duration
  // of the call, so it is opt-in and only the table grid path uses it.
  // Test seam: false lays out every gap with GfxRenderer::getSpaceAdvance(), without the pass's memory of its parts.
  inline static bool rememberSpaceParts = true;

  void layoutAndExtractLines(
      const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
      const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
      bool includeLastLine = true,
      int16_t blockStartY = 0,  // currentPageNextY at call site — needed for float zone geometry
      int lineHeight = 0,       // 0 = no float zones (fast path, existing callers unchanged)
      bool preserveSource = false);
};