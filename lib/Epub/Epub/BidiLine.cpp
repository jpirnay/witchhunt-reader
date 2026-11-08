#include "BidiLine.h"

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace BidiLine {
namespace {

// The build's heap Scratch (heapScratch): builds and the dictionary pages run under RenderLock, one line pass at a
// time, so it needs no lock.
bool heapScratchRefusalLogged = false;  // logged once until releaseBuildMemory()
std::unique_ptr<Scratch> heapScratchStore;

constexpr uint32_t LRE = 0x202A, RLE = 0x202B, PDF = 0x202C, LRO = 0x202D, RLO = 0x202E;
constexpr uint32_t LRI = 0x2066, RLI = 0x2067, FSI = 0x2068, PDI = 0x2069;
constexpr uint32_t SOFT_HYPHEN = 0x00AD, ZWNJ = 0x200C, ZWJ = 0x200D;
constexpr uint32_t SEPARATOR = 0x0020, OBJECT = 0xFFFC;  // U+FFFC stands for a child span in its parent's pass

constexpr uint16_t kNoToken = 0xFFFF;    // an open control's slot
constexpr uint16_t kNoCluster = 0xFFFF;  // an open control's or a separator's unit
constexpr uint16_t kChild = 0x8000;      // a pass entry that is a child span

bool isInitiator(const uint32_t cp) { return cp >= LRI && cp <= FSI; }

// What a drawn form holds: no control, no soft hyphen, and no joiner (applyBidiVisual drops ZWJ and ZWNJ).
bool isDrawn(const uint32_t cp) { return !isControl(cp) && cp != SOFT_HYPHEN && cp != ZWNJ && cp != ZWJ; }

bool isJoiner(const uint32_t cp) { return cp == ZWNJ || cp == ZWJ; }

// What takes no slot of its own after a unit of its token: a transparent mark (W1 gives it its base's type and level)
// and a joiner (BN: X9 removes it, and deviation 12 gives it the preceding level). Both stay next to their base, and a
// mark after a joiner then still follows the joiner's base.
bool ridesOnItsBase(const uint32_t cp) { return BidiUtils::isTransparentMark(cp) || isJoiner(cp); }

// Decodes the code point at text[pos], never reading past len, by utf8NextCodepoint's rule (a bad or missing
// continuation byte consumes the bytes read so far; an overlong, surrogate or out-of-range value, or a byte that
// starts nothing, consumes one), as HebrewShaper does, so the units match what the rest of the pipeline decodes.
uint32_t decodeAt(const char* text, const size_t len, size_t& pos) {
  const auto b0 = static_cast<uint8_t>(text[pos]);
  if (b0 < 0x80) {
    pos++;
    return b0;
  }
  const int n = utf8CodepointLen(b0);
  if (n == 1) {
    pos++;
    return REPLACEMENT_GLYPH;
  }
  uint32_t cp = b0 & ((1u << (7 - n)) - 1);
  for (int i = 1; i < n; i++) {
    if (pos + i >= len || (static_cast<uint8_t>(text[pos + i]) & 0xC0) != 0x80) {
      pos += i;
      return REPLACEMENT_GLYPH;
    }
    cp = (cp << 6) | (static_cast<uint8_t>(text[pos + i]) & 0x3F);
  }
  const bool overlong = (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000);
  if (overlong || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    pos++;
    return REPLACEMENT_GLYPH;
  }
  pos += n;
  return cp;
}

// BidiUtils::detectParagraphLevel over a view: the first R/AL (1) or L (0) among the first 64 code points, stopping
// at NUL or U+FFFD, else the fallback.
int firstStrongLevel(const std::string_view text, const int fallbackLevel) {
  size_t pos = 0;
  for (int checked = 0; pos < text.size() && checked < 64; checked++) {
    const uint32_t cp = decodeAt(text.data(), text.size(), pos);
    if (cp == 0 || cp == REPLACEMENT_GLYPH) break;
    const int level = BidiUtils::strongLevel(cp);
    if (level >= 0) return level;
  }
  return fallbackLevel & 1;
}

// --- A token's segments (Token::cuts) -----------------------------------------------------------------------------

constexpr uint16_t kNoSegment = 0xFFFF;  // no segment is numbered so: a token has fewer cuts than bytes

// The segment of `token` that holds byte `at`: how many of its cuts are at or below it.
uint16_t segmentOf(const Token& token, const uint16_t at) {
  if (token.cutCount == 0) return 0;
  return static_cast<uint16_t>(std::upper_bound(token.cuts, token.cuts + token.cutCount, at) - token.cuts);
}

uint16_t segmentBegin(const Token& token, const size_t segment) { return segment == 0 ? 0 : token.cuts[segment - 1]; }

uint16_t segmentEnd(const Token& token, const size_t segment) {
  return segment == token.cutCount ? static_cast<uint16_t>(token.text.size()) : token.cuts[segment];
}

// Where a token's piece starts its bytes when its first drawn byte is `next`, after a piece of the same token whose
// first drawn byte is `previous`: at its segment's start when it opens that segment, else at `next`.
uint16_t pieceStart(const Token& token, const uint16_t previous, const uint16_t next) {
  const uint16_t segment = segmentOf(token, next);
  return segment > segmentOf(token, previous) ? segmentBegin(token, segment) : next;
}

// --- tokenForm --------------------------------------------------------------------------------------------------

// A token's own form in the making: the level its bidi input is ordered at, the first of open's controls that input
// takes, and its TextShaping units (in the Scratch's units) when it is shaped.
struct Form {
  std::string_view text;
  const OpenControls& open;
  size_t prepended;
  int level;
  size_t shapedCount;
  bool shaped;
};

// The level, and the controls prepended: an open isolate is never prepended, it gives the level instead.
Form formOf(const std::string_view token, const OpenControls& open, const uint8_t paragraphLevel, Scratch& s) {
  int isolate = -1;
  for (int i = open.count - 1; i >= 0; i--) {
    if (isInitiator(open.cps[i])) {
      isolate = i;
      break;
    }
  }
  int level = 0;
  if (isolate < 0) {
    level = firstStrongLevel(token, paragraphLevel);
  } else {
    const uint32_t initiator = open.cps[isolate];
    level = initiator == LRI ? 0 : initiator == RLI ? 1 : firstStrongLevel(token, 0);
  }
  size_t shapedCount = 0;
  const bool shaped = TextShaping::shape(token.data(), token.size(), s.units, kMaxUnits, shapedCount);
  return {token, open, static_cast<size_t>(isolate + 1), level, shapedCount, shaped};
}

// The bidi input of the form's bytes [from, to) in s.passCp[0..n): the embeddings and overrides opened inside the
// isolate (or all of them), then the code points there (its shaped units whose clusters are there) without their
// joiners (BN, which X9 removes; leaving them out keeps a mark after a joiner on its base). s.passEntry holds each
// one's byte, kNoCluster for a control. False when they are over kMaxCodepoints.
bool fillPass(const Form& form, const size_t from, const size_t to, Scratch& s, size_t& n) {
  n = 0;
  for (size_t i = form.prepended; i < form.open.count; i++) {
    s.passCp[n] = form.open.cps[i];
    s.passEntry[n++] = kNoCluster;
  }
  const auto add = [&](const uint32_t cp, const size_t at) {
    if (isJoiner(cp)) return true;
    if (n == kMaxCodepoints) return false;
    s.passCp[n] = cp;
    s.passEntry[n++] = static_cast<uint16_t>(at);
    return true;
  };
  if (form.shaped) {
    for (size_t k = 0; k < form.shapedCount; k++) {
      const uint16_t at = s.units[k].cluster;
      if (at >= from && at < to && !add(s.units[k].cp, at)) return false;
    }
    return true;
  }
  for (size_t pos = from; pos < to;) {
    const size_t at = pos;
    if (!add(decodeAt(form.text.data(), form.text.size(), pos), at)) return false;
  }
  return true;
}

// Appends the form of the bytes [from, to): their bidi input through one orderLine, else (past the bidi limit, or
// without a workspace) what applyBidiVisual leaves, the text as it is with the drops still applied.
void appendRangeForm(const Form& form, const size_t from, const size_t to, Scratch& s, std::string& dest) {
  size_t n = 0;
  if (fillPass(form, from, to, s, n) && n > 0 &&
      BidiUtils::orderLine(s.passCp, n, form.level, s.passVisual, s.passDrawn)) {
    for (size_t v = 0; v < n; v++) {
      if (isDrawn(s.passDrawn[v])) utf8AppendCodepoint(dest, s.passDrawn[v]);
    }
  } else if (form.shaped) {
    for (size_t k = 0; k < form.shapedCount; k++) {
      const uint16_t at = s.units[k].cluster;
      if (at >= from && at < to && isDrawn(s.units[k].cp)) utf8AppendCodepoint(dest, s.units[k].cp);
    }
  } else {
    for (size_t pos = from; pos < to;) {
      const size_t at = pos;
      if (isDrawn(decodeAt(form.text.data(), form.text.size(), pos))) dest.append(form.text.data() + at, pos - at);
    }
  }
}

// Appends the token's form to `dest`. When it is the token's text byte for byte, takes it back off and returns false.
bool appendForm(const std::string_view token, const OpenControls& open, const uint8_t paragraphLevel, Scratch& s,
                std::string& dest) {
  const size_t start = dest.size();
  appendRangeForm(formOf(token, open, paragraphLevel, s), 0, token.size(), s, dest);
  if (std::string_view(dest).substr(start) == token) {
    dest.resize(start);
    return false;
  }
  return true;
}

// --- layoutLine: the logical sequence -----------------------------------------------------------------------------

// Fills units and slots: the open controls, then each token's units with a separator before each spaced token after
// the first. A transparent mark or a joiner that follows a unit of its own token (not a control) is folded onto that
// unit's slot when both are in one segment of the token. Across a cut it takes a slot of its own, which the order
// resolves as it does any NSM (W1: its base's type) or BN.
// False when the line is over kMaxCodepoints slots or kMaxUnits units.
bool collect(const Token* tokens, const size_t count, const OpenControls& open, Scratch& s, size_t& slotCount) {
  size_t units = 0;
  size_t slots = 0;
  const auto addSlot = [&](const size_t unit, const uint16_t token) {
    if (slots == kMaxCodepoints) return false;
    s.slotUnit[slots] = static_cast<uint16_t>(unit);
    s.slotToken[slots] = token;
    slots++;
    return true;
  };
  const auto addUnit = [&](const uint32_t cp, const uint16_t cluster) {
    if (units == kMaxUnits) return false;
    s.units[units++] = {cp, cluster};
    return true;
  };

  for (size_t i = 0; i < open.count; i++) {
    if (!addSlot(units, kNoToken) || !addUnit(open.cps[i], kNoCluster)) return false;
  }
  for (size_t t = 0; t < count; t++) {
    const auto token = static_cast<uint16_t>(t);
    const std::string_view text = tokens[t].text;
    if (t > 0 && tokens[t].spaceBefore) {
      if (!addSlot(units, token) || !addUnit(SEPARATOR, kNoCluster)) return false;
    }
    const size_t first = units;
    size_t shaped = 0;
    if (TextShaping::shape(text.data(), text.size(), s.units + units, kMaxUnits - units, shaped)) {
      units += shaped;
    } else {
      for (size_t pos = 0; pos < text.size();) {
        const size_t at = pos;  // under kMaxUnits * 4 bytes, so it fits a cluster
        if (!addUnit(decodeAt(text.data(), text.size(), pos), static_cast<uint16_t>(at))) return false;
      }
    }
    uint16_t slotSegment = 0;  // the segment of the slot that units fold onto
    for (size_t u = first; u < units; u++) {
      const uint16_t segment = segmentOf(tokens[t], s.units[u].cluster);
      const bool ridesOnBase =
          u > first && ridesOnItsBase(s.units[u].cp) && !isControl(s.units[u - 1].cp) && segment == slotSegment;
      if (ridesOnBase) continue;
      if (!addSlot(u, token)) return false;
      slotSegment = segment;
    }
  }
  s.slotUnit[slots] = static_cast<uint16_t>(units);
  slotCount = slots;
  return true;
}

// --- layoutLine: isolates ordered on their own (decision 8) -------------------------------------------------------

uint32_t baseOf(const Scratch& s, const size_t slot) { return s.units[s.slotUnit[slot]].cp; }

// Orders the direct slots of [from, to), one U+FFFC per child span (the spans from firstChild up to spanCount whose
// initiator lies in the range), at `level` (-1: an FSI's, the first strong base, else 0). Appends the visual order to
// s.expanded from k, each child replaced by its own order. False when BidiUtils has no workspace.
bool orderRange(Scratch& s, const size_t from, const size_t to, size_t child, const size_t spanCount, int level,
                size_t& k) {
  size_t m = 0;
  int firstStrong = -1;
  for (size_t slot = from; slot < to; slot++) {
    if (child < spanCount && s.spans[child].begin == slot) {
      s.passCp[m] = OBJECT;
      s.passEntry[m++] = static_cast<uint16_t>(kChild | child);
      slot = s.spans[child].end;  // skip the child, and every span inside it
      while (child < spanCount && s.spans[child].begin <= slot) child++;
      continue;
    }
    s.passCp[m] = baseOf(s, slot);
    s.passEntry[m++] = static_cast<uint16_t>(slot);
    if (firstStrong < 0) firstStrong = BidiUtils::strongLevel(s.passCp[m - 1]);
  }
  if (m == 0) return true;
  if (level < 0) level = firstStrong > 0 ? 1 : 0;
  if (!BidiUtils::orderLine(s.passCp, m, level, s.passVisual, s.passDrawn)) return false;
  for (size_t v = 0; v < m; v++) {
    const uint16_t entry = s.passEntry[s.passVisual[v]];
    if (entry & kChild) {
      const Scratch::Span& span = s.spans[entry & ~kChild];
      for (size_t slot = span.begin; slot <= span.end; slot++) s.expanded[k++] = s.order[slot];
    } else {
      s.units[s.slotUnit[entry]].cp = s.passDrawn[v];  // the base as drawn, mirrored; read once, by this pass
      s.expanded[k++] = static_cast<uint8_t>(entry);
    }
  }
  return true;
}

// Orders one span, its initiator first and its PDI last, onto its own slots of s.order.
bool orderSpan(Scratch& s, const size_t index, const size_t spanCount) {
  const Scratch::Span& span = s.spans[index];
  const uint32_t initiator = baseOf(s, span.begin);
  const int level = initiator == LRI ? 0 : initiator == RLI ? 1 : -1;
  size_t k = 0;
  s.expanded[k++] = span.begin;
  const size_t contentEnd = span.closed ? span.end : span.end + 1;
  if (!orderRange(s, span.begin + 1, contentEnd, index + 1, spanCount, level, k)) return false;
  if (span.closed) s.expanded[k++] = span.end;
  std::copy(s.expanded, s.expanded + k, s.order + span.begin);
  return true;
}

// Fills s.order with the line's slots in visual order. Spans are ordered innermost first: each as its PDI closes it,
// then those open at the line's end from the innermost out, then the line itself at paragraphLevel.
bool orderSlots(Scratch& s, const size_t slots, const int paragraphLevel) {
  size_t spanCount = 0;
  size_t depth = 0;
  for (size_t slot = 0; slot < slots; slot++) {
    const uint32_t cp = baseOf(s, slot);
    if (isInitiator(cp)) {
      s.spans[spanCount] = {static_cast<uint8_t>(slot), static_cast<uint8_t>(slot), false};
      s.openSpans[depth++] = static_cast<uint8_t>(spanCount++);
    } else if (cp == PDI && depth > 0) {
      const size_t index = s.openSpans[--depth];
      s.spans[index].end = static_cast<uint8_t>(slot);
      s.spans[index].closed = true;
      if (!orderSpan(s, index, spanCount)) return false;
    }
  }
  while (depth > 0) {
    const size_t index = s.openSpans[--depth];
    s.spans[index].end = static_cast<uint8_t>(slots - 1);
    if (!orderSpan(s, index, spanCount)) return false;
  }
  size_t k = 0;
  if (!orderRange(s, 0, slots, 0, spanCount, paragraphLevel, k)) return false;
  std::copy(s.expanded, s.expanded + k, s.order);
  return true;
}

// --- layoutLine: pieces ---------------------------------------------------------------------------------------------

void closeRun(Line& out, bool& runOpen) {
  if (!runOpen) return;
  Piece& piece = out.pieces.back();
  piece.displayLength = static_cast<uint16_t>(out.display.size() - piece.displayBegin);
  out.display.push_back('\0');
  runOpen = false;
}

constexpr uint8_t kNoRank = 0xFF;

// Marks where the walk must cut a run of one token's slots so that each piece holds characters adjacent in the text
// too: each slot gets its rank among the drawn slots in logical order, and each run (as the walk meets them) is cut
// into the longest prefixes whose ranks form an interval. A run like `(שלום)` jumps about and stays whole; an isolate
// or embedding that carries part of a word away leaves the rest in two pieces. A run also ends where its token's
// segment changes, as it does at a token change, and the walk starts a piece there.
void markCuts(const Token* tokens, Scratch& s, const size_t slots, uint8_t* const rankOf, uint8_t* const cutBefore,
              uint16_t* const run) {
  size_t rank = 0;
  for (size_t slot = 0; slot < slots; slot++) {
    cutBefore[slot] = 0;
    rankOf[slot] = kNoRank;
    const size_t u0 = s.slotUnit[slot];
    if (s.slotToken[slot] == kNoToken || s.units[u0].cluster == kNoCluster) continue;  // open controls, separators
    bool drawsAnything = false;
    for (size_t u = u0; u < s.slotUnit[slot + 1] && !drawsAnything; u++) drawsAnything = isDrawn(s.units[u].cp);
    if (drawsAnything) rankOf[slot] = static_cast<uint8_t>(rank++);
  }
  size_t length = 0;
  uint16_t runToken = kNoToken;
  uint16_t runSegment = 0;
  const auto cutRun = [&] {
    for (size_t i = 0; i < length;) {
      size_t end = i;
      uint8_t lo = rankOf[run[i]];
      uint8_t hi = lo;
      for (size_t j = i + 1; j < length; j++) {
        lo = std::min(lo, rankOf[run[j]]);
        hi = std::max(hi, rankOf[run[j]]);
        if (static_cast<size_t>(hi - lo) == j - i) end = j;
      }
      if (i > 0) cutBefore[run[i]] = 1;
      i = end + 1;
    }
    length = 0;
  };
  for (size_t i = 0; i < slots; i++) {
    const size_t slot = s.order[i];
    const uint16_t token = s.slotToken[slot];
    if (token == kNoToken) continue;
    if (s.units[s.slotUnit[slot]].cluster == kNoCluster) {  // a separator ends the run
      cutRun();
      continue;
    }
    if (rankOf[slot] == kNoRank) continue;  // controls and U+00AD join no run
    const uint16_t segment = segmentOf(tokens[token], s.units[s.slotUnit[slot]].cluster);
    if (length > 0 && runToken != token) {
      cutRun();
    } else if (length > 0 && segment != runSegment) {
      cutRun();
      cutBefore[slot] = 1;
    }
    runToken = token;
    runSegment = segment;
    run[length++] = static_cast<uint16_t>(slot);
  }
  cutRun();
}

// Grows a buffer the Line keeps to at least n, geometrically, so lines that grow a little at a time do not reallocate
// on each one.
template <typename Buffer>
void reserveAtLeast(Buffer& buffer, const size_t n) {
  if (buffer.capacity() < n) buffer.reserve(std::max(n, 2 * buffer.capacity()));
}

// The line's segments: one per token, and one more at each cut.
size_t segmentCount(const Token* tokens, const size_t count) {
  size_t segments = count;
  for (size_t t = 0; t < count; t++) segments += tokens[t].cutCount;
  return segments;
}

// The line's text bytes, and one more per segment: about what its forms and their NULs take.
size_t formBytes(const Token* tokens, const size_t count) {
  size_t bytes = 0;
  for (size_t t = 0; t < count; t++) bytes += tokens[t].text.size() + 1 + tokens[t].cutCount;
  return bytes;
}

// Walks the visual order into pieces, then gives each its bytes and its reading index. False only when a shaped
// cluster's units land in two pieces: the caller falls back.
bool buildPieces(const Token* tokens, const size_t count, Scratch& s, const size_t slots, Line& out) {
  // About one piece per segment, and the forms about the tokens' bytes: a line that holds more grows the buffers once.
  reserveAtLeast(out.pieces, segmentCount(tokens, count));
  reserveAtLeast(out.display, formBytes(tokens, count));
  // Free once the slots are ordered.
  uint8_t* const rankOf = s.expanded;
  uint8_t* const cutBefore = s.openSpans;
  markCuts(tokens, s, slots, rankOf, cutBefore, s.passEntry);

  bool runOpen = false;
  bool spacePending = false;
  uint16_t spaceToken = 0;
  for (size_t i = 0; i < slots; i++) {
    const size_t slot = s.order[i];
    const uint16_t token = s.slotToken[slot];
    if (token == kNoToken) continue;  // an open control
    const size_t u0 = s.slotUnit[slot];
    const size_t u1 = s.slotUnit[slot + 1];
    if (s.units[u0].cluster == kNoCluster) {  // a separator: two with no piece between them are one gap
      closeRun(out, runOpen);
      spaceToken = spacePending ? std::max(spaceToken, token) : token;
      spacePending = true;
      continue;
    }
    if (rankOf[slot] == kNoRank) continue;  // controls and U+00AD join no run

    if (!runOpen || out.pieces.back().token != token || cutBefore[slot]) {
      closeRun(out, runOpen);
      Piece piece{};
      piece.token = token;
      piece.begin = UINT16_MAX;  // the first byte, and in `end` the last, until the bytes are given below
      piece.end = 0;
      piece.displayBegin = static_cast<uint16_t>(out.display.size());
      piece.gapBefore = out.pieces.empty() ? Gap::None : spacePending ? Gap::Space : Gap::Glued;
      piece.spaceToken = piece.gapBefore == Gap::Space ? spaceToken : token;
      spacePending = false;
      out.pieces.push_back(piece);
      runOpen = true;
    }
    Piece& piece = out.pieces.back();
    for (size_t u = u0; u < u1; u++) {  // the base (mirrored by its pass), then its marks
      if (isDrawn(s.units[u].cp)) utf8AppendCodepoint(out.display, s.units[u].cp);
    }
    for (size_t u = u0; u < u1; u++) {
      piece.begin = std::min(piece.begin, s.units[u].cluster);
      piece.end = std::max(piece.end, s.units[u].cluster);
    }
  }
  closeRun(out, runOpen);

  // Each token's pieces by first byte: the first begins at 0, each ends where the next begins, the last at the
  // token's end. In a token with cuts a piece that opens a segment begins at the segment's start: the segment's
  // undrawn bytes before its first drawn one stay in it, and a segment that draws nothing joins the piece before it.
  // The same order ranks them for reading. Pieces are at most the slots, and the pass arrays are free.
  uint16_t* const pieceRank = s.passVisual;
  const size_t n = out.pieces.size();
  const auto before = [&](const size_t a, const size_t b) {
    const Piece& pa = out.pieces[a];
    const Piece& pb = out.pieces[b];
    return pa.token != pb.token ? pa.token < pb.token : pa.begin < pb.begin;
  };
  for (size_t r = 0; r < n; r++) {
    size_t j = r;
    for (; j > 0 && before(r, pieceRank[j - 1]); j--) pieceRank[j] = pieceRank[j - 1];
    pieceRank[j] = static_cast<uint16_t>(r);
  }
  // How many pieces draw each segment (each token's, when it has no cuts): in byte order a segment's pieces are
  // consecutive. Read on their first drawn bytes, before those become segment starts below.
  for (size_t r = 0; r < n;) {
    const Piece& head = out.pieces[pieceRank[r]];
    const Token& token = tokens[head.token];
    const uint16_t segment = segmentOf(token, head.begin);
    size_t end = r + 1;
    while (end < n && out.pieces[pieceRank[end]].token == head.token &&
           segmentOf(token, out.pieces[pieceRank[end]].begin) == segment) {
      end++;
    }
    const auto pieces = static_cast<uint8_t>(std::min<size_t>(end - r, UINT8_MAX));
    for (; r < end; r++) out.pieces[pieceRank[r]].segmentPieces = pieces;
  }
  uint16_t previousEnd = 0;
  for (size_t r = 0; r < n; r++) {
    Piece& piece = out.pieces[pieceRank[r]];
    const Token& token = tokens[piece.token];
    const bool first = r == 0 || out.pieces[pieceRank[r - 1]].token != piece.token;
    const bool last = r + 1 == n || out.pieces[pieceRank[r + 1]].token != piece.token;
    const uint16_t next = last ? static_cast<uint16_t>(token.text.size())
                               : pieceStart(token, piece.begin, out.pieces[pieceRank[r + 1]].begin);
    if (piece.end >= next) return false;  // a shaped cluster shared with the next piece
    piece.begin = first ? 0 : previousEnd;
    piece.end = next;
    previousEnd = next;
    piece.readingIndex = static_cast<uint8_t>(r);
    if (pieceRank[r] != r) out.reordered = true;
  }
  return true;
}

// --- layoutLine: the fallback (upstream #2541's result) -------------------------------------------------------------

// Appends the pieces of a token with cuts, in the token's own visual order. With a form of its own (one orderLine),
// a piece for each run of one segment's characters in that form's order: a segment that another one's characters
// interrupt gives a piece on each side, as the line pass splits it, and its pieces count them (segmentPieces). Past
// kMaxCodepoints, one piece for each segment that draws something, in logical order at level 0 and reversed at level 1,
// each formed on its own. A segment's first piece in the text begins at the segment's start (the token's first piece
// at 0), another at its first drawn byte, and each ends where the next begins, so a segment that draws nothing joins
// the piece before it; they are ranked for reading by them, after the pieces already in `out`.
void appendSegments(const Token& token, const uint16_t index, const OpenControls& open, const uint8_t paragraphLevel,
                    Scratch& s, Line& out) {
  const Form form = formOf(token.text, open, paragraphLevel, s);
  const size_t first = out.pieces.size();
  const auto openPiece = [&](const uint16_t begin) {
    Piece piece{};
    piece.token = index;
    piece.segmentPieces = 1;
    piece.begin = begin;
    piece.displayBegin = static_cast<uint16_t>(out.display.size());
    out.pieces.push_back(piece);
  };
  const auto closePiece = [&] {
    Piece& piece = out.pieces.back();
    piece.displayLength = static_cast<uint16_t>(out.display.size() - piece.displayBegin);
    if (piece.displayLength == 0) {
      out.pieces.pop_back();
    } else {
      out.display.push_back('\0');
    }
  };

  size_t n = 0;
  if (fillPass(form, 0, token.text.size(), s, n) && n > 0 &&
      BidiUtils::orderLine(s.passCp, n, form.level, s.passVisual, s.passDrawn)) {
    // A piece opens where the drawn characters' segment changes; one drawn nothing (a control) interrupts nothing.
    // Until its bytes are given below, a piece's `begin` is its first drawn byte.
    uint16_t segment = kNoSegment;
    for (size_t v = 0; v < n; v++) {
      if (!isDrawn(s.passDrawn[v])) continue;
      const uint16_t at = s.passEntry[s.passVisual[v]];
      const uint16_t own = segmentOf(token, at);
      if (own != segment) {
        if (segment != kNoSegment) closePiece();
        openPiece(at);
        segment = own;
      }
      Piece& piece = out.pieces.back();
      piece.begin = std::min(piece.begin, at);
      utf8AppendCodepoint(out.display, s.passDrawn[v]);
    }
    if (segment != kNoSegment) closePiece();
    // At most kMaxCodepoints pieces. Each counts its segment's pieces, and the first of them in the text begins at the
    // segment's start: that keeps the pieces' order by first byte, and their segments, for the ranks below.
    const size_t last = out.pieces.size();
    for (size_t i = first; i < last; i++) {
      Piece& piece = out.pieces[i];
      const uint16_t own = segmentOf(token, piece.begin);
      size_t pieces = 0;
      bool opens = true;
      for (size_t j = first; j < last; j++) {
        if (segmentOf(token, out.pieces[j].begin) != own) continue;
        pieces++;
        opens = opens && out.pieces[j].begin >= piece.begin;
      }
      piece.segmentPieces = static_cast<uint8_t>(std::min<size_t>(pieces, UINT8_MAX));
      if (opens) piece.begin = segmentBegin(token, own);
    }
    // Each ranked against the others by its start.
    size_t lowest = first;
    for (size_t i = first; i < last; i++) {
      Piece& piece = out.pieces[i];
      size_t rank = 0;
      piece.end = static_cast<uint16_t>(token.text.size());
      for (size_t j = first; j < last; j++) {
        const uint16_t other = out.pieces[j].begin;
        if (other < piece.begin) rank++;
        if (other > piece.begin) piece.end = std::min(piece.end, other);
      }
      piece.readingIndex = static_cast<uint8_t>(first + rank);
      if (rank == 0) lowest = i;
    }
    if (last > first) out.pieces[lowest].begin = 0;
    return;
  }

  for (size_t segment = 0; segment <= token.cutCount; segment++) {
    openPiece(segmentBegin(token, segment));
    appendRangeForm(form, segmentBegin(token, segment), segmentEnd(token, segment), s, out.display);
    closePiece();
  }
  const size_t last = out.pieces.size();
  for (size_t i = first; i < last; i++) {
    Piece& piece = out.pieces[i];
    piece.end = i + 1 < last ? out.pieces[i + 1].begin : static_cast<uint16_t>(token.text.size());
    piece.readingIndex = static_cast<uint8_t>(i);
  }
  if (last > first) out.pieces[first].begin = 0;
  if (form.level == 1) std::reverse(out.pieces.begin() + static_cast<std::ptrdiff_t>(first), out.pieces.end());
}

size_t encodedBytes(const uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

// The most pieces, and bytes of forms with their NULs, the fallback makes of the tokens, so it reserves them whole.
// - Bytes: a form draws a token's code points (each as long as its mirrored pair; TextShaping only shortens) or
//   copies its bytes, so a code point takes the longer of its bytes and its encoding (U+FFFD for a bad byte is 3).
// - A token without cuts is one piece, and its form's NUL.
// - A token with cuts gives a piece for each run of one segment's characters (appendSegments). Each draws one drawn
//   code point at least, and no two the same (TextShaping gives no more drawn units than the drawn code points), so
//   it gives at most its drawn code points; its own form holds at most kMaxCodepoints of them, and past it each
//   segment is one piece. The logical branch opens a piece for each segment before it knows whether it draws (and
//   closes an empty one), so a token never takes fewer than its segments, cuts + 1. A NUL a piece.
void fallbackBounds(const Token* tokens, const size_t count, size_t& pieces, size_t& bytes) {
  pieces = 0;
  bytes = 0;
  for (size_t t = 0; t < count; t++) {
    const std::string_view text = tokens[t].text;
    size_t drawn = 0;
    for (size_t pos = 0; pos < text.size();) {
      const size_t at = pos;
      const uint32_t cp = decodeAt(text.data(), text.size(), pos);
      bytes += std::max(pos - at, encodedBytes(cp));
      if (isDrawn(cp)) drawn++;
    }
    const size_t cuts = tokens[t].cutCount;
    const size_t own = cuts == 0 ? 1 : std::max(std::min(drawn, kMaxCodepoints), cuts + 1);
    pieces += own;
    bytes += own;
  }
}

// The tokens in computeVisualWordOrder's order over the tokens without their transparent marks; when it declines,
// reversed at level 1 and as is at level 0. A token without cuts is one piece with its whole form, one with cuts its
// pieces in its own form's order (appendSegments), glued to one another; a token with no drawn character gives none.
// Its corners are upstream's: past 128 code points computeVisualWordOrder declines too, so a right-to-left line
// reverses an embedded Latin phrase's words, and minibidi sees isolates as embeddings. False (out cleared) when
// BidiUtils has no workspace, as for a line that fits.
bool fallbackLine(const Token* tokens, const size_t count, const OpenControls& open, const uint8_t paragraphLevel,
                  Scratch& s, Line& out) {
  out.clear();
  // computeVisualWordOrder and the forms need the workspace too, and say nothing when it is missing: ask orderLine,
  // which refuses one code point only for that.
  const uint32_t probe = SEPARATOR;
  uint16_t probeVisual = 0;
  uint32_t probeDrawn = 0;
  if (!BidiUtils::orderLine(&probe, 1, 0, &probeVisual, &probeDrawn)) return false;
  out.fallback = true;
  if (count == 0) return true;

  // The words without their marks (computeVisualWordOrder takes std::string words, about the line's bytes) and the
  // order followed by each token's first piece (4 bytes a token), in the Line's buffers: only a fallback line longer
  // than any before allocates.
  std::vector<std::string>& words = out.fallbackWords;
  std::vector<uint16_t>& order = out.fallbackOrder;
  reserveAtLeast(words, count);
  words.resize(count);
  for (size_t t = 0; t < count; t++) {
    const std::string_view text = tokens[t].text;
    std::string& stripped = words[t];
    stripped.clear();
    stripped.reserve(text.size());
    for (size_t pos = 0; pos < text.size();) {
      const size_t at = pos;
      if (!BidiUtils::isTransparentMark(decodeAt(text.data(), text.size(), pos))) {
        stripped.append(text.data() + at, pos - at);
      }
    }
  }
  reserveAtLeast(order, 2 * count + 1);
  if (!BidiUtils::computeVisualWordOrder(words, paragraphLevel == 1, order)) order.clear();
  if (order.size() != count) {
    order.clear();
    for (size_t v = 0; v < count; v++) order.push_back(static_cast<uint16_t>(paragraphLevel == 1 ? count - 1 - v : v));
  }
  order.resize(2 * count + 1);
  uint16_t* const firstPiece = order.data() + count;  // token t's pieces are [firstPiece[t], firstPiece[t + 1])

  // The pieces in logical order, made as the open controls follow the text, each ranked for reading as it comes.
  // Fewer than 65,536 of them: each holds a drawn form in `display`, which 16-bit offsets index. Their buffers are
  // reserved to what they can hold, so that neither grows while they are made.
  size_t pieceBound = 0;
  size_t byteBound = 0;
  fallbackBounds(tokens, count, pieceBound, byteBound);
  reserveAtLeast(out.display, byteBound);
  reserveAtLeast(out.pieces, pieceBound);
  OpenControls at = open;
  for (size_t t = 0; t < count; t++) {
    const std::string_view text = tokens[t].text;
    firstPiece[t] = static_cast<uint16_t>(out.pieces.size());
    if (tokens[t].cutCount > 0) {
      appendSegments(tokens[t], static_cast<uint16_t>(t), at, paragraphLevel, s, out);
    } else {
      Piece piece{};
      piece.token = static_cast<uint16_t>(t);
      piece.segmentPieces = 1;
      piece.begin = 0;
      piece.end = static_cast<uint16_t>(text.size());
      piece.displayBegin = static_cast<uint16_t>(out.display.size());
      if (!appendForm(text, at, paragraphLevel, s, out.display)) out.display.append(text);
      piece.displayLength = static_cast<uint16_t>(out.display.size() - piece.displayBegin);
      out.display.push_back('\0');
      piece.readingIndex = static_cast<uint8_t>(out.pieces.size());
      if (piece.displayLength > 0) out.pieces.push_back(piece);  // a token with no drawn character gives no piece
    }
    at.advance(text);
  }
  firstPiece[count] = static_cast<uint16_t>(out.pieces.size());

  // Into visual order a token's pieces at a time: each piece's place in spaceToken (given its own value below), then
  // each piece swapped straight into its place. The places are a permutation, as the order is.
  uint16_t place = 0;
  for (size_t v = 0; v < count; v++) {
    const uint16_t t = order[v];
    for (size_t i = firstPiece[t]; i < firstPiece[t + 1]; i++) out.pieces[i].spaceToken = place++;
  }
  for (size_t i = 0; i < out.pieces.size(); i++) {
    while (out.pieces[i].spaceToken != i) std::swap(out.pieces[i], out.pieces[out.pieces[i].spaceToken]);
  }

  // One token's pieces are glued to one another. Visual neighbours that are logical neighbours keep the later one's
  // glue; any other pair is a word space, the right-hand piece's.
  for (size_t v = 0; v < out.pieces.size(); v++) {
    Piece& piece = out.pieces[v];
    if (piece.readingIndex != v) out.reordered = true;
    piece.spaceToken = piece.token;
    if (v == 0) {
      piece.gapBefore = Gap::None;
      continue;
    }
    const uint16_t left = out.pieces[v - 1].token;
    if (left == piece.token) {
      piece.gapBefore = Gap::Glued;
      continue;
    }
    const uint16_t later = std::max(left, piece.token);
    const bool neighbours = later - std::min(left, piece.token) == 1;
    piece.gapBefore = neighbours && !tokens[later].spaceBefore ? Gap::Glued : Gap::Space;
    if (neighbours && piece.gapBefore == Gap::Space) piece.spaceToken = later;
  }
  return true;
}

}  // namespace

bool isControl(const uint32_t cp) {
  return cp == 0x200E || cp == 0x200F || cp == 0x061C || (cp >= LRE && cp <= RLO) || (cp >= LRI && cp <= PDI);
}

bool dropControls(std::string& text) {
  size_t write = 0;
  bool dropped = false;
  for (size_t pos = 0; pos < text.size();) {
    const size_t at = pos;
    if (isControl(decodeAt(text.data(), text.size(), pos))) {
      dropped = true;
      continue;
    }
    if (write != at) std::copy(text.begin() + at, text.begin() + pos, text.begin() + write);  // write < at
    write += pos - at;
  }
  text.resize(write);
  return dropped;
}

void OpenControls::advance(const std::string_view text) {
  // Every control here is E2 80 AA-AE or E2 81 A6-A9; 0xE2 is only ever a lead byte.
  for (size_t i = 0; i + 2 < text.size(); i++) {
    if (static_cast<uint8_t>(text[i]) != 0xE2) continue;
    const auto b1 = static_cast<uint8_t>(text[i + 1]);
    const auto b2 = static_cast<uint8_t>(text[i + 2]);
    uint32_t cp = 0;
    if (b1 == 0x80 && b2 >= 0xAA && b2 <= 0xAE) cp = 0x2000 + (b2 - 0x80);
    if (b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9) cp = 0x2040 + (b2 - 0x80);
    if (cp == 0) continue;
    i += 2;
    if (cp == PDF || cp == PDI) {
      if (overflow > 0) {
        overflow--;
      } else if (cp == PDF) {
        if (count > 0 && !isInitiator(cps[count - 1])) count--;
      } else {
        int innermost = count - 1;
        while (innermost >= 0 && !isInitiator(cps[innermost])) innermost--;
        if (innermost >= 0) count = static_cast<uint8_t>(innermost);
      }
    } else if (count < kMaxOpenControls) {
      cps[count++] = cp;
    } else {
      overflow++;
    }
  }
}

void Line::clear() {
  pieces.clear();
  display.clear();
  reordered = false;
  fallback = false;
}

bool cutsAreValid(const Token& token) {
  if (token.cutCount > 0 && token.cuts == nullptr) return false;
  size_t previous = 0;
  for (size_t i = 0; i < token.cutCount; i++) {
    const size_t cut = token.cuts[i];
    if (cut <= previous || cut >= token.text.size()) return false;
    if ((static_cast<uint8_t>(token.text[cut]) & 0xC0) == 0x80) return false;  // inside a code point
    previous = cut;
  }
  return true;
}

bool layoutLine(const Token* const callerTokens, const size_t count, const OpenControls& open,
                const uint8_t paragraphLevel, Scratch& scratch, Line& out) {
  out.clear();
  // The cuts checked, O(cuts): the pass reads the caller's tokens, or, when a cut list breaks its contract, a copy for
  // this call (one Token a token, made for a broken caller only) with that list dropped, the token laid out uncut.
  const Token* tokens = callerTokens;
  std::unique_ptr<Token[]> checked;
  size_t bad = 0;
  while (bad < count && cutsAreValid(callerTokens[bad])) bad++;
  if (bad < count) {
    LOG_ERR("BIDI", "Token %u's cuts break their contract: laid out uncut", static_cast<unsigned>(bad));
    checked = makeUniqueNoThrow<Token[]>(count);
    if (!checked) {
      LOG_ERR("BIDI", "No %u B for the line's tokens: laid out without bidi",
              static_cast<unsigned>(sizeof(Token) * count));
      return false;
    }
    std::copy(callerTokens, callerTokens + count, checked.get());
    for (size_t t = bad; t < count; t++) {
      if (cutsAreValid(checked[t])) continue;
      checked[t].cutCount = 0;
      checked[t].cuts = nullptr;
    }
    tokens = checked.get();
  }
  const uint8_t level = paragraphLevel & 1;
  size_t slots = 0;
  if (!collect(tokens, count, open, scratch, slots)) return fallbackLine(tokens, count, open, level, scratch, out);
  if (slots == 0) return true;
  if (!orderSlots(scratch, slots, level)) return false;  // out is still clear
  return buildPieces(tokens, count, scratch, slots, out) || fallbackLine(tokens, count, open, level, scratch, out);
}

namespace {
// The level of a token of letters alone, all of one strong class (L, or R and AL): no mark, joiner, digit, neutral or
// control among them, so nothing in it is resolved by its neighbours, and its form is its code points as they are
// (level 0) or reversed (level 1), on its own and in any line. -1 for every other token, an empty one included.
int plainLevel(const std::string_view token) {
  int level = -1;
  for (size_t pos = 0; pos < token.size();) {
    const int strong = BidiUtils::strongLevel(decodeAt(token.data(), token.size(), pos));
    if (strong < 0 || (level >= 0 && strong != level)) return -1;
    level = strong;
  }
  return level;
}

// The token's code points in reverse order, each one's bytes as they are (plainLevel found them well formed).
void appendReversed(const std::string_view token, std::string& out) {
  size_t end = token.size();
  while (end > 0) {
    size_t begin = end - 1;
    while (begin > 0 && (static_cast<unsigned char>(token[begin]) & 0xC0) == 0x80) begin--;
    out.append(token.data() + begin, end - begin);
    end = begin;
  }
}
}  // namespace

bool tokenForm(const std::string_view token, const OpenControls& open, const uint8_t paragraphLevel, Scratch& scratch,
               std::string& out, bool* const plain) {
  // The short path: a plain token that no shaper module touches (TextShaping::shape is false for a token drawn as
  // its own code points) needs no pass. With a control open the full path runs: an override among them would turn
  // the letters. Without a workspace the full path runs too, and leaves the form the text, as the line pass then
  // lays the line out without bidi. Measured on the host (Zaydan's Arabic chapter, 2,686 tokens): the pass ran once
  // a token here and once a line in the line pass, nine passes in ten for nothing.
  if (open.count == 0) {
    const int level = plainLevel(token);
    size_t shapedCount = 0;
    if (level >= 0 && !TextShaping::shape(token.data(), token.size(), scratch.units, kMaxUnits, shapedCount) &&
        BidiUtils::workspaceAvailable()) {
      if (plain != nullptr) *plain = true;
      if (level == 0) return false;
      out.clear();
      appendReversed(token, out);
      return true;
    }
  }
  if (plain != nullptr) *plain = false;
  const size_t previous = out.size();
  if (!appendForm(token, open, paragraphLevel, scratch, out)) return false;
  out.erase(0, previous);
  return true;
}

ArenaWorkspace::ArenaWorkspace(BuildArena* arena) {
  if (arena == nullptr || !arena->valid()) return;
  block_ = arena->reserveBlock();
  void* const workspace = arena->alloc(BidiUtils::kWorkspaceBytes, 4);
  void* const scratch = workspace ? arena->alloc(sizeof(Scratch), alignof(Scratch)) : nullptr;
  if (scratch == nullptr) {
    arena->release(block_);
    return;
  }
  arena_ = arena;
  scratch_ = new (scratch) Scratch;  // plain arrays: placed without zeroing, and nothing to destroy
  BidiUtils::bindWorkspace(workspace);
}

ArenaWorkspace::~ArenaWorkspace() {
  if (arena_ == nullptr) return;
  BidiUtils::bindWorkspace(nullptr);
  if (!arena_->release(block_)) {
    LOG_ERR("BIDI", "Line pass block not the arena's newest; its %zu bytes stay held",
            BidiUtils::kWorkspaceBytes + sizeof(Scratch));
  }
}

Scratch* heapScratch() {
  if (!heapScratchStore) {
    heapScratchStore = makeUniqueNoThrow<Scratch>();
    if (!heapScratchStore && !heapScratchRefusalLogged) {
      heapScratchRefusalLogged = true;
      LOG_ERR("BIDI", "No %u B for the line pass: laid out without bidi", static_cast<unsigned>(sizeof(Scratch)));
    }
  }
  return heapScratchStore.get();
}

void releaseBuildMemory() {
  heapScratchStore.reset();
  heapScratchRefusalLogged = false;
  BidiUtils::releaseWorkspace();
}

bool heapScratchHeld() { return static_cast<bool>(heapScratchStore); }

}  // namespace BidiLine
