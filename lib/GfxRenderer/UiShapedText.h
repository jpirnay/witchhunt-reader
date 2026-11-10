#pragma once
// Ours (writing modes step 5): UI text -- book titles, authors, chapter names, the TOC, the status bar -- with its
// Indic and Myanmar runs shaped. The UI faces are bitmap; a script they lack is drawn from the TrueType face behind
// them (the reader's, or outside the reader the last book's, FallbackFaceProvider::setReaderFace). When that face draws
// glyph keys, a string's runs are shaped over it (IndicShaper::shapeToken) at the size its glyphs are drawn, and the
// glyphs come by ID through the UI face's own provider chain, as that face's characters do: the same 1-bit ink, side by
// side. Anything else keeps the reordered form (reordered()). Split out of GfxRenderer so the host can test it: it
// needs a font and nothing else, as TextTruncation does.

#include <EpdFontFamily.h>
#include <GlyphRun.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class ShapingFace;

namespace ui_shaping {

/// UI text's Indic syllables in the order the UI draws them when they are not shaped (indicReorderForDisplay): a
/// pre-base vowel sign in front of its consonant, a two-part vowel split around it, a nukta letter precomposed; a
/// conjunct keeps its visible virama, as a book in a bitmap font does. The form, kept in `holder`, or `text` itself.
/// The order is not idempotent -- "किकि" ordered twice moves the second sign in front again -- so it is applied only
/// where text is measured or drawn, never to a string handed back (truncatedText, wrappedText).
const char* reordered(const char* text, std::string& holder);

/// Whether `text` is one shape() may take: it holds Indic (U+0900-0DFF) or Myanmar text and no right-to-left
/// character (a line with one keeps the bidi path, BidiUtils::visualUiLine). A byte scan.
bool wantsShaping(const char* text);

/// One piece of a shaped string, in drawing order (left to right: such a string has no right-to-left character).
struct Segment {
  uint16_t begin = 0;  // the bytes of the string it draws: [begin, end)
  uint16_t end = 0;
  bool glyphs = false;     // a shaped run: `run`'s records, by glyph ID of Shaped::source()
  bool reordered = false;  // `text` is IndicReorder's form of a run in a script the face does not map
  const char* text = "";   // NUL-terminated, drawn through the codepoint path; empty for a run
  GlyphRun run;
  int advance = 0;  // how far the pen moves over it, in whole pixels (codepointAdvance, glyphRunAdvance)
};

/// A string shaped for the UI: its segments, which cover it in order. Owns one heap block of the exact size.
class Shaped {
 public:
  const Segment* begin() const { return segments_; }
  const Segment* end() const { return segments_ + count_; }
  size_t size() const { return count_; }
  const Segment& operator[](const size_t i) const { return segments_[i]; }
  /// The pen's advance over the whole string: what the UI measures it as (rawTextWidth).
  int width() const { return width_; }
  /// The face whose glyph IDs the runs hold (the reader's face at the UI size).
  const EpdFontData* source() const { return source_; }

  bool matches(const EpdFontData* ui, const EpdFontData* source, std::string_view text) const;
  void reset();

 private:
  friend bool build(const EpdFontFamily& family, EpdFontFamily::Style style, std::string_view text,
                    const EpdFontData* source, ShapingFace& face, uint32_t ppem26_6, Shaped& out);
  std::unique_ptr<uint8_t[]> block_;
  const Segment* segments_ = nullptr;
  uint16_t count_ = 0;
  int width_ = 0;
  const EpdFontData* ui_ = nullptr;
  const EpdFontData* source_ = nullptr;
  std::string_view text_;
};

/// The shaped form of `text` in `family`'s `style`, after family.beginText(text, style) (which decides where its
/// glyphs come from); nullptr when it is drawn as before: wantsShaping() is false, the string is longer than 255 bytes,
/// no face behind the UI face shapes its script (GlyphProvider::shapingSource) or maps its probe letter, or the face,
/// the plan or the call's memory was refused. Unscaled, as drawText draws. Under RenderLock (the face reads the glyph
/// store). The last string shaped is kept, so a title measured and then drawn is shaped once: the result is valid until
/// the next shape(), truncate() or width() call, or endPass().
const Shaped* shape(const EpdFontFamily& family, EpdFontFamily::Style style, const char* text);

/// drawText's pen over `text` in `family`'s `style` at the font's own size: the sum of its advances and kerning,
/// snapped per glyph as drawText snaps it. Asks nothing of beginText(); GfxRenderer::getTextAdvanceX is this walk.
int codepointAdvance(const EpdFontFamily& family, const char* text, EpdFontFamily::Style style);

/// The width UI text measures unscaled (GfxRenderer::rawTextWidth), beginText() included: its shaped form's advance
/// when `mayShape` and shape() gives one, else the ink box of its reordered form (EpdFontFamily::getTextDimensions).
int width(const EpdFontFamily& family, EpdFontFamily::Style style, const char* text, bool mayShape = true);

/// Whether UI text may be cut at byte `at` of `text` (0 < at < size, a character boundary): not inside an Indic or a
/// Myanmar syllable (TextShaping::glueInsideSyllable) and not before a mark drawn over its letter.
bool cutAllowed(std::string_view text, size_t at);

/// truncatedText for a string shape() takes, after family.beginText(text, style): `text` when its shaped form fits in
/// `maxWidth` (<=), else its longest prefix cut where cutAllowed() allows, with U+2026 appended, narrower than
/// `maxWidth`, found from the one shaping (a run cut inside is shaped again up to the cut) and measured as width()
/// measures it; U+2026 alone when nothing fits. False, `out` untouched, when shape() gives no form: the caller fits the
/// text as before.
bool truncate(const EpdFontFamily& family, EpdFontFamily::Style style, const char* text, int maxWidth,
              std::string& out);

/// The ink extents of a shaped string about its baseline, as GfxRenderer::getTextInkMetrics reads a string's glyphs: a
/// run's glyphs where their offsets put them. False when no glyph was found.
bool inkExtents(const EpdFontFamily& family, EpdFontFamily::Style style, const Shaped& shaped, int* aboveBaseline,
                int* belowBaseline);

/// The end of a render pass (the RenderLock's release): the string kept is freed, and the answer is whether UI text
/// made a shaping face in the pass, which the caller frees (GfxRenderer::releaseShapingFaces) so that nothing it shaped
/// with stays on the heap. A face a section build made is the build's, and is left to it.
bool endPass();

}  // namespace ui_shaping
