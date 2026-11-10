#include "UiShapedText.h"

#include <BidiUtils.h>
#include <BuildArena.h>
#include <GlyphKey.h>
#include <GlyphProvider.h>
#include <IndicReorder.h>
#include <IndicScripts.h>
#include <IndicShaper.h>
#include <Logging.h>
#include <MarkOverlay.h>
#include <Memory.h>
#include <MyanmarScript.h>
#include <OtFace.h>
#include <ShapingFace.h>
#include <SmallCaps.h>
#include <TextShaping.h>
#include <TextTruncation.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <new>
#include <optional>

namespace ui_shaping {

namespace {

// A UI string past this keeps the reordered form (truncate() shapes its head): its shaping block, the call's and the
// pieces' rooms, stays about 11 KB on the C3, and a title is far shorter (five Hindi words are about 100 bytes, 7 KB).
constexpr size_t kMaxTextBytes = 255;
// The code points one ot::shape call takes (IndicShaper::CallMemory): a 48-slot buffer, 3 KB on the C3, where a
// book's 32 take 5 KB. A UI word past 16 code points is shaped in pieces between its syllables, with a syllable of
// context on either side (IndicShaper's loop), as a book's word past 32 is.
constexpr unsigned kRunCodepoints = 16;
// The scripts a string asks the UI face's chain about, at most: the first ones it holds.
constexpr size_t kMaxProbes = 4;
// The cuts of one run truncate() chooses among, at most: a run of more syllables is cut in its first ones. 64 B.
constexpr size_t kMaxRunCuts = 32;
// The cuts truncate() steps back over when the string it cut measures wider than its estimate (a cut before the first
// shaped run, which is measured unshaped).
constexpr int kMaxStepsBack = 8;

// The string the last shape() made (one heap block), and whether UI text made a shaping face since endPass().
Shaped lastShaped;
bool faceMade = false;
bool refusalLogged = false;  // until endPass()

size_t alignUp(const size_t bytes, const size_t align) { return (bytes + align - 1) / align * align; }

// One piece shapeToken wrote: its kind, its bytes of the string, and where its records or its form went.
struct Piece {
  IndicShaper::PieceKind kind;
  uint16_t begin;
  uint16_t end;
  uint16_t at;     // into the records (Glyphs) or the forms (Reordered)
  uint16_t bytes;  // of records or of the form
  uint16_t count;  // records
};

// shapeToken's pieces in fixed rooms of the shaping block; a piece past its room degrades the string (drawn reordered).
class PieceRooms final : public IndicShaper::PieceSink {
 public:
  PieceRooms(Piece* pieces, const size_t pieceRoom, uint8_t* records, const size_t recordRoom, char* forms,
             const size_t formRoom)
      : pieces_(pieces),
        pieceRoom_(pieceRoom),
        records_(records),
        recordRoom_(recordRoom),
        forms_(forms),
        formRoom_(formRoom) {}

  bool beginPiece(const IndicShaper::PieceKind kind, const uint16_t begin, const uint16_t end) override {
    if (count_ == pieceRoom_) return false;
    const size_t at = kind == IndicShaper::PieceKind::Glyphs ? recordsUsed_ : formsUsed_;
    pieces_[count_++] = Piece{kind, begin, end, static_cast<uint16_t>(at), 0, 0};
    return true;
  }
  bool addGlyph(const GlyphRecord& record) override {
    size_t written = 0;
    if (count_ == 0 || !glyph_run::encode(record, records_ + recordsUsed_, recordRoom_ - recordsUsed_, &written)) {
      return false;
    }
    recordsUsed_ += written;
    Piece& piece = pieces_[count_ - 1];
    piece.bytes = static_cast<uint16_t>(piece.bytes + written);
    piece.count++;
    return true;
  }
  bool addBytes(const char* bytes, const size_t length) override {
    if (count_ == 0 || length > formRoom_ - formsUsed_) return false;
    memcpy(forms_ + formsUsed_, bytes, length);
    formsUsed_ += length;
    Piece& piece = pieces_[count_ - 1];
    piece.bytes = static_cast<uint16_t>(piece.bytes + length);
    return true;
  }

  size_t count() const { return count_; }
  const Piece& operator[](const size_t i) const { return pieces_[i]; }
  const uint8_t* records() const { return records_; }
  size_t recordsUsed() const { return recordsUsed_; }
  const char* forms() const { return forms_; }

 private:
  Piece* pieces_;
  size_t pieceRoom_;
  uint8_t* records_;
  size_t recordRoom_;
  char* forms_;
  size_t formRoom_;
  size_t count_ = 0;
  size_t recordsUsed_ = 0;
  size_t formsUsed_ = 0;
};

// A run's prefix shaped for its width alone (truncate()): its records' pen advance, snapped as glyphRunAdvance snaps
// it for drawText. A piece that is not a shaped run leaves the width unknown.
class AdvanceSum final : public IndicShaper::PieceSink {
 public:
  bool beginPiece(const IndicShaper::PieceKind kind, uint16_t /*begin*/, uint16_t /*end*/) override {
    glyphsOnly_ = glyphsOnly_ && kind == IndicShaper::PieceKind::Glyphs;
    return true;
  }
  bool addGlyph(const GlyphRecord& record) override {
    sum_ += fp4::toPixel(record.advance);
    return true;
  }
  bool addBytes(const char* /*bytes*/, size_t /*length*/) override { return true; }
  bool glyphsOnly() const { return glyphsOnly_; }
  int sum() const { return sum_; }

 private:
  bool glyphsOnly_ = true;
  int sum_ = 0;
};

// The probe letters of the scripts `text` holds, in the order they first appear, at most kMaxProbes.
size_t probesOf(const std::string_view text, uint32_t* probes) {
  size_t count = 0;
  for (size_t pos = 0; pos < text.size() && count < kMaxProbes;) {
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    uint32_t probe = 0;
    if (const indic::ScriptInfo* const script = indic::scriptOf(cp)) {
      probe = script->probe;
    } else if (myanmar::isRunMember(cp)) {
      probe = myanmar::kProbe;
    }
    if (probe != 0 && std::find(probes, probes + count, probe) == probes + count) probes[count++] = probe;
  }
  return count;
}

// Whether `face` maps one of the probes: its cmap, nothing rendered. False while it cannot be mapped.
bool mapsAny(ShapingFace& face, const uint32_t* probes, const size_t count) {
  const ot::Face* const shaping = face.current();
  if (shaping == nullptr) return false;
  for (size_t i = 0; i < count; ++i) {
    uint32_t glyph = 0;
    if (shaping->nominalGlyph(probes[i], &glyph)) return true;
  }
  return false;
}

void logRefusal([[maybe_unused]] const size_t bytes) {
  if (refusalLogged) return;
  refusalLogged = true;
  LOG_ERR("UISHP", "No %u B to shape UI text: it is drawn reordered", static_cast<unsigned>(bytes));
}

// The face `source` (shapingSource()) shapes over, at the size its glyphs are drawn; nullptr when the heap refused it.
// A face this call makes is UI text's (endPass()).
ShapingFace* faceOf(const EpdFontData* source, uint32_t* ppem26_6) {
  GlyphProvider* const provider = source->provider;
  const bool held = provider->holdsShapingFace();
  ShapingFace* const face = provider->shapingFace(ppem26_6, nullptr);
  if (face != nullptr && !held) faceMade = true;
  return face;
}

// The pen advance of `run` (a run of the string, or its prefix) shaped alone over `face`, in `arena` (room for
// CallMemory::bytesFor(run.size())); INT_MAX when it does not shape into a run of glyphs.
int runAdvance(ShapingFace& face, const uint32_t ppem26_6, const std::string_view run, BuildArena& arena) {
  AdvanceSum sum;
  IndicShaper::CallMemory memory(&arena, run.size(), IndicShaper::CallKind::Shape, kRunCodepoints);
  if (!memory.valid()) return INT_MAX;
  IndicShaper::Request request;
  request.face = &face;
  request.ppem26_6 = ppem26_6;
  const IndicShaper::Outcome outcome = IndicShaper::shapeToken(run, request, memory, sum);
  return outcome == IndicShaper::Outcome::Formed && sum.glyphsOnly() ? sum.sum() : INT_MAX;
}

// codepointAdvance of `text` with the ellipsis after it, through `scratch`.
int advanceWithEllipsis(const EpdFontFamily& family, const EpdFontFamily::Style style, const std::string_view text,
                        std::string& scratch) {
  scratch.assign(text.data(), text.size());
  scratch += textTruncation::ELLIPSIS_UTF8;
  return codepointAdvance(family, scratch.c_str(), style);
}

// The cut before byte `at` that cutAllowed() allows, nearest first; 0 when there is none.
size_t previousCut(const std::string_view text, size_t at) {
  while (at > 0) {
    do at--;
    while (at > 0 && (static_cast<uint8_t>(text[at]) & 0xC0) == 0x80);
    if (at > 0 && cutAllowed(text, at)) return at;
  }
  return 0;
}

// The furthest cut inside the Text segment `segment`, or at its end when the string goes on, at which the string,
// ended there with an ellipsis, measures narrower than `maxWidth`; `before` is the advance of the segments ahead of
// it. The ellipsis joins the segment, so each prefix is measured with it, as the cut string's last piece. `plain`: no
// run is ahead, so the cut string is not shaped, and is measured as width() measures such a string, by its ink box.
size_t bestTextCut(const EpdFontFamily& family, const EpdFontFamily::Style style, const std::string_view text,
                   const Segment& segment, const int before, const int maxWidth, const bool plain) {
  std::string scratch;
  size_t best = segment.begin;
  for (size_t at = segment.begin; at < segment.end;) {
    TextShaping::decodeAt(text, at);
    if (at >= text.size() || !cutAllowed(text, at)) continue;
    int width = 0;
    if (plain) {
      scratch.assign(text.data(), at);
      scratch += textTruncation::ELLIPSIS_UTF8;
      width = ui_shaping::width(family, style, scratch.c_str(), false);
    } else {
      width = before + advanceWithEllipsis(family, style, text.substr(segment.begin, at - segment.begin), scratch);
    }
    if (width >= maxWidth) break;
    best = at;
  }
  return best;
}

// The same inside a run, between its syllables, found by halving: the cut run is shaped up to each cut tried (a run
// in a script the face does not map is reordered up to it), and ends the string before the ellipsis, a piece of its
// own. The estimate is exact wherever the cut string shapes: shapeToken shapes each run alone, so the segments ahead
// keep their records.
size_t bestRunCut(const EpdFontFamily& family, const EpdFontFamily::Style style, const std::string_view text,
                  const Segment& segment, const EpdFontData* source, const int before, const int ellipsis,
                  const int maxWidth) {
  uint16_t cuts[kMaxRunCuts];
  size_t count = 0;
  for (size_t at = segment.begin; at < segment.end && count < kMaxRunCuts;) {
    TextShaping::decodeAt(text, at);
    if (at < segment.end && cutAllowed(text, at)) cuts[count++] = static_cast<uint16_t>(at);
  }
  if (count == 0) return segment.begin;

  ShapingFace* face = nullptr;
  uint32_t ppem26_6 = 0;
  std::unique_ptr<uint8_t[]> work;
  std::optional<BuildArena> arena;
  if (segment.glyphs) {
    // One call block for every prefix tried: CallMemory gives it back to the arena after each.
    const size_t bytes =
        IndicShaper::CallMemory::bytesFor(segment.end - segment.begin, IndicShaper::CallKind::Shape, kRunCodepoints);
    face = faceOf(source, &ppem26_6);
    work = makeUniqueNoThrow<uint8_t[]>(bytes);
    if (face == nullptr || !work) {
      if (!work) logRefusal(bytes);
      return segment.begin;
    }
    arena.emplace(work.get(), bytes);
  }
  std::string scratch;
  const auto fits = [&](const size_t cut) {
    const std::string_view prefix = text.substr(segment.begin, cut - segment.begin);
    int advance = INT_MAX;
    if (segment.glyphs) {
      advance = runAdvance(*face, ppem26_6, prefix, *arena);
    } else {
      scratch.resize(3 * prefix.size() + 1);
      size_t formBytes = 0;
      if (indicReorderForDisplay(prefix.data(), prefix.size(), scratch.data(), scratch.size(), &formBytes)) {
        scratch.resize(formBytes);
      } else {
        scratch.assign(prefix.data(), prefix.size());
      }
      advance = codepointAdvance(family, scratch.c_str(), style);
    }
    return advance != INT_MAX && before + advance + ellipsis < maxWidth;
  };
  int lo = -1;  // the run's start, which fits: the segments ahead did with an ellipsis
  int hi = static_cast<int>(count);
  while (hi - lo > 1) {
    const int mid = (lo + hi) / 2;
    if (fits(cuts[mid])) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo < 0 ? segment.begin : cuts[lo];
}

}  // namespace

// The shaped form of `text` into `out` (empty), over `face` at `ppem26_6`: shapeToken in one transient block, then the
// segments copied into one block of their exact size, and measured once the transient block is gone. False when a
// block was refused or the shaper wrote no form (Plain) or could not (Degraded).
bool build(const EpdFontFamily& family, const EpdFontFamily::Style style, const std::string_view text,
           const EpdFontData* source, ShapingFace& face, const uint32_t ppem26_6, Shaped& out) {
  const size_t n = text.size();
  // The rooms shapeToken writes into, ahead of the call's block: every piece of a string whose runs (3 bytes at least)
  // alternate with text; four record bytes for each byte of text (a code point of 3 bytes gives two 6-byte records at
  // most, mostly one of 4) and 64 to spare; IndicReorder's form at its bound.
  const size_t pieceRoom = 2 * (n / 3) + 2;
  const size_t recordRoom = 4 * n + 64;
  const size_t formRoom = 3 * n + 1;
  const size_t roomBytes = alignUp(pieceRoom * sizeof(Piece) + recordRoom + formRoom, alignof(std::max_align_t));
  const size_t callBytes = IndicShaper::CallMemory::bytesFor(n, IndicShaper::CallKind::Shape, kRunCodepoints);
  if (callBytes == 0) return false;
  // One transient block, about 7 KB on the C3 for a 100-byte title (the call's 3 KB glyph buffer the most of it): past
  // any task's stack. It is freed before this call returns, and before any glyph is looked up.
  std::unique_ptr<uint8_t[]> work = makeUniqueNoThrow<uint8_t[]>(roomBytes + callBytes);
  if (!work) {
    logRefusal(roomBytes + callBytes);
    return false;
  }
  auto* const pieces = reinterpret_cast<Piece*>(work.get());
  uint8_t* const records = work.get() + pieceRoom * sizeof(Piece);
  char* const forms = reinterpret_cast<char*>(records + recordRoom);
  PieceRooms rooms(pieces, pieceRoom, records, recordRoom, forms, formRoom);
  IndicShaper::Outcome outcome = IndicShaper::Outcome::Degraded;
  {
    BuildArena arena(work.get() + roomBytes, callBytes);
    IndicShaper::CallMemory memory(&arena, n, IndicShaper::CallKind::Shape, kRunCodepoints);
    if (!memory.valid()) return false;
    IndicShaper::Request request;
    request.face = &face;
    request.ppem26_6 = ppem26_6;
    outcome = IndicShaper::shapeToken(text, request, memory, rooms);
  }
  if (outcome != IndicShaper::Outcome::Formed || rooms.count() == 0) return false;

  // The result: the segments, the string (its key), the segments' texts, each NUL-terminated, and the records.
  size_t textBytes = n + 1;
  for (size_t i = 0; i < rooms.count(); ++i) {
    const Piece& piece = rooms[i];
    if (piece.kind == IndicShaper::PieceKind::Text) textBytes += piece.end - piece.begin + 1u;
    if (piece.kind == IndicShaper::PieceKind::Reordered) textBytes += piece.bytes + 1u;
  }
  const size_t segmentBytes = rooms.count() * sizeof(Segment);
  std::unique_ptr<uint8_t[]> block = makeUniqueNoThrow<uint8_t[]>(segmentBytes + textBytes + rooms.recordsUsed());
  if (!block) {
    logRefusal(segmentBytes + textBytes + rooms.recordsUsed());
    return false;
  }
  auto* const segments = reinterpret_cast<Segment*>(block.get());
  char* const key = reinterpret_cast<char*>(block.get() + segmentBytes);
  memcpy(key, text.data(), n);
  key[n] = '\0';
  char* at = key + n + 1;
  uint8_t* const runs = block.get() + segmentBytes + textBytes;
  memcpy(runs, rooms.records(), rooms.recordsUsed());
  for (size_t i = 0; i < rooms.count(); ++i) {
    const Piece& piece = rooms[i];
    Segment* const segment = new (&segments[i]) Segment;
    segment->begin = piece.begin;
    segment->end = piece.end;
    switch (piece.kind) {
      case IndicShaper::PieceKind::Glyphs:
        segment->glyphs = true;
        segment->run = GlyphRun{runs + piece.at, piece.count, piece.bytes};
        break;
      case IndicShaper::PieceKind::Text:
      case IndicShaper::PieceKind::Reordered: {
        const bool form = piece.kind == IndicShaper::PieceKind::Reordered;
        const size_t length = form ? piece.bytes : piece.end - piece.begin;
        memcpy(at, form ? rooms.forms() + piece.at : text.data() + piece.begin, length);
        at[length] = '\0';
        segment->text = at;
        segment->reordered = form;
        at += length + 1;
        break;
      }
    }
  }
  const size_t count = rooms.count();
  work.reset();

  // Measured now that no shaping memory is held: a Text segment's glyphs may be looked up, and rendered, here.
  int width = 0;
  for (size_t i = 0; i < count; ++i) {
    Segment& segment = segments[i];
    segment.advance =
        segment.glyphs ? glyphRunAdvance(segment.run, false) : codepointAdvance(family, segment.text, style);
    width += segment.advance;
  }
  out.block_ = std::move(block);
  out.segments_ = segments;
  out.count_ = static_cast<uint16_t>(count);
  out.width_ = width;
  out.ui_ = family.getData(style);
  out.source_ = source;
  out.text_ = std::string_view(key, n);
  return true;
}

bool Shaped::matches(const EpdFontData* ui, const EpdFontData* source, const std::string_view text) const {
  return block_ && ui_ == ui && source_ == source && text_ == text;
}

void Shaped::reset() {
  // The segments are trivially destructible: the block is all there is to free.
  block_.reset();
  segments_ = nullptr;
  count_ = 0;
  width_ = 0;
  ui_ = nullptr;
  source_ = nullptr;
  text_ = {};
}

const char* reordered(const char* text, std::string& holder) {
  if (!text || !indic::containsIndic(text)) return text;
  return indicReorderForDisplay(text, holder) ? holder.c_str() : text;
}

bool wantsShaping(const char* text) {
  if (text == nullptr) return false;
  const size_t length = strlen(text);
  return (indic::containsIndic(text) || myanmar::holdsMyanmar(text, length)) && !BidiUtils::hasRtlClass(text, length);
}

const Shaped* shape(const EpdFontFamily& family, const EpdFontFamily::Style style, const char* text) {
  if (!wantsShaping(text)) return nullptr;
  const std::string_view view(text);
  if (view.size() > kMaxTextBytes) return nullptr;
  const EpdFontData* const ui = family.getData(style);
  if (ui == nullptr || ui->provider == nullptr) return nullptr;
  uint32_t probes[kMaxProbes];
  const size_t probeCount = probesOf(view, probes);
  const EpdFontData* source = nullptr;
  for (size_t i = 0; i < probeCount && source == nullptr; ++i) source = ui->provider->shapingSource(probes[i]);
  if (source == nullptr) return nullptr;
  if (lastShaped.matches(ui, source, view)) return &lastShaped;

  uint32_t ppem26_6 = 0;
  ShapingFace* const face = faceOf(source, &ppem26_6);
  if (face == nullptr || !mapsAny(*face, probes, probeCount)) return nullptr;
  lastShaped.reset();  // its block goes before the new ones are asked for
  if (!build(family, style, view, source, *face, ppem26_6, lastShaped)) {
    lastShaped.reset();
    return nullptr;
  }
  return &lastShaped;
}

int codepointAdvance(const EpdFontFamily& family, const char* text, const EpdFontFamily::Style style) {
  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (overlaysItsBase(cp)) {
      continue;
    }
    cp = family.applyLigatures(cp, text, style);

    // Small-caps fold — mirror drawText so measurement and rendering agree exactly.
    const bool folded = (style & EpdFontFamily::SMALL_CAPS) != 0 && smallCaps::fold(cp);

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      auto kernFP = static_cast<int32_t>(family.getKerning(prevCp, cp, style));  // 4.4 fixed-point kern
      if (folded) kernFP = static_cast<int32_t>(kernFP * smallCaps::SCALE + 0.5f);
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);  // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyphRef glyph = family.getGlyph(cp, style);
    if (!glyph) {
      widthPx += fp4::toPixel(prevAdvanceFP);
      prevCp = 0;
      prevAdvanceFP = 0;
      continue;
    }
    prevAdvanceFP = glyph.advanceX;
    // SUP/SUB no longer halve here — superscript/subscript words are measured at their
    // explicit per-word scale by the layout code, matching drawTextScaled exactly.
    if (folded) {
      prevAdvanceFP = static_cast<int>(prevAdvanceFP * smallCaps::SCALE + 0.5f);
    }
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int width(const EpdFontFamily& family, const EpdFontFamily::Style style, const char* text, const bool mayShape) {
  if (text == nullptr) return 0;
  family.beginText(text, style);  // the string decides its glyphs' source (GlyphProvider::beginText)
  if (mayShape) {
    if (const Shaped* const shaped = shape(family, style, text)) return shaped->width();
  }
  std::string holder;
  int w = 0;
  int h = 0;
  family.getTextDimensions(reordered(text, holder), &w, &h, style);
  return w;
}

bool cutAllowed(const std::string_view text, const size_t at) {
  if (at == 0 || at >= text.size()) return true;
  size_t pos = at;
  const uint32_t cp = TextShaping::decodeAt(text, pos);
  if (overlaysItsBase(cp) || utf8IsDefaultIgnorable(cp)) return false;
  // A syllable ends with its script's code points: a space or a Latin letter after a word's final virama may follow a
  // cut, which glueInsideSyllable (written for tokens glued without a space) refuses.
  const bool syllabic =
      indic::scriptOf(cp) != nullptr || indic::isShared(cp) || myanmar::isRunMember(cp) || myanmar::isShared(cp);
  return !syllabic || !TextShaping::glueInsideSyllable(text.substr(0, at), text.substr(at));
}

bool truncate(const EpdFontFamily& family, const EpdFontFamily::Style style, const char* text, const int maxWidth,
              std::string& out) {
  std::string_view all(text);
  // A string longer than shape() takes is fitted from its head, as much of it as shape() takes with an ellipsis after
  // it, cut where a cut is allowed, and ends with an ellipsis however narrow that head is.
  std::string head;
  const bool cutAnyway = all.size() > kMaxTextBytes;
  if (cutAnyway) {
    if (!wantsShaping(text)) return false;
    head.assign(text, previousCut(all, kMaxTextBytes - strlen(textTruncation::ELLIPSIS_UTF8) + 1));
    family.beginText(head.c_str(), style);
    all = head;
  }
  const Shaped* const shaped = shape(family, style, all.data());
  if (shaped == nullptr) return false;
  if (!cutAnyway && shaped->width() <= maxWidth) {
    out = text;
    return true;
  }
  const int ellipsis = codepointAdvance(family, textTruncation::ELLIPSIS_UTF8, style);
  size_t cut = 0;
  if (cutAnyway && shaped->width() + ellipsis < maxWidth) {
    cut = all.size();
  } else {
    // The segments the cut string keeps whole, and the one it is cut in: the string does not fit, so one is.
    int before = 0;
    size_t k = 0;
    while (k + 1 < shaped->size() && before + (*shaped)[k].advance + ellipsis < maxWidth) {
      before += (*shaped)[k].advance;
      ++k;
    }
    const Segment& segment = (*shaped)[k];
    if (segment.glyphs || segment.reordered) {
      if (before + ellipsis < maxWidth) {
        cut = bestRunCut(family, style, all, segment, shaped->source(), before, ellipsis, maxWidth);
      }
    } else {
      // Text pieces lie between runs, so only the first segment has no run ahead of it.
      cut = bestTextCut(family, style, all, segment, before, maxWidth, k == 0);
    }
  }

  // Measured as the UI measures it (width(), which also keeps it shaped for the draw that follows): a cut before the
  // first shaped run leaves a string measured unshaped, whose ink box may not be its advance. While it does not fit,
  // the cut steps back.
  std::string candidate;
  for (int step = 0; cut > 0 && step < kMaxStepsBack; ++step) {
    candidate.assign(all.data(), cut);
    candidate += textTruncation::ELLIPSIS_UTF8;
    if (width(family, style, candidate.c_str()) < maxWidth) {
      out = std::move(candidate);
      return true;
    }
    cut = previousCut(all, cut);
  }
  out = textTruncation::ELLIPSIS_UTF8;
  return true;
}

bool inkExtents(const EpdFontFamily& family, const EpdFontFamily::Style style, const Shaped& shaped, int* aboveBaseline,
                int* belowBaseline) {
  *aboveBaseline = 0;
  *belowBaseline = 0;
  bool any = false;
  // glyph.top: baseline to bitmap top edge; ink below baseline = height - top. A record's offset moves it (y down).
  const auto take = [&](const EpdGlyphRef& glyph, const int dy) {
    *aboveBaseline = std::max(*aboveBaseline, static_cast<int>(glyph.top) - dy);
    *belowBaseline = std::max(*belowBaseline, static_cast<int>(glyph.height) - static_cast<int>(glyph.top) + dy);
    any = true;
  };
  const EpdFont font(family.getData(style));  // a run's glyphs come by key through the UI face's own chain
  for (const Segment& segment : shaped) {
    if (segment.glyphs) {
      forEachGlyph(segment.run, 0, false, [&](const GlyphRecord& record, int /*penX*/) {
        if (const EpdGlyphRef glyph = font.findGlyph(glyph_key::of(record.glyph))) take(glyph, record.dy);
      });
      continue;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(segment.text);
    while (const uint32_t cp = utf8NextCodepoint(&p)) {
      if (const EpdGlyphRef glyph = family.getGlyph(cp, style)) take(glyph, 0);
    }
  }
  return any;
}

bool endPass() {
  lastShaped.reset();
  refusalLogged = false;
  const bool made = faceMade;
  faceMade = false;
  return made;
}

}  // namespace ui_shaping
