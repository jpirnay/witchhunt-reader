#pragma once

#include <BuildArena.h>
#include <GlyphRun.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ArabicScripts.h"
#include "IndicScripts.h"
#include "MyanmarScript.h"

class ShapingFace;
namespace ot {
struct Buffer;
}

// Ours (this repo's): the shaping module of writing modes step 2, for the Indic scripts and, since PR 4b, Arabic runs
// too, and Myanmar runs since the Myanmar design's PR 2 (the name is PR 3's). Its text rules (which runs a token
// holds), and shapeToken, which turns a token into pieces over a ShapingFace at section build, each run under its own
// script. The run scan, the piecewise loop of a long run and the glyph rounding are ported from crosspoint-reader PR
// #3787 ("feat: add Indic language support without HarfBuzz", SSafayet / @SSafayet).
namespace IndicShaper {

// What a run is written in.
enum class RunKind : uint8_t { Indic, Arabic, Myanmar };

// One run of a token, bytes [begin, end): upstream's scanRun for Indic text, one script's letters with the shared code
// points around them (dandas, Vedic signs, joiners, the dotted circle), an Arabic run (arabic::isRunMember code
// points with ZWJ and ZWNJ inside), or a Myanmar run (myanmar::isRunMember code points with the joiners and the dotted
// circle among them). An Arabic or Myanmar run's `script` is null; an Indic run's is null for shared code points
// alone.
struct Run {
  uint16_t begin;
  uint16_t end;
  const indic::ScriptInfo* script;
  RunKind kind = RunKind::Indic;
};

// The most runs a token of `bytes` bytes can hold: an Arabic run takes 2 bytes at least (an Indic one 3), and two runs
// abut with no byte between them.
constexpr size_t maxRuns(size_t bytes) { return bytes / 2 + 1; }

// The token's runs, in order, into `out` (room for maxRuns(text.size())); text outside them is not listed. An Arabic
// run starts at a run member that is not a mark; an Arabic letter ends an Indic run, and an Indic code point (shared
// ones included) ends an Arabic run, except that a ZWJ or ZWNJ after an Arabic member stays in its run. A Myanmar run
// starts at a member, or at the joiners and dotted circles just before one, and ends at the first code point that is
// neither a member nor one of those; a Myanmar member ends an Indic or an Arabic run. A token of more than UINT16_MAX
// bytes has none listed.
size_t scanRuns(std::string_view text, Run* out);

// Whether `text` holds a byte sequence of U+0900-0DFF (indic::startsIndic), bounded by the view.
bool holdsIndic(std::string_view text);

// Whether `text` holds Indic text, a byte sequence of the Arabic blocks (arabic::holdsArabic) or a Myanmar member
// (myanmar::holdsMyanmar): a token the scan may find a run in.
bool holdsShaped(std::string_view text);

// What a token's text is drawn as, piece by piece; the pieces cover the text in order.
enum class PieceKind : uint8_t { Text, Glyphs, Reordered };

// Where shapeToken writes a token's pieces (ParsedText's paragraph store). Each call returns false when it has no
// room: the token is then laid out as its code points.
class PieceSink {
 public:
  virtual ~PieceSink() = default;
  virtual bool beginPiece(PieceKind kind, uint16_t begin, uint16_t end) = 0;  // bytes of the token's text
  virtual bool addGlyph(const GlyphRecord& record) = 0;         // a Glyphs piece's records, in visual order
  virtual bool addBytes(const char* bytes, size_t length) = 0;  // a Reordered piece's UTF-8 form
};

// The longest run one ot::shape call takes (its buffer has 2n + 16 slots); a longer one is shaped in pieces. 32 from a
// Malayalam and a Tamil novel: a run past 32 code points is in 0.031% and 0.0005% of their tokens, and none passes 48.
constexpr unsigned kMaxRunCodepoints = 32;
// The syllable of a glued Indic or Myanmar neighbour, kept apart (deviation 7), that a token is shaped with as context,
// at most. A Myanmar syllable is the break rule's (myanmar::breakAllowed): 0.2% of those in the design's corpora are
// longer.
constexpr unsigned kContextCodepoints = 8;
// The code points of a glued Arabic neighbour, kept apart, that a token is shaped with as context, at most: its whole
// clusters nearest the token (a letter with the marks and joiners after it), as many as fit; one that does not fit is
// left out, with all those farther.
constexpr unsigned kArabicContextCodepoints = 16;

struct Request;
enum class Outcome : uint8_t;

// What a call's block is laid out for. A call with a face (Request::face) shapes: the Buffer and the rest. A call
// without one (a bitmap or built-in font) only reorders, so its block is IndicReorder's form alone; shaping in it is
// Degraded.
enum class CallKind : uint8_t { Shape, Reorder };

// The memory one token is shaped in: the Buffer, its storage (ot::Buffer::bytesFor(2 * runCodepoints + 16)), the
// token's runs, its code points with their context and IndicReorder's form of it (3 * tokenBytes + 1 B; the form
// alone for CallKind::Reorder), in one block of `arena` for this call alone, else in the build's heap block (made on
// first use, grown nothrow to the longest token, freed by TextShaping::releaseBuildMemory(); lent to one call at a
// time, so a second while it lives is invalid). Released before any glyph is looked up: LIFO with the TrueType miss
// scope.
class CallMemory {
 public:
  CallMemory(BuildArena* arena, size_t tokenBytes, CallKind kind = CallKind::Shape,
             unsigned runCodepoints = kMaxRunCodepoints);
  ~CallMemory();
  CallMemory(const CallMemory&) = delete;
  CallMemory& operator=(const CallMemory&) = delete;
  bool valid() const;
  // The bytes such a call's block takes, at the worst alignment: the room an arena needs to lend it (0 for a call no
  // block can serve).
  static size_t bytesFor(size_t tokenBytes, CallKind kind = CallKind::Shape,
                         unsigned runCodepoints = kMaxRunCodepoints);

  // IndicReorder's bounded form of the token: 3 * tokenBytes + 1 bytes; null and 0 when !valid().
  char* reorderBuffer() const { return reorder_; }
  size_t reorderRoom() const { return reorderRoom_; }

 private:
  friend Outcome shapeToken(std::string_view text, const Request& request, CallMemory& memory, PieceSink& sink);

  BuildArena* arena_ = nullptr;  // set when the block is the arena's
  BuildArena::Block block_;
  bool heapBlock_ = false;  // the block is the build's heap block, lent to this call
  size_t tokenBytes_ = 0;   // the longest token the parts hold
  // Null for CallKind::Reorder, as the runs and the code points are.
  ot::Buffer* buffer_ = nullptr;
  Run* runs_ = nullptr;             // room for maxRuns(tokenBytes_)
  uint32_t* codepoints_ = nullptr;  // room for codepointRoom_: a run's, with the context on either side
  size_t codepointRoom_ = 0;
  char* reorder_ = nullptr;
  size_t reorderRoom_ = 0;
};

struct Request {
  ShapingFace* face = nullptr;  // null: the font cannot shape: Indic text is reordered, Arabic and Myanmar left as text
  uint32_t ppem26_6 = 0;        // the size the glyphs are drawn at
  const uint32_t* languageTags = nullptr;  // the book's (TypographyProfile::shapingLanguage); null: the default
  std::string_view before, after;          // glued neighbours kept apart: context, never output
};

// Plain: nothing needs a form (no shaped run, or every form equals its text); nothing written.
// Formed: the pieces are written. Degraded: a run could not be shaped (no memory, the sink's room included; the font
// failed it; a record outside its fields); what was written is to be dropped and the token laid out as its code points.
enum class Outcome : uint8_t { Plain, Formed, Degraded };

// A token's pieces into `sink`, each run under its own script, in one call. With a face, per run: Indic shared code
// points alone are Text; an Indic script the face does not map (its probe letter) is Reordered when IndicReorder
// changes it, else Text; Arabic the face does not map (beh) is Text, never reordered, and so is Myanmar the face does
// not map (ka); any other run is Glyphs, shaped in the book's language with context from the glued neighbours: an
// Indic or Myanmar neighbour's nearest syllable, an Arabic neighbour's nearest whole clusters (and an Arabic run's
// joiners just before it). A Glyphs piece is the run's records
// in visual order (an Arabic run's last code point first); a run whose glyphs its context took (a lam-alef across a
// glue) is a Glyphs piece of no record. Text between runs is Text. Without a face, IndicReorder's form of the text
// between the Arabic runs, a Reordered piece where it changes it: over the whole token when it holds no Arabic run.
// Under RenderLock (the face maps the glyph store).
Outcome shapeToken(std::string_view text, const Request& request, CallMemory& memory, PieceSink& sink);

// Whether `face` draws the script whose probe letter is `probe` (arabic::kProbe, beh; myanmar::kProbe, ka): it maps it,
// shapeToken's test of each Arabic or Myanmar run, so a face that does not lays every one out as Text. True when the
// face is not mapped now (ShapingFace::current() null): shapeToken then degrades the token, as it would. Under
// RenderLock, as shapeToken.
bool draws(ShapingFace& face, uint32_t probe);

}  // namespace IndicShaper
