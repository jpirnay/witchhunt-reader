#include "IndicShaper.h"

#include <Logging.h>
#include <OtShaper.h>
#include <ShapingFace.h>

#include <cstring>
#include <new>

#include "IndicReorder.h"
#include "TextShaping.h"

// Ours (this repo's), with three algorithms ported from crosspoint-reader PR #3787 ("feat: add Indic language support
// without HarfBuzz", SSafayet / @SSafayet), lib/EpdFont/ComplexShaper.cpp at 0fbd1575:
// - scanRun, as scanRuns, over a bounded view and listing every run;
// - the piecewise loop of appendShapedRun, as shapeRun, over a run with its glued neighbours' syllables as context;
// - the rounding of appendGlyphs, as appendGlyphs, into GlyphRecords that refuse what their fields cannot hold.
// Upstream shapes in the renderer, through a run cache; here a token is shaped once at section build, into pieces, in
// the call's own memory (writing modes step 2, deviations 2, 6, 7 and 9).
namespace IndicShaper {

namespace {

// ot::shape's growth limit for a run of n code points: 2n + 16 glyphs (OtShaper.cpp's MAX_LENGTH_FACTOR and
// MAX_LENGTH_EXTRA).
constexpr unsigned kGrowthFactor = 2;
constexpr unsigned kGrowthExtra = 16;
// Every code point a run holds is 3 bytes of UTF-8: the letters of U+0900-0DFF, the dandas, the joiners and the dotted
// circle alike.
constexpr size_t kRunCodepointBytes = 3;

// Lays a block's parts end to end, each at an address aligned for its type. Without a block it only measures: the
// bytes they need at the worst padding.
class Carver {
 public:
  explicit Carver(uint8_t* block = nullptr) : at_(block) {}

  template <typename T>
  T* take(const size_t count) {
    const size_t size = count * sizeof(T);
    if (at_ == nullptr) {
      bytes_ += size + alignof(T) - 1;
      return nullptr;
    }
    const size_t misalignment = reinterpret_cast<uintptr_t>(at_) % alignof(T);
    uint8_t* const start = at_ + (misalignment != 0 ? alignof(T) - misalignment : 0);
    at_ = start + size;
    return reinterpret_cast<T*>(start);
  }
  size_t bytes() const { return bytes_; }

 private:
  uint8_t* at_;
  size_t bytes_ = 0;
};

struct Sizes {
  bool shapes;        // CallKind::Shape: the Buffer and the three after it are laid out
  size_t storage;     // the Buffer's
  size_t runs;        // maxRuns(tokenBytes)
  size_t codepoints;  // a run's, with the context on either side
  size_t reorder;     // IndicReorder's bounded form: 3 * tokenBytes + 1
};

struct Parts {
  void* buffer;
  uint8_t* storage;
  Run* runs;
  uint32_t* codepoints;
  char* reorder;
};

// The parts in their order; measured when `carver` has no block. A reordering call's block is its form alone.
Parts layOut(Carver& carver, const Sizes& sizes) {
  Parts parts{};
  if (sizes.shapes) {
    parts.buffer = carver.take<ot::Buffer>(1);
    parts.storage = carver.take<uint8_t>(sizes.storage);
    parts.runs = carver.take<Run>(sizes.runs);
    parts.codepoints = carver.take<uint32_t>(sizes.codepoints);
  }
  parts.reorder = carver.take<char>(sizes.reorder);
  return parts;
}

// 26.6 -> whole pixels, rounding half away from zero (upstream's roundPixels), in 64 bits so that no value overflows.
constexpr int64_t roundPixels(const int64_t v) { return v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64); }

// The glyphs `buffer` holds into `sink` as records, rounded as upstream's appendGlyphs rounds them: the advance from
// 26.6 to 12.4 pixels, the offsets to whole pixels with y down, in visual order. False when a record falls outside its
// fields (refused, never clamped) or the sink has no room.
bool appendGlyphs(const ot::Buffer& buffer, PieceSink& sink) {
  for (unsigned i = 0; i < buffer.len(); i++) {
    const ot::GlyphPosition& pos = buffer.pos[i];
    const uint32_t glyph = buffer.info[i].codepoint;
    const int64_t advance = (static_cast<int64_t>(pos.xAdvance) + 2) >> 2;
    const int64_t dx = roundPixels(pos.xOffset);
    const int64_t dy = -roundPixels(pos.yOffset);  // font y grows up, the screen's down
    if (glyph > UINT16_MAX || advance < glyph_run::kMinAdvance || advance > glyph_run::kMaxAdvance || dx < INT8_MIN ||
        dx > INT8_MAX || dy < INT8_MIN || dy > INT8_MAX) {
      return false;
    }
    GlyphRecord record;
    record.glyph = static_cast<uint16_t>(glyph);
    record.advance = static_cast<int16_t>(advance);
    record.dx = static_cast<int8_t>(dx);
    record.dy = static_cast<int8_t>(dy);
    if (!sink.addGlyph(record)) return false;
  }
  return true;
}

// A glued neighbour's syllable next to the token, as context: `count` code points, the ones nearest the token when
// the syllable is longer than kContextCodepoints (then `cut`: it starts or ends inside the syllable).
struct Context {
  unsigned count = 0;
  bool cut = false;
};

// Whether the context is of `script`: each code point the script's or shared, and one of them the script's.
bool ofScript(const uint32_t* cps, const unsigned count, const indic::ScriptInfo& script) {
  bool letter = false;
  for (unsigned i = 0; i < count; i++) {
    const indic::ScriptInfo* const of = indic::scriptOf(cps[i]);
    if (of == &script) {
      letter = true;
    } else if (of != nullptr || !indic::isShared(cps[i])) {
      return false;
    }
  }
  return letter;
}

// The last syllable of `text` (glued before the token) into `out` (room for kContextCodepoints), when it is of
// `script`; none otherwise.
Context lastSyllable(const std::string_view text, const indic::ScriptInfo& script, uint32_t* out) {
  Context context;
  uint32_t previous = 0;
  for (size_t pos = 0; pos < text.size();) {
    const bool first = pos == 0;
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    if (!first && indic::syllableBreakAllowed(previous, cp)) context = Context{};
    if (context.count == kContextCodepoints) {
      memmove(out, out + 1, (kContextCodepoints - 1) * sizeof(uint32_t));
      context.count--;
      context.cut = true;
    }
    out[context.count++] = cp;
    previous = cp;
  }
  return ofScript(out, context.count, script) ? context : Context{};
}

// The first syllable of `text` (glued after the token) into `out` (room for kContextCodepoints), when it is of
// `script`; none otherwise.
Context firstSyllable(const std::string_view text, const indic::ScriptInfo& script, uint32_t* out) {
  Context context;
  uint32_t previous = 0;
  for (size_t pos = 0; pos < text.size();) {
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    if (context.count > 0 && indic::syllableBreakAllowed(previous, cp)) break;
    if (context.count == kContextCodepoints) {
      context.cut = true;
      break;
    }
    out[context.count++] = cp;
    previous = cp;
  }
  return ofScript(out, context.count, script) ? context : Context{};
}

enum class Attempt : uint8_t { Shaped, TooLong, Failed };

// One Glyphs run into `sink`: cps[before.count, count - after.count) is the run, with the contexts ahead and behind.
// Shaped whole; when that is past the buffer's storage, upstream's piecewise loop shapes it between syllables. The plan
// and the face are asked again before each ot::shape: a plan is valid until the next plan() on its face, and a face's
// mapping can move. False for a run the font's GSUB deletes every glyph of: a glyph form of no glyphs is no form (a
// line would draw the run's code points at no width), so it is laid out as a run that cannot be shaped.
bool shapeRun(ShapingFace& face, const Request& request, const indic::ScriptInfo& script, const uint32_t* cps,
              const unsigned count, const Context& before, const Context& after, ot::Buffer& buffer, PieceSink& sink) {
  const auto otScript = static_cast<ot::Script>(indic::indexOf(script));
  const auto attempt = [&](const unsigned from, const unsigned length, const ot::RunContext& context) {
    const ot::Plan* const plan = face.plan(otScript, request.languageTags);
    const ot::Face* const shaping = plan != nullptr ? face.current() : nullptr;
    if (shaping == nullptr) return Attempt::Failed;
    ot::Scale scale;
    scale.set(static_cast<int32_t>(request.ppem26_6), (request.ppem26_6 + 32) >> 6, shaping->upem());
    if (ot::shape(*shaping, scale, *plan, cps + from, length, buffer, context)) return Attempt::Shaped;
    return buffer.successful ? Attempt::TooLong : Attempt::Failed;  // successful still set: past the storage
  };

  const Attempt whole = attempt(0, count, ot::RunContext{before.count, after.count, before.cut || after.cut});
  if (whole != Attempt::TooLong) return whole == Attempt::Shaped && buffer.len() != 0 && appendGlyphs(buffer, sink);

  // Upstream's piecewise loop: as many syllables as the buffer holds, or one when it holds fewer. Each piece shapes
  // with the syllable on either side as context, so word-start forms, lookahead rules and kerning across the split
  // match the whole run; when that does not fit, with one code point on either side, before, after, then none. The
  // run's edges are breaks, and a glued neighbour's syllable is the context across them.
  const unsigned runBegin = before.count;
  const unsigned runEnd = count - after.count;
  const auto breakAt = [&](const unsigned i) {
    return i == 0 || i == count || i == runBegin || i == runEnd || indic::syllableBreakAllowed(cps[i - 1], cps[i]);
  };
  const auto prevBreak = [&](unsigned i) {
    do i--;
    while (!breakAt(i));
    return i;
  };
  const auto nextBreak = [&](unsigned i) {
    do i++;
    while (!breakAt(i));
    return i;
  };
  unsigned glyphs = 0;  // the run's, over its pieces
  for (unsigned begin = runBegin; begin < runEnd;) {
    const unsigned limit = ot::maxRunLength(buffer);
    const unsigned from = begin > 0 ? prevBreak(begin) : 0;
    unsigned end = nextBreak(begin);
    while (end < runEnd) {
      const unsigned next = nextBreak(end);
      if ((next < count ? nextBreak(next) : next) - from > limit) break;
      end = next;
    }
    // Context per attempt, before and after: 2 = the syllable, 1 = one code point.
    static constexpr uint8_t CONTEXT[][2] = {{2, 2}, {1, 1}, {1, 0}, {0, 1}, {0, 0}};
    Attempt piece = Attempt::TooLong;
    for (unsigned level = 0; level < sizeof(CONTEXT) / sizeof(CONTEXT[0]) && piece == Attempt::TooLong; level++) {
      const uint8_t wantBefore = begin == 0 ? 0 : CONTEXT[level][0];
      const uint8_t wantAfter = end == count ? 0 : CONTEXT[level][1];
      const unsigned contextBefore = wantBefore == 2 ? begin - from : wantBefore;
      const unsigned contextAfter = wantAfter == 2 ? nextBreak(end) - end : wantAfter;
      // A context reaching into a cut neighbour's syllable is partial as well.
      const bool partial = wantBefore == 1 || wantAfter == 1 || (before.cut && begin - contextBefore < runBegin) ||
                           (after.cut && end + contextAfter > runEnd);
      piece = attempt(begin - contextBefore, contextBefore + (end - begin) + contextAfter,
                      ot::RunContext{contextBefore, contextAfter, partial});
    }
    if (piece != Attempt::Shaped || !appendGlyphs(buffer, sink)) return false;
    glyphs += buffer.len();
    begin = end;
  }
  return glyphs != 0;
}

// What a Glyphs run is shaped in: the call's code points (room for `room`) and its Buffer.
struct RunMemory {
  uint32_t* codepoints;
  size_t room;
  ot::Buffer& buffer;
};

// `run` of the token as a Glyphs piece's records: its code points, with the glued neighbours' syllables as context
// when it is the token's first run from byte 0 (`first`) or its last to the end (`last`), shaped into `sink`.
bool shapeGlyphs(const std::string_view text, const Run& run, const bool first, const bool last, const Request& request,
                 const RunMemory& memory, PieceSink& sink) {
  uint32_t* const cps = memory.codepoints;
  Context before;
  if (first && run.begin == 0 && !request.before.empty()) before = lastSyllable(request.before, *run.script, cps);
  unsigned count = before.count;
  for (size_t pos = run.begin; pos < run.end;) {
    if (count + kContextCodepoints >= memory.room) return false;
    cps[count++] = TextShaping::decodeAt(text, pos);
  }
  Context after;
  if (last && run.end == text.size() && !request.after.empty()) {
    after = firstSyllable(request.after, *run.script, cps + count);
  }
  return shapeRun(*request.face, request, *run.script, cps, count + after.count, before, after, memory.buffer, sink);
}

}  // namespace

size_t scanRuns(std::string_view text, Run* out) {
  if (text.size() > UINT16_MAX) return 0;
  const size_t room = maxRuns(text.size());
  size_t count = 0;
  size_t pos = 0;
  while (pos < text.size() && count < room) {
    size_t next = pos;
    const uint32_t first = TextShaping::decodeAt(text, next);
    if (indic::scriptOf(first) == nullptr && !indic::isShared(first)) {
      pos = next;
      continue;
    }
    const indic::ScriptInfo* script = nullptr;
    const size_t begin = pos;
    while (pos < text.size()) {
      next = pos;
      const uint32_t cp = TextShaping::decodeAt(text, next);
      const indic::ScriptInfo* cpScript = indic::scriptOf(cp);
      if (cpScript == nullptr && !indic::isShared(cp)) break;
      if (cpScript != nullptr) {
        if (script != nullptr && cpScript != script) break;
        script = cpScript;
      }
      pos = next;
    }
    out[count++] = Run{static_cast<uint16_t>(begin), static_cast<uint16_t>(pos), script};
  }
  return count;
}

bool holdsIndic(std::string_view text) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  for (size_t i = 0; i + 3 <= text.size(); i++) {
    if (indic::startsIndic(bytes + i)) return true;
  }
  return false;
}

CallMemory::CallMemory(BuildArena* const arena, const size_t tokenBytes, const CallKind kind,
                       const unsigned runCodepoints) {
  // A piece's bytes are uint16_t, and ot::Buffer numbers at most 65536 slots.
  if (tokenBytes > UINT16_MAX || runCodepoints > (UINT16_MAX - kGrowthExtra) / kGrowthFactor) return;
  // A call without a face writes IndicReorder's form and nothing else, so it takes the form's room alone: a book in
  // such a font holds a heap block of the longest token's form (in 512 B steps), not the 6.5 KB a shaping call takes
  // on the C3.
  const bool shapes = kind == CallKind::Shape;
  const Sizes sizes{shapes, shapes ? ot::Buffer::bytesFor(kGrowthFactor * runCodepoints + kGrowthExtra) : 0,
                    shapes ? maxRuns(tokenBytes) : 0,
                    shapes ? tokenBytes / kRunCodepointBytes + 2 * kContextCodepoints : 0, 3 * tokenBytes + 1};
  Carver measure;
  layOut(measure, sizes);

  uint8_t* block = nullptr;
  // Room checked first: a refused allocation would read in the arena's own log as its owner running out, where the
  // heap block is the plan.
  if (arena != nullptr && arena->valid() && arena->capacity() - arena->used() >= measure.bytes()) {
    block_ = arena->reserveBlock();
    block = static_cast<uint8_t*>(arena->alloc(measure.bytes(), 1));
    if (block != nullptr) {
      arena_ = arena;
    } else {
      arena->release(block_);
    }
  }
  if (block == nullptr) {
    block = static_cast<uint8_t*>(TextShaping::buildHeapBlock(measure.bytes()));
    heapBlock_ = block != nullptr;
  }
  if (block == nullptr) return;

  Carver carve(block);
  const Parts parts = layOut(carve, sizes);
  if (shapes) {
    buffer_ = new (parts.buffer) ot::Buffer;  // placed in the block; destroyed by ~CallMemory
    buffer_->setStorage(parts.storage, sizes.storage);
  }
  tokenBytes_ = tokenBytes;
  runs_ = parts.runs;
  codepoints_ = parts.codepoints;
  codepointRoom_ = sizes.codepoints;
  reorder_ = parts.reorder;
  reorderRoom_ = sizes.reorder;
}

CallMemory::~CallMemory() {
  if (buffer_ != nullptr) buffer_->~Buffer();
  if (heapBlock_) TextShaping::returnBuildHeapBlock();
  if (arena_ != nullptr && !arena_->release(block_)) {
    LOG_ERR("SHP", "Shaping call block not the arena's newest; it stays held");
  }
}

bool CallMemory::valid() const { return reorder_ != nullptr; }

Outcome shapeToken(const std::string_view text, const Request& request, CallMemory& memory, PieceSink& sink) {
  if (!holdsIndic(text)) return Outcome::Plain;
  if (!memory.valid() || text.size() > memory.tokenBytes_) return Outcome::Degraded;
  const auto size = static_cast<uint16_t>(text.size());  // tokenBytes_ is at most UINT16_MAX
  char* const form = memory.reorderBuffer();
  size_t formBytes = 0;

  if (request.face == nullptr) {
    if (!indicReorderForDisplay(text.data(), text.size(), form, memory.reorderRoom(), &formBytes)) {
      return Outcome::Plain;
    }
    return sink.beginPiece(PieceKind::Reordered, 0, size) && sink.addBytes(form, formBytes) ? Outcome::Formed
                                                                                            : Outcome::Degraded;
  }

  if (memory.buffer_ == nullptr) return Outcome::Degraded;  // a reordering call's block (CallKind::Reorder)
  Run* const runs = memory.runs_;
  const size_t runCount = scanRuns(text, runs);
  const RunMemory runMemory{memory.codepoints_, memory.codepointRoom_, *memory.buffer_};
  // Text is written only once a form follows it or precedes it: a token whose every piece is Text writes nothing.
  uint16_t textFrom = 0;
  bool formed = false;
  for (size_t r = 0; r < runCount; r++) {
    const Run& run = runs[r];
    if (run.script == nullptr) continue;  // shared code points alone: Text
    const ot::Face* const shaping = request.face->current();
    if (shaping == nullptr) return Outcome::Degraded;
    uint32_t probe = 0;
    const bool mapped = shaping->nominalGlyph(run.script->probe, &probe);  // upstream's coverage
    if (!mapped &&
        !indicReorderForDisplay(text.data() + run.begin, run.end - run.begin, form, memory.reorderRoom(), &formBytes)) {
      continue;  // a script the face does not draw, its form its text: Text
    }
    if (run.begin > textFrom && !sink.beginPiece(PieceKind::Text, textFrom, run.begin)) return Outcome::Degraded;
    textFrom = run.end;
    formed = true;
    const bool written =
        mapped ? sink.beginPiece(PieceKind::Glyphs, run.begin, run.end) &&
                     shapeGlyphs(text, run, r == 0, r + 1 == runCount, request, runMemory, sink)
               : sink.beginPiece(PieceKind::Reordered, run.begin, run.end) && sink.addBytes(form, formBytes);
    if (!written) return Outcome::Degraded;
  }
  if (!formed) return Outcome::Plain;
  if (textFrom < size && !sink.beginPiece(PieceKind::Text, textFrom, size)) return Outcome::Degraded;
  return Outcome::Formed;
}

}  // namespace IndicShaper
