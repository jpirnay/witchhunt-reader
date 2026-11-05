#pragma once

#include <BuildArena.h>
#include <GlyphRun.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "IndicScripts.h"

class ShapingFace;
namespace ot {
struct Buffer;
}

// Ours (this repo's): the Indic shaping module of writing modes step 2. Its text rules (which runs a token holds), and
// shapeToken, which turns a token into pieces over a ShapingFace at section build. The run scan, the piecewise loop of
// a long run and the glyph rounding are ported from crosspoint-reader PR #3787 ("feat: add Indic language support
// without HarfBuzz", SSafayet / @SSafayet).
namespace IndicShaper {

// One run of a token, upstream's scanRun: one script's letters with the shared code points around them (dandas, Vedic
// signs, joiners, the dotted circle), bytes [begin, end); `script` null for shared code points alone.
struct Run {
  uint16_t begin;
  uint16_t end;
  const indic::ScriptInfo* script;
};

// The most runs a token of `bytes` bytes can hold: a run takes 3 bytes at least, and two runs of different scripts
// abut with no byte between them.
constexpr size_t maxRuns(size_t bytes) { return bytes / 3 + 1; }

// The token's runs, in order, into `out` (room for maxRuns(text.size())); text outside them is not listed. A token of
// more than UINT16_MAX bytes has none listed.
size_t scanRuns(std::string_view text, Run* out);

// Whether `text` holds a byte sequence of U+0900-0DFF (indic::startsIndic), bounded by the view.
bool holdsIndic(std::string_view text);

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
// The syllable of a glued neighbour, kept apart (deviation 7), that a token is shaped with as context, at most.
constexpr unsigned kContextCodepoints = 8;

// The memory one token is shaped in: the Buffer, its storage (ot::Buffer::bytesFor(2 * runCodepoints + 16)), the
// token's runs, its code points with their context and IndicReorder's form of it, in one block of `arena` for this call
// alone, else in the build's heap block (made on first use, grown nothrow to the longest token, freed by
// TextShaping::releaseBuildMemory()). Released before any glyph is looked up: LIFO with the TrueType miss scope.
class CallMemory {
 public:
  CallMemory(BuildArena* arena, size_t tokenBytes, unsigned runCodepoints = kMaxRunCodepoints);
  ~CallMemory();
  CallMemory(const CallMemory&) = delete;
  CallMemory& operator=(const CallMemory&) = delete;
  bool valid() const;

  // The block's parts, for shapeToken; null and 0 when !valid().
  size_t tokenBytes() const { return tokenBytes_; }     // the longest token the parts hold
  ot::Buffer* buffer() const { return buffer_; }        // bound to its storage
  Run* runs() const { return runs_; }                   // room for maxRuns(tokenBytes())
  uint32_t* codepoints() const { return codepoints_; }  // room for codepointRoom()
  size_t codepointRoom() const { return codepointRoom_; }
  char* reorderBuffer() const { return reorder_; }  // IndicReorder's bounded form (3 * tokenBytes() + 1 bytes)
  size_t reorderRoom() const { return reorderRoom_; }

 private:
  BuildArena* arena_ = nullptr;  // set when the block is the arena's
  BuildArena::Block block_;
  size_t tokenBytes_ = 0;
  ot::Buffer* buffer_ = nullptr;
  Run* runs_ = nullptr;
  uint32_t* codepoints_ = nullptr;
  size_t codepointRoom_ = 0;
  char* reorder_ = nullptr;
  size_t reorderRoom_ = 0;
};

struct Request {
  ShapingFace* face = nullptr;             // null: the font cannot shape: the token is reordered whole
  uint32_t ppem26_6 = 0;                   // the size the glyphs are drawn at
  const uint32_t* languageTags = nullptr;  // the book's (TypographyProfile::shapingLanguage); null: the default
  std::string_view before, after;          // glued neighbours kept apart: context, never output
};

// Plain: nothing needs a form (no Indic run, or every form equals its text); nothing written.
// Formed: the pieces are written. Degraded: a run could not be shaped (no memory, the sink's room included; the font
// failed it; a record outside its fields); what was written is to be dropped and the token laid out as its code points.
enum class Outcome : uint8_t { Plain, Formed, Degraded };

// A token's pieces into `sink`. With a face, per run: shared code points alone are Text; a script the face does not
// map is Reordered when IndicReorder changes it, else Text; any other run is Glyphs, shaped in the book's language with
// the glued neighbours' syllables as context. Text between runs is Text. Without a face, one Reordered piece over the
// whole token when IndicReorder changes it. Under RenderLock (the face maps the glyph store).
Outcome shapeToken(std::string_view text, const Request& request, CallMemory& memory, PieceSink& sink);

}  // namespace IndicShaper
