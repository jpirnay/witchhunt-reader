#include "BidiLine.h"

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <memory>
#include <new>

namespace BidiLine {
namespace {

// The build's heap Scratch (heapScratch): the loop task alone runs the line pass, so it needs no lock.
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

// --- tokenForm --------------------------------------------------------------------------------------------------

// Appends the token's form to `dest`. When it is the token's text byte for byte, takes it back off and returns false.
bool appendForm(const std::string_view token, const OpenControls& open, const uint8_t paragraphLevel, Scratch& s,
                std::string& dest) {
  // The level, and the controls prepended: an open isolate is never prepended, it gives the level instead.
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

  // The bidi input: the embeddings and overrides opened inside the isolate (or all of them), then the code points
  // without their joiners (BN, which X9 removes; leaving them out keeps a mark after a joiner on its base).
  size_t n = 0;
  for (size_t i = static_cast<size_t>(isolate + 1); i < open.count; i++) s.passCp[n++] = open.cps[i];
  bool fits = true;
  const auto add = [&](const uint32_t cp) {
    if (isJoiner(cp)) return;
    if (n == kMaxCodepoints) {
      fits = false;
      return;
    }
    s.passCp[n++] = cp;
  };
  if (shaped) {
    for (size_t k = 0; fits && k < shapedCount; k++) add(s.units[k].cp);
  } else {
    for (size_t pos = 0; fits && pos < token.size();) add(decodeAt(token.data(), token.size(), pos));
  }

  const size_t start = dest.size();
  if (fits && n > 0 && BidiUtils::orderLine(s.passCp, n, level, s.passVisual, s.passDrawn)) {
    for (size_t v = 0; v < n; v++) {
      if (isDrawn(s.passDrawn[v])) utf8AppendCodepoint(dest, s.passDrawn[v]);
    }
  } else if (shaped) {
    // Past the bidi limit or without a workspace applyBidiVisual leaves the text as it is; the drops still apply.
    for (size_t k = 0; k < shapedCount; k++) {
      if (isDrawn(s.units[k].cp)) utf8AppendCodepoint(dest, s.units[k].cp);
    }
  } else {
    for (size_t pos = 0; pos < token.size();) {
      const size_t at = pos;
      if (isDrawn(decodeAt(token.data(), token.size(), pos))) dest.append(token.data() + at, pos - at);
    }
  }
  if (std::string_view(dest).substr(start) == token) {
    dest.resize(start);
    return false;
  }
  return true;
}

// --- layoutLine: the logical sequence -----------------------------------------------------------------------------

// Fills units and slots: the open controls, then each token's units with a separator before each spaced token after
// the first. A transparent mark or a joiner that follows a unit of its own token (not a control) is folded onto that
// unit's slot.
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
    for (size_t u = first; u < units; u++) {
      const bool ridesOnBase = u > first && ridesOnItsBase(s.units[u].cp) && !isControl(s.units[u - 1].cp);
      if (!ridesOnBase && !addSlot(u, token)) return false;
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
// or embedding that carries part of a word away leaves the rest in two pieces.
void markCuts(Scratch& s, const size_t slots, uint8_t* const rankOf, uint8_t* const cutBefore, uint16_t* const run) {
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
    if (length > 0 && runToken != token) cutRun();
    runToken = token;
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

// The line's text bytes, and one more per token: about what its forms and their NULs take.
size_t formBytes(const Token* tokens, const size_t count) {
  size_t bytes = 0;
  for (size_t t = 0; t < count; t++) bytes += tokens[t].text.size() + 1;
  return bytes;
}

// Walks the visual order into pieces, then gives each its bytes and its reading index. False only when a shaped
// cluster's units land in two pieces: the caller falls back.
bool buildPieces(const Token* tokens, const size_t count, Scratch& s, const size_t slots, Line& out) {
  // About one piece per token, and the forms about the tokens' bytes: a line that holds more grows the buffers once.
  reserveAtLeast(out.pieces, count);
  reserveAtLeast(out.display, formBytes(tokens, count));
  // Free once the slots are ordered.
  uint8_t* const rankOf = s.expanded;
  uint8_t* const cutBefore = s.openSpans;
  markCuts(s, slots, rankOf, cutBefore, s.passEntry);

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
  // token's end. The same order ranks them for reading. Pieces are at most the slots, and the pass arrays are free.
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
  for (size_t r = 0; r < n; r++) {
    Piece& piece = out.pieces[pieceRank[r]];
    const bool first = r == 0 || out.pieces[pieceRank[r - 1]].token != piece.token;
    const bool last = r + 1 == n || out.pieces[pieceRank[r + 1]].token != piece.token;
    const uint16_t next =
        last ? static_cast<uint16_t>(tokens[piece.token].text.size()) : out.pieces[pieceRank[r + 1]].begin;
    if (piece.end >= next) return false;  // a shaped cluster shared with the next piece
    if (first) piece.begin = 0;
    piece.end = next;
    piece.readingIndex = static_cast<uint8_t>(r);
    if (pieceRank[r] != r) out.reordered = true;
  }
  return true;
}

// --- layoutLine: the fallback (upstream #2541's result) -------------------------------------------------------------

// One piece per token with a drawn form, in computeVisualWordOrder's order over the tokens without their transparent
// marks; when it declines, reversed at level 1 and as is at level 0. Its corners are upstream's: past 128 code points
// it declines too, so a right-to-left line reverses an embedded Latin phrase's words, and minibidi sees isolates as
// embeddings. False (out cleared) when BidiUtils has no workspace, as for a line that fits.
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
  // order with its inverse (4 bytes a token), in the Line's buffers: only a fallback line longer than any before
  // allocates.
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
  reserveAtLeast(order, 2 * count);
  if (!BidiUtils::computeVisualWordOrder(words, paragraphLevel == 1, order)) order.clear();
  if (order.size() != count) {
    order.clear();
    for (size_t v = 0; v < count; v++) order.push_back(static_cast<uint16_t>(paragraphLevel == 1 ? count - 1 - v : v));
  }
  order.resize(2 * count);
  for (size_t v = 0; v < count; v++) order[count + order[v]] = static_cast<uint16_t>(v);  // each token's visual index

  // The forms, made in logical order as the open controls follow the text.
  reserveAtLeast(out.display, formBytes(tokens, count));
  reserveAtLeast(out.pieces, count);
  out.pieces.resize(count);
  OpenControls at = open;
  for (size_t t = 0; t < count; t++) {
    const std::string_view text = tokens[t].text;
    Piece& piece = out.pieces[order[count + t]];
    piece.token = static_cast<uint16_t>(t);
    piece.begin = 0;
    piece.end = static_cast<uint16_t>(text.size());
    piece.displayBegin = static_cast<uint16_t>(out.display.size());
    if (!appendForm(text, at, paragraphLevel, s, out.display)) out.display.append(text);
    piece.displayLength = static_cast<uint16_t>(out.display.size() - piece.displayBegin);
    out.display.push_back('\0');
    at.advance(text);
  }

  // A token with no drawn character gives no piece; the rest are ranked by token.
  size_t rank = 0;
  for (size_t t = 0; t < count; t++) {
    Piece& piece = out.pieces[order[count + t]];
    if (piece.displayLength > 0) piece.readingIndex = static_cast<uint8_t>(rank++);
  }
  out.pieces.erase(
      std::remove_if(out.pieces.begin(), out.pieces.end(), [](const Piece& p) { return p.displayLength == 0; }),
      out.pieces.end());

  // Visual neighbours that are logical neighbours keep the later one's glue; any other pair is a word space, the
  // right-hand piece's.
  for (size_t v = 0; v < out.pieces.size(); v++) {
    Piece& piece = out.pieces[v];
    if (piece.readingIndex != v) out.reordered = true;
    piece.spaceToken = piece.token;
    if (v == 0) {
      piece.gapBefore = Gap::None;
      continue;
    }
    const uint16_t left = out.pieces[v - 1].token;
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

bool layoutLine(const Token* tokens, const size_t count, const OpenControls& open, const uint8_t paragraphLevel,
                Scratch& scratch, Line& out) {
  out.clear();
  const uint8_t level = paragraphLevel & 1;
  size_t slots = 0;
  if (!collect(tokens, count, open, scratch, slots)) return fallbackLine(tokens, count, open, level, scratch, out);
  if (slots == 0) return true;
  if (!orderSlots(scratch, slots, level)) return false;  // out is still clear
  return buildPieces(tokens, count, scratch, slots, out) || fallbackLine(tokens, count, open, level, scratch, out);
}

bool tokenForm(const std::string_view token, const OpenControls& open, const uint8_t paragraphLevel, Scratch& scratch,
               std::string& out) {
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
    if (!heapScratchStore) {
      LOG_ERR("BIDI", "No %u B for the line pass: laid out without bidi", static_cast<unsigned>(sizeof(Scratch)));
    }
  }
  return heapScratchStore.get();
}

void releaseBuildMemory() {
  heapScratchStore.reset();
  BidiUtils::releaseWorkspace();
}

bool heapScratchHeld() { return static_cast<bool>(heapScratchStore); }

}  // namespace BidiLine
