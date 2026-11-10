#include "IndicShaper.h"

#include <Logging.h>
#include <OtShaper.h>
#include <ShapingFace.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "IndicReorder.h"
#include "TextShaping.h"

// Ours (this repo's), with three algorithms ported from crosspoint-reader PR #3787 ("feat: add Indic language support
// without HarfBuzz", SSafayet / @SSafayet), lib/EpdFont/ComplexShaper.cpp at 0fbd1575:
// - scanRun, as scanRuns, over a bounded view and listing every run;
// - the piecewise loop of appendShapedRun, as shapeRun, over a run with its glued neighbours' clusters as context;
// - the rounding of appendGlyphs, as appendGlyphs, into GlyphRecords that refuse what their fields cannot hold.
// Upstream shapes in the renderer, through a run cache; here a token is shaped once at section build, into pieces, in
// the call's own memory (writing modes step 2, deviations 2, 6, 7 and 9). PR 4b shapes Arabic runs through the same
// loop, each run under its own script's policy (ScriptPolicy); upstream's loop is written for left-to-right runs, and
// takes a right-to-left run's pieces from its end. The Myanmar design's PR 2 shapes Myanmar runs through it too,
// between the syllables of myanmar::breakAllowed, with one syllable of a glued neighbour as context, as Indic is.
namespace IndicShaper {

namespace {

// ot::shape's growth limit for a run of n code points: 2n + 16 glyphs (OtShaper.cpp's MAX_LENGTH_FACTOR and
// MAX_LENGTH_EXTRA).
constexpr unsigned kGrowthFactor = 2;
constexpr unsigned kGrowthExtra = 16;
// A run's code points take 2 bytes of UTF-8 at least: an Arabic letter or mark 2; the letters of U+0900-0DFF and of the
// Myanmar blocks, the dandas, the joiners and the dotted circle 3.
constexpr size_t kRunCodepointBytes = 2;
constexpr size_t kJoinerBytes = 3;  // U+200C and U+200D

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

// What a Glyphs run takes from its script: the plan's script and direction, the letter a face that draws the script
// maps (upstream's coverage probe), where a long run may be split (between Indic syllables, between Arabic base
// clusters: a letter with the marks and joiners after it, between Myanmar syllables), and the context, of a piece and
// of a glued neighbour: the syllable for Indic, upstream's, and for Myanmar, where the design measured a syllable on
// either side to shape as the whole phrase does. A piece of an Arabic run takes two clusters on either side, as a
// letter's glyph can follow its neighbour's joining form, which follows the neighbour's own neighbour (Noto's alef
// takes another glyph after a medial lam than after an initial one). A glued Arabic neighbour gives its whole clusters,
// as many as fit in kArabicContextCodepoints: both halves of a word cut by a style span then see the whole word, and a
// ligature across the cut (Allah, four letters) forms alike on both and is drawn once. The context test is the
// script's: ofScript, ofArabic or ofMyanmar.
struct ScriptPolicy {
  RunKind kind;
  ot::Script script;
  bool backward;  // right to left, as the engine's plan for the script is (Plan::backward): the last code point's
                  // glyphs come out first
  uint32_t probe;
  const indic::ScriptInfo* indic;  // the Indic script; null for Arabic and Myanmar
  // Whether a run may be split before cps[i] of cps[0, count), 0 < i < count.
  bool (*clusterBreak)(const uint32_t* cps, unsigned count, unsigned i);
  uint8_t contextClusters;    // on either side of a piece of a long run, and of a token from its glued neighbours
  uint8_t contextCodepoints;  // a glued neighbour's context, at most: kContextCodepoints, kArabicContextCodepoints
};

constexpr uint8_t kArabicContextClusters = 2;
// The code points a call's context takes on either side, at most, whatever the script.
constexpr unsigned kMaxContextCodepoints = kArabicContextCodepoints;
static_assert(kMaxContextCodepoints >= kContextCodepoints && kArabicContextCodepoints <= UINT8_MAX);

bool indicBreak(const uint32_t* cps, unsigned /*count*/, const unsigned i) {
  return indic::syllableBreakAllowed(cps[i - 1], cps[i]);
}

bool arabicBreak(const uint32_t* cps, unsigned /*count*/, const unsigned i) {
  return arabic::isClusterBreak(cps[i - 1], cps[i]);
}

// The break rule reads the code point after cps[i] past any dot below.
bool myanmarBreak(const uint32_t* cps, const unsigned count, const unsigned i) {
  unsigned next = i + 1;
  while (next < count && cps[next] == myanmar::DOT_BELOW) next++;
  return myanmar::breakAllowed(cps[i - 1], cps[i], next < count ? cps[next] : 0);
}

ScriptPolicy policyOf(const Run& run) {
  switch (run.kind) {
    case RunKind::Arabic:
      return {.kind = RunKind::Arabic,
              .script = ot::Script::Arabic,
              .backward = true,
              .probe = arabic::kProbe,
              .indic = nullptr,
              .clusterBreak = arabicBreak,
              .contextClusters = kArabicContextClusters,
              .contextCodepoints = kArabicContextCodepoints};
    case RunKind::Myanmar:
      return {.kind = RunKind::Myanmar,
              .script = ot::Script::Myanmar,
              .backward = false,
              .probe = myanmar::kProbe,
              .indic = nullptr,
              .clusterBreak = myanmarBreak,
              .contextClusters = 1,
              .contextCodepoints = kContextCodepoints};
    case RunKind::Indic:
      break;
  }
  return {.kind = RunKind::Indic,
          .script = static_cast<ot::Script>(indic::indexOf(*run.script)),
          .backward = false,
          .probe = run.script->probe,
          .indic = run.script,
          .clusterBreak = indicBreak,
          .contextClusters = 1,
          .contextCodepoints = kContextCodepoints};
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

// Whether a Myanmar context is Myanmar: each code point a member, a joiner or the dotted circle, and one of them a
// member.
bool ofMyanmar(const uint32_t* cps, const unsigned count) {
  bool member = false;
  for (unsigned i = 0; i < count; i++) {
    if (myanmar::isRunMember(cps[i])) {
      member = true;
    } else if (!myanmar::isShared(cps[i])) {
      return false;
    }
  }
  return member;
}

// Whether a context of `policy` (Indic or Myanmar) is of its script.
bool ofPolicy(const ScriptPolicy& policy, const uint32_t* cps, const unsigned count) {
  return policy.kind == RunKind::Myanmar ? ofMyanmar(cps, count) : ofScript(cps, count, *policy.indic);
}

// The code point of `text` at byte `pos` or after it that is not the dot below, 0 at the text's end: what
// myanmar::breakAllowed reads after the code point before `pos`.
uint32_t nextPastDotBelow(const std::string_view text, size_t pos) {
  while (pos < text.size()) {
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    if (cp != myanmar::DOT_BELOW) return cp;
  }
  return 0;
}

// Whether a syllable of `policy`'s script (Indic or Myanmar) starts at `cp`, after `previous`; `end` is the byte of
// `text` just past `cp`.
bool syllableStarts(const ScriptPolicy& policy, const std::string_view text, const uint32_t previous, const uint32_t cp,
                    const size_t end) {
  if (policy.kind == RunKind::Myanmar) return myanmar::breakAllowed(previous, cp, nextPastDotBelow(text, end));
  return indic::syllableBreakAllowed(previous, cp);
}

// The last syllable of `text` (glued before the token) into `out` (room for kContextCodepoints), when it is of the
// policy's script (Indic or Myanmar); none otherwise.
Context lastSyllable(const std::string_view text, const ScriptPolicy& policy, uint32_t* out) {
  Context context;
  uint32_t previous = 0;
  for (size_t pos = 0; pos < text.size();) {
    const bool first = pos == 0;
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    if (!first && syllableStarts(policy, text, previous, cp, pos)) context = Context{};
    if (context.count == kContextCodepoints) {
      memmove(out, out + 1, (kContextCodepoints - 1) * sizeof(uint32_t));
      context.count--;
      context.cut = true;
    }
    out[context.count++] = cp;
    previous = cp;
  }
  return ofPolicy(policy, out, context.count) ? context : Context{};
}

// The first syllable of `text` (glued after the token) into `out` (room for kContextCodepoints), when it is of the
// policy's script (Indic or Myanmar); none otherwise.
Context firstSyllable(const std::string_view text, const ScriptPolicy& policy, uint32_t* out) {
  Context context;
  uint32_t previous = 0;
  for (size_t pos = 0; pos < text.size();) {
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    if (context.count > 0 && syllableStarts(policy, text, previous, cp, pos)) break;
    if (context.count == kContextCodepoints) {
      context.cut = true;
      break;
    }
    out[context.count++] = cp;
    previous = cp;
  }
  return ofPolicy(policy, out, context.count) ? context : Context{};
}

// Whether an Arabic context cluster is Arabic: each code point a run member or a joiner, and one of them a letter (a
// member that is not a mark, AL punctuation included).
bool ofArabic(const uint32_t* cps, const unsigned count) {
  bool letter = false;
  for (unsigned i = 0; i < count; i++) {
    const uint32_t cp = cps[i];
    if (arabic::isRunMember(cp)) {
      letter = letter || !arabic::isMark(cp);
    } else if (cp != arabic::ZWJ && cp != arabic::ZWNJ) {
      return false;
    }
  }
  return letter;
}

// The glued Arabic neighbour's last whole clusters, a cluster being a letter with the marks and joiners after it: as
// many as fit in `room` code points, nearest first, into `out` (none cut: a cluster that does not fit is left out, with
// all before it). It stops at the first cluster that is not Arabic. A window of the text's last room + 1 code points is
// enough: a cluster that starts at its first code point cannot fit with the window's rest.
Context lastArabicClusters(const std::string_view text, const unsigned room, uint32_t* out) {
  Context context;
  if (room == 0) return context;
  uint32_t window[kArabicContextCodepoints + 1];  // the text's last code points, in order; room < the array's size
  unsigned held = 0;
  size_t start = text.size();
  while (start > 0 && held <= room) {
    do start--;
    while (start > 0 && (static_cast<uint8_t>(text[start]) & 0xC0) == 0x80);
    size_t pos = start;
    window[held++] = TextShaping::decodeAt(text, pos);
  }
  const bool truncated = start > 0;  // code points before the window: its first is of an unknown cluster
  std::reverse(window, window + held);
  unsigned end = held;
  while (end > 0) {
    unsigned begin = end - 1;  // where the cluster before `end` begins
    while (begin > 0 && !arabic::isClusterBreak(window[begin - 1], window[begin])) begin--;
    if (begin == 0 && truncated) break;
    if (context.count + (end - begin) > room || !ofArabic(window + begin, end - begin)) break;
    context.count += end - begin;
    end = begin;
  }
  memmove(out, window + end, context.count * sizeof(uint32_t));
  return context;
}

// The glued Arabic neighbour's first whole clusters: as many as fit in `room` code points, into `out`, as
// lastArabicClusters takes the last. It stops at the first cluster that is not Arabic.
Context firstArabicClusters(const std::string_view text, const unsigned room, uint32_t* out) {
  Context context;
  unsigned kept = 0;  // the whole clusters read: out[0, kept)
  uint32_t previous = 0;
  for (size_t pos = 0; pos < text.size();) {
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    if (context.count > 0 && arabic::isClusterBreak(previous, cp)) {  // out[kept, count) is whole
      if (!ofArabic(out + kept, context.count - kept)) return Context{kept};
      kept = context.count;
    }
    if (context.count == room) return Context{kept};  // the cluster under reading does not fit
    out[context.count++] = cp;
    previous = cp;
  }
  return Context{ofArabic(out + kept, context.count - kept) ? context.count : kept};
}

enum class Attempt : uint8_t { Shaped, TooLong, Failed };

// One Glyphs run into `sink`: cps[before.count, count - after.count) is the run, with the contexts ahead and behind.
// Shaped whole; when that is past the buffer's storage, upstream's piecewise loop shapes it between clusters. The plan
// and the face are asked again before each ot::shape: a plan is valid until the next plan() on its face, and a face's
// mapping can move. A run shaped with context may keep no glyph: a glued neighbour took its glyphs (a lam-alef across
// the glue is the first token's ligature), and it is an empty piece. False for a run without context the font's GSUB
// deletes every glyph of: a glyph form of no glyphs is no form (a line would draw the run's code points at no width),
// so it is laid out as a run that cannot be shaped.
bool shapeRun(ShapingFace& face, const Request& request, const ScriptPolicy& policy, const uint32_t* cps,
              const unsigned count, const Context& before, const Context& after, ot::Buffer& buffer, PieceSink& sink) {
  const auto attempt = [&](const unsigned from, const unsigned length, const ot::RunContext& context) {
    const ot::Plan* const plan = face.plan(policy.script, request.languageTags);
    const ot::Face* const shaping = plan != nullptr ? face.current() : nullptr;
    if (shaping == nullptr) return Attempt::Failed;
    ot::Scale scale;
    scale.set(static_cast<int32_t>(request.ppem26_6), (request.ppem26_6 + 32) >> 6, shaping->upem());
    if (ot::shape(*shaping, scale, *plan, cps + from, length, buffer, context)) return Attempt::Shaped;
    return buffer.successful ? Attempt::TooLong : Attempt::Failed;  // successful still set: past the storage
  };

  const bool withContext = before.count != 0 || after.count != 0;
  const Attempt whole = attempt(0, count, ot::RunContext{before.count, after.count, before.cut || after.cut});
  if (whole != Attempt::TooLong) {
    return whole == Attempt::Shaped && (buffer.len() != 0 || withContext) && appendGlyphs(buffer, sink);
  }

  // Upstream's piecewise loop: as many clusters as the buffer holds, or one when it holds fewer. Each piece shapes
  // with the policy's clusters on either side as context, so word-start forms, joining, lookahead rules and kerning
  // across the split match the whole run; when that does not fit, with one cluster (an Arabic run), then one code
  // point on either side, before, after, then none. The run's edges are breaks, and a glued neighbour's cluster is the
  // context across them. A right-to-left run's glyphs come out last code point first, so its pieces are taken from its
  // end, mirrored, to be written in that order.
  const unsigned runBegin = before.count;
  const unsigned runEnd = count - after.count;
  const auto breakAt = [&](const unsigned i) {
    return i == 0 || i == count || i == runBegin || i == runEnd || policy.clusterBreak(cps, count, i);
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
  // Where `clusters` clusters before `i` begin, and where as many after it end: a piece's context at most.
  const auto contextFrom = [&](unsigned i, const unsigned clusters) {
    for (unsigned c = 0; c < clusters && i > 0; c++) i = prevBreak(i);
    return i;
  };
  const auto contextTo = [&](unsigned i, const unsigned clusters) {
    for (unsigned c = 0; c < clusters && i < count; c++) i = nextBreak(i);
    return i;
  };
  const unsigned reach = policy.contextClusters;
  unsigned glyphs = 0;  // the run's, over its pieces
  for (unsigned at = policy.backward ? runEnd : runBegin; policy.backward ? at > runBegin : at < runEnd;) {
    const unsigned limit = ot::maxRunLength(buffer);
    unsigned begin = at;
    unsigned end = at;
    if (policy.backward) {
      begin = prevBreak(end);
      while (begin > runBegin) {
        const unsigned previous = prevBreak(begin);
        if (contextTo(end, reach) - contextFrom(previous, reach) > limit) break;
        begin = previous;
      }
    } else {
      end = nextBreak(begin);
      while (end < runEnd) {
        const unsigned next = nextBreak(end);
        if (contextTo(next, reach) - contextFrom(begin, reach) > limit) break;
        end = next;
      }
    }
    // Context per attempt, before and after: Reach = the policy's clusters, Cluster = one, Codepoint = one code point.
    // Upstream's levels are the last five, its syllable the Cluster; a policy that reaches one cluster starts there.
    enum Want : uint8_t { None, Codepoint, Cluster, Reach };
    static constexpr Want CONTEXT[][2] = {{Reach, Reach},    {Cluster, Cluster}, {Codepoint, Codepoint},
                                          {Codepoint, None}, {None, Codepoint},  {None, None}};
    const auto lengthOf = [&](const Want want, const bool ahead) -> unsigned {
      if (want < Cluster) return want;  // none or one code point
      const unsigned clusters = want == Reach ? reach : 1;
      return ahead ? begin - contextFrom(begin, clusters) : contextTo(end, clusters) - end;
    };
    Attempt piece = Attempt::TooLong;
    for (unsigned level = reach > 1 ? 0 : 1; level < sizeof(CONTEXT) / sizeof(CONTEXT[0]) && piece == Attempt::TooLong;
         level++) {
      const Want wantBefore = begin == 0 ? None : CONTEXT[level][0];
      const Want wantAfter = end == count ? None : CONTEXT[level][1];
      const unsigned contextBefore = lengthOf(wantBefore, true);
      const unsigned contextAfter = lengthOf(wantAfter, false);
      // A context reaching into a cut neighbour's cluster is partial as well.
      const bool partial = wantBefore == Codepoint || wantAfter == Codepoint ||
                           (before.cut && begin - contextBefore < runBegin) ||
                           (after.cut && end + contextAfter > runEnd);
      piece = attempt(begin - contextBefore, contextBefore + (end - begin) + contextAfter,
                      ot::RunContext{contextBefore, contextAfter, partial});
    }
    if (piece != Attempt::Shaped || !appendGlyphs(buffer, sink)) return false;
    glyphs += buffer.len();
    at = policy.backward ? begin : end;
  }
  return glyphs != 0 || withContext;
}

// What a Glyphs run is shaped in: the call's code points (room for `room`) and its Buffer.
struct RunMemory {
  uint32_t* codepoints;
  size_t room;
  ot::Buffer& buffer;
};

// Where the joiners just before byte `at` of `text` begin, at most `most` of them.
size_t joinersBefore(const std::string_view text, size_t at, const unsigned most) {
  for (unsigned n = 0; n < most && at >= kJoinerBytes; n++) {
    size_t pos = at - kJoinerBytes;
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    if (cp != arabic::ZWJ && cp != arabic::ZWNJ) break;
    at -= kJoinerBytes;
  }
  return at;
}

// `run` of the token as a Glyphs piece's records, shaped into `sink`: its code points, with context, at most
// policy.contextCodepoints on either side. Before it, an Arabic run takes the joiners just before it in the token (a
// run starts at a letter, and a ZWJ there joins it); a run from the token's start, those joiners included, takes the
// glued neighbour's last syllable (Indic, Myanmar) or last clusters (Arabic, as many as fit) before them. After it, a
// run to the token's end takes the neighbour's first syllable or first clusters.
bool shapeGlyphs(const std::string_view text, const Run& run, const ScriptPolicy& policy, const Request& request,
                 const RunMemory& memory, PieceSink& sink) {
  uint32_t* const cps = memory.codepoints;
  const unsigned room = policy.contextCodepoints;  // at most kMaxContextCodepoints: CallMemory's room holds both sides
  const bool arabicRun = run.kind == RunKind::Arabic;
  const size_t lead = arabicRun ? joinersBefore(text, run.begin, room) : run.begin;
  const auto joiners = static_cast<unsigned>((run.begin - lead) / kJoinerBytes);  // at most `room`
  Context before;
  if (lead == 0 && !request.before.empty()) {
    before =
        arabicRun ? lastArabicClusters(request.before, room - joiners, cps) : lastSyllable(request.before, policy, cps);
  }
  for (size_t pos = lead; pos < run.begin;) cps[before.count++] = TextShaping::decodeAt(text, pos);
  unsigned count = before.count;  // at most `room`
  for (size_t pos = run.begin; pos < run.end;) {
    if (count + room >= memory.room) return false;  // `room` stays free for the context after the run
    cps[count++] = TextShaping::decodeAt(text, pos);
  }
  Context after;
  if (run.end == text.size() && !request.after.empty()) {
    after = arabicRun ? firstArabicClusters(request.after, room, cps + count)
                      : firstSyllable(request.after, policy, cps + count);
  }
  return shapeRun(*request.face, request, policy, cps, count + after.count, before, after, memory.buffer, sink);
}

// The Arabic run starting at `begin`: members and, after one, ZWJ and ZWNJ. Returns its end.
size_t arabicRunEnd(const std::string_view text, const size_t begin) {
  size_t pos = begin;
  while (pos < text.size()) {
    size_t next = pos;
    const uint32_t cp = TextShaping::decodeAt(text, next);
    if (!arabic::isRunMember(cp) && cp != arabic::ZWJ && cp != arabic::ZWNJ) break;
    pos = next;
  }
  return pos;
}

// Where the Myanmar run that starts at byte `begin` ends: members, with the joiners and dotted circles among them and
// around them. `begin` when no member follows the joiners and dotted circles there: no Myanmar run starts at it.
size_t myanmarRunEnd(const std::string_view text, const size_t begin) {
  size_t pos = begin;
  bool member = false;
  while (pos < text.size()) {
    size_t next = pos;
    const uint32_t cp = TextShaping::decodeAt(text, next);
    if (myanmar::isRunMember(cp)) {
      member = true;
    } else if (!myanmar::isShared(cp)) {
      break;
    }
    pos = next;
  }
  return member ? pos : begin;
}

// The first Arabic run of `text` at or after byte `from`, as scanRuns finds it: [begin, end), both text.size() when
// there is none. An Indic run never holds the letter that starts one.
void nextArabicRun(const std::string_view text, size_t from, size_t* begin, size_t* end) {
  while (from < text.size()) {
    size_t next = from;
    const uint32_t cp = TextShaping::decodeAt(text, next);
    if (arabic::isRunMember(cp) && !arabic::isMark(cp)) {
      *begin = from;
      *end = arabicRunEnd(text, from);
      return;
    }
    from = next;
  }
  *begin = *end = text.size();
}

// A token without a face (a bitmap or built-in font): IndicReorder's form of the text between its Arabic runs, a
// Reordered piece where the form differs from the text. The Arabic runs are Text, unjoined as step 1 draws them
// (decision 5), and never reordered. A token without an Arabic run is one Reordered piece, as upstream reorders it.
Outcome reorderWithoutAFace(const std::string_view text, const CallMemory& memory, PieceSink& sink) {
  const auto size = static_cast<uint16_t>(text.size());  // at most the call's tokenBytes, so at most UINT16_MAX
  uint16_t textFrom = 0;                                 // Text is written only once a form follows it or precedes it
  bool formed = false;
  for (size_t from = 0; from < text.size();) {
    size_t runBegin = 0;
    size_t runEnd = 0;
    nextArabicRun(text, from, &runBegin, &runEnd);
    size_t formBytes = 0;
    if (runBegin > from && indicReorderForDisplay(text.data() + from, runBegin - from, memory.reorderBuffer(),
                                                  memory.reorderRoom(), &formBytes)) {
      if (from > textFrom && !sink.beginPiece(PieceKind::Text, textFrom, static_cast<uint16_t>(from))) {
        return Outcome::Degraded;
      }
      if (!sink.beginPiece(PieceKind::Reordered, static_cast<uint16_t>(from), static_cast<uint16_t>(runBegin)) ||
          !sink.addBytes(memory.reorderBuffer(), formBytes)) {
        return Outcome::Degraded;
      }
      textFrom = static_cast<uint16_t>(runBegin);
      formed = true;
    }
    from = runEnd;
  }
  if (!formed) return Outcome::Plain;
  if (textFrom < size && !sink.beginPiece(PieceKind::Text, textFrom, size)) return Outcome::Degraded;
  return Outcome::Formed;
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
    if (const size_t end = myanmarRunEnd(text, pos); end > pos) {
      out[count++] = Run{static_cast<uint16_t>(pos), static_cast<uint16_t>(end), nullptr, RunKind::Myanmar};
      pos = end;
      continue;
    }
    if (arabic::isRunMember(first) && !arabic::isMark(first)) {
      const size_t end = arabicRunEnd(text, pos);
      out[count++] = Run{static_cast<uint16_t>(pos), static_cast<uint16_t>(end), nullptr, RunKind::Arabic};
      pos = end;
      continue;
    }
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

bool holdsShaped(std::string_view text) {
  return holdsIndic(text) || arabic::holdsArabic(text.data(), text.size()) ||
         myanmar::holdsMyanmar(text.data(), text.size());
}

namespace {

// A piece's bytes are uint16_t, and ot::Buffer numbers at most 65536 slots.
bool callFits(const size_t tokenBytes, const unsigned runCodepoints) {
  return tokenBytes <= UINT16_MAX && runCodepoints <= (UINT16_MAX - kGrowthExtra) / kGrowthFactor;
}

// A call without a face writes IndicReorder's form and nothing else, so it takes the form's room alone: a book in such
// a font holds a heap block of the longest token's form (in 512 B steps), not the 6.5 KB a shaping call takes on the
// C3. The code points: a run's (at most tokenBytes / kRunCodepointBytes) and the context on either side (an Arabic
// run's joiners before it count in its context).
Sizes callSizes(const size_t tokenBytes, const CallKind kind, const unsigned runCodepoints) {
  const bool shapes = kind == CallKind::Shape;
  return Sizes{shapes, shapes ? ot::Buffer::bytesFor(kGrowthFactor * runCodepoints + kGrowthExtra) : 0,
               shapes ? maxRuns(tokenBytes) : 0,
               shapes ? tokenBytes / kRunCodepointBytes + 2 * kMaxContextCodepoints : 0, 3 * tokenBytes + 1};
}

}  // namespace

size_t CallMemory::bytesFor(const size_t tokenBytes, const CallKind kind, const unsigned runCodepoints) {
  if (!callFits(tokenBytes, runCodepoints)) return 0;
  Carver measure;
  layOut(measure, callSizes(tokenBytes, kind, runCodepoints));
  return measure.bytes();
}

CallMemory::CallMemory(BuildArena* const arena, const size_t tokenBytes, const CallKind kind,
                       const unsigned runCodepoints) {
  if (!callFits(tokenBytes, runCodepoints)) return;
  const bool shapes = kind == CallKind::Shape;
  const Sizes sizes = callSizes(tokenBytes, kind, runCodepoints);
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
  if (!holdsShaped(text)) return Outcome::Plain;
  // Without a face, Arabic is drawn unjoined (decision 5) and Myanmar as its code points, so only Indic text needs a
  // form.
  if (request.face == nullptr && !holdsIndic(text)) return Outcome::Plain;
  if (!memory.valid() || text.size() > memory.tokenBytes_) return Outcome::Degraded;
  const auto size = static_cast<uint16_t>(text.size());  // tokenBytes_ is at most UINT16_MAX
  char* const form = memory.reorderBuffer();
  size_t formBytes = 0;

  if (request.face == nullptr) return reorderWithoutAFace(text, memory, sink);

  if (memory.buffer_ == nullptr) return Outcome::Degraded;  // a reordering call's block (CallKind::Reorder)
  Run* const runs = memory.runs_;
  const size_t runCount = scanRuns(text, runs);
  const RunMemory runMemory{memory.codepoints_, memory.codepointRoom_, *memory.buffer_};
  // Text is written only once a form follows it or precedes it: a token whose every piece is Text writes nothing.
  uint16_t textFrom = 0;
  bool formed = false;
  for (size_t r = 0; r < runCount; r++) {
    const Run& run = runs[r];
    if (run.kind == RunKind::Indic && run.script == nullptr) continue;  // shared code points alone: Text
    const ScriptPolicy policy = policyOf(run);
    const ot::Face* const shaping = request.face->current();
    if (shaping == nullptr) return Outcome::Degraded;
    uint32_t probe = 0;
    const bool mapped = shaping->nominalGlyph(policy.probe, &probe);  // upstream's coverage
    // A script the face does not draw: an Arabic run is Text, unjoined as step 1 draws it (decision 5), and so is a
    // Myanmar run, as its code points; an Indic run is Reordered where IndicReorder changes it, else Text.
    if (!mapped && (run.kind != RunKind::Indic || !indicReorderForDisplay(text.data() + run.begin, run.end - run.begin,
                                                                          form, memory.reorderRoom(), &formBytes))) {
      continue;
    }
    if (run.begin > textFrom && !sink.beginPiece(PieceKind::Text, textFrom, run.begin)) return Outcome::Degraded;
    textFrom = run.end;
    formed = true;
    const bool written =
        mapped ? sink.beginPiece(PieceKind::Glyphs, run.begin, run.end) &&
                     shapeGlyphs(text, run, policy, request, runMemory, sink)
               : sink.beginPiece(PieceKind::Reordered, run.begin, run.end) && sink.addBytes(form, formBytes);
    if (!written) return Outcome::Degraded;
  }
  if (!formed) return Outcome::Plain;
  if (textFrom < size && !sink.beginPiece(PieceKind::Text, textFrom, size)) return Outcome::Degraded;
  return Outcome::Formed;
}

bool draws(ShapingFace& face, const uint32_t probe) {
  const ot::Face* const shaping = face.current();
  uint32_t glyph = 0;
  return shaping == nullptr || shaping->nominalGlyph(probe, &glyph);
}

}  // namespace IndicShaper
