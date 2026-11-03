#pragma once

#include <BidiUtils.h>
#include <BuildArena.h>
#include <TextShaping.h>
#include <Utf8.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The bidi line pass (design decisions 5 and 8): one UAX #9 pass over a whole line turns its tokens into pieces in
// visual order. A piece is a run of one token's characters that is unbroken both on the line and in the text, so a
// word whose characters land apart is split into glued pieces. Isolates are ordered on their own; transparent marks
// ride on their base; past kMaxCodepoints slots the line takes upstream's result (#2541's computeVisualWordOrder with
// per-word forms). Pure: no renderer and no fonts; a line that fits allocates nothing beyond the Line's own buffers,
// whose capacity the caller keeps.
namespace BidiLine {

inline constexpr size_t kMaxCodepoints = BidiUtils::kMaxLineCodepoints;  // slots: marks ride on their base
inline constexpr uint8_t kMaxOpenControls = 8;
inline constexpr size_t kMaxUnits = 3 * kMaxCodepoints;  // code points a line may hold, marks included

// U+200E, 200F, 061C, 202A-202E, 2066-2069: the marks, embeddings, overrides and isolates.
bool isControl(uint32_t cp);

// U+200E, 200F, 061C and 202A-202E, the controls that are default-ignorable too (utf8IsDefaultIgnorable). The parser
// keeps them for the pass, but they count nowhere: in no content offset (KOSync), and in no parse-time read of a word's
// first or last character. The isolates are not default-ignorable there, and count as the text they always were.
inline bool isUncountedControl(const uint32_t cp) { return isControl(cp) && utf8IsDefaultIgnorable(cp); }

// Removes them; true when it removed any.
bool dropControls(std::string& text);

// The embeddings, overrides and isolates open, outermost first.
struct OpenControls {
  uint32_t cps[kMaxOpenControls] = {};
  uint8_t count = 0;
  uint8_t overflow = 0;  // openers past kMaxOpenControls; their closers consume these first
  // LRE, RLE, LRO, RLO, LRI, RLI, FSI push. PDF pops the innermost entry if it is not an isolate. PDI pops up to and
  // including the innermost isolate, and does nothing when none is open (UAX #9 X6a, X7).
  void advance(std::string_view text);
};

struct Token {
  std::string_view text;
  bool spaceBefore;  // TokenBoundary::hasSpace
};

enum class Gap : uint8_t { None, Glued, Space };

struct Piece {
  uint16_t token;       // index into the line's tokens
  uint16_t begin, end;  // bytes of that token's text; a token's pieces, in logical order, cover it exactly
  uint16_t displayBegin, displayLength;  // the drawn form in Line::display
  Gap gapBefore;                         // from the piece drawn left of it; None for the first
  uint16_t spaceToken;                   // Gap::Space: the token whose word space it is (else the piece's own token)
  uint8_t readingIndex;                  // its place in reading order
};

struct Line {
  std::vector<Piece> pieces;  // visual order, left to right
  // The drawn forms: each one's characters in visual order, mirrored, marks after their base, without controls,
  // U+00AD, ZWJ or ZWNJ; each followed by a NUL, so a form's view measures as a C string. Pieces index into it.
  std::string display;
  bool reordered = false;  // a readingIndex differs from its visual index
  bool fallback = false;   // over kMaxCodepoints slots or kMaxUnits units
  void clear();            // keeps capacity

  // The fallback's working copies (its words without their marks, and their order with its inverse), kept with their
  // capacity across lines; not part of the result.
  std::vector<std::string> fallbackWords;
  std::vector<uint16_t> fallbackOrder;
};

// The pass's working memory: plain arrays, no constructor work, so a build's arena can hold it (ArenaWorkspace). The
// arrays below `order` serve the ordering; once the slots are ordered, the pieces reuse them (named where they do).
struct Scratch {
  // A span from an isolate initiator to its matching PDI, or to the line's end.
  struct Span {
    uint8_t begin;  // the initiator's slot
    uint8_t end;    // the PDI's slot, or the line's last slot
    bool closed;    // a PDI ends it
  };
  // The logical sequence; open controls and separators have no cluster. Once its pass has ordered it, a slot's base
  // unit holds the code point drawn for it (mirrored).
  TextShaping::Unit units[kMaxUnits];
  uint16_t slotUnit[kMaxCodepoints + 1];  // slot s holds units [slotUnit[s], slotUnit[s + 1]): a base and its marks
  uint16_t slotToken[kMaxCodepoints];     // the slot's token; a separator's is the token it precedes
  uint8_t order[kMaxCodepoints];          // the slots in visual order; a span's order sits on its own slots
  uint8_t expanded[kMaxCodepoints];       // one span's order while its children are expanded; then each slot's rank
  uint32_t passCp[kMaxCodepoints];        // one span's outer sequence: its direct bases, U+FFFC per child span
  uint16_t passEntry[kMaxCodepoints];     // each entry's slot, or a flag above the slots; then one run's slots
  uint16_t passVisual[kMaxCodepoints];    // orderLine's visual order of the pass; then the pieces in reading order
  uint32_t passDrawn[kMaxCodepoints];     // and what it draws there
  Span spans[kMaxCodepoints];             // in the order they open
  uint8_t openSpans[kMaxCodepoints];      // the spans open while the slots are read; then where a run is cut
};
static_assert(kMaxCodepoints <= 128, "slots and spans are indexed by uint8_t");

// Lays one line out: pieces in visual order, each with its bytes, its drawn form and the gap before it. False (out
// cleared) only when BidiUtils has no workspace, for a line past the limit too: the caller lays the line out without
// bidi. `paragraphLevel` is 0 or 1.
// Tokens are fewer than 65,535 and each shorter than 64 KB; a line past 256 pieces (only a fallback line can be one)
// has no exact readingIndex.
bool layoutLine(const Token* tokens, size_t count, const OpenControls& open, uint8_t paragraphLevel, Scratch& scratch,
                Line& out);

// A token's own drawn form, which breaking measures: TextShaping::shape, then the bidi of applyBidiVisual, then
// dropControls, and U+00AD, ZWJ and ZWNJ dropped. No isolate initiator is ever prepended (minibidi has no isolating
// run sequences).
// - With an isolate open, the token is formed at that isolate's level: the innermost of `open`'s LRI 0, RLI 1, or FSI
//   detectParagraphLevel(token, 0). Only the embeddings and overrides opened inside that isolate are prepended.
// - With none open: at detectParagraphLevel(token, paragraphLevel), with every open embedding and override prepended.
// False, out untouched, when the form is the token's text byte for byte. `out` is the caller's reused string, and
// `token` must not view it.
bool tokenForm(std::string_view token, const OpenControls& open, uint8_t paragraphLevel, Scratch& scratch,
               std::string& out);

// Lends a block of `arena` holding the workspace and a Scratch for one scope; unbinds and releases it on destruction.
// scratch() is null with no arena or no room (the caller then uses its own heap Scratch). Its block nests LIFO with
// the arena's other blocks: the scope ends before anything below it is released. One at a time: its end unbinds the
// workspace, whoever bound it.
class ArenaWorkspace {
 public:
  explicit ArenaWorkspace(BuildArena* arena);
  ~ArenaWorkspace();
  ArenaWorkspace(const ArenaWorkspace&) = delete;
  ArenaWorkspace& operator=(const ArenaWorkspace&) = delete;

  Scratch* scratch() const { return scratch_; }

 private:
  BuildArena* arena_ = nullptr;
  BuildArena::Block block_;
  Scratch* scratch_ = nullptr;
};

}  // namespace BidiLine
