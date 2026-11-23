#include "PagedTextBody.h"

#include <GfxRenderer.h>
#include <HeapFit.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

}  // namespace

bool PagedTextBody::layOut(GfxRenderer& renderer, std::string source, const bool html, const size_t maxStyledBytes,
                           const int bodyFontId, const int width, const int height,
                           const LanguageHyphenator* hyphenator) {
  text = std::move(source);
  fontId = bodyFontId;
  boxHeight = height;
  pages.clear();
  lines.clear();
  totalPages = 1;
  linesPerPage = 1;

  // Styled path: lay the HTML out through the EPUB chapter parser into reader-identical Pages. Frees `text` on success
  // (the page arenas own the words); any failure leaves it for the plain-text path.
  if (html && text.size() <= maxStyledBytes && width > 0 && height > 0 &&
      buildDictionaryHtmlPages(renderer, text, fontId, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                               hyphenator, pages)) {
    text.clear();
    text.shrink_to_fit();
    totalPages = static_cast<int>(pages.size());
    return true;
  }

  htmlToPlainTextInPlace(text);
  if (wrapText(renderer, width, height)) return true;
  // Say so rather than show part of the text as if it were all of it. Nothing draws either buffer now, so both go back
  // to the heap.
  std::vector<Line>().swap(lines);
  text.clear();
  text.shrink_to_fit();
  return false;
}

int PagedTextBody::measureSpan(const GfxRenderer& renderer, const char* span, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, span, len);
  buf[len] = '\0';
  return renderer.getTextWidth(fontId, buf);
}

// Grows the line table to `capacity` only when the heap has the block for it.
// A definition runs to 64 KB, and its table to a quarter of that, so a failed
// growth -- an abort with -fno-exceptions -- is a real outcome on a heap the
// text itself has just drawn down.
//
// Ported from crosspoint-reader PR #3988 (Uri Tauber), which checks each growth
// of its line table the same way; it is kept a std::vector here, guarded by the
// shared heapHasBlockFor().
bool PagedTextBody::reserveLines(const size_t capacity) {
  if (capacity <= lines.capacity()) return true;
  if (!heapHasBlockFor(capacity * sizeof(Line))) {
    LOG_ERR("PTB", "No %u-byte block for the text's line table (%u lines)",
            static_cast<unsigned>(capacity * sizeof(Line)), static_cast<unsigned>(capacity));
    return false;
  }
  lines.reserve(capacity);
  return true;
}

// Greedy word-wrap of `text` into byte spans. '\n' breaks lines (blank lines
// survive as paragraph spacing); '\r' is dropped by treating it as a space at a
// token edge.
//
// False when the line table could not grow.
bool PagedTextBody::wrapText(const GfxRenderer& renderer, const int width, const int height) {
  lines.clear();
  // The size-based estimate (about one line per 32 bytes) first: one block and
  // no regrowth for ordinary prose. When even that block is missing, a small
  // start still serves a text that turns out to need fewer lines, and growth
  // below is checked the same way.
  constexpr size_t MIN_LINE_CAPACITY = 64;
  if (!reserveLines(text.size() / 32 + 8) && !reserveLines(MIN_LINE_CAPACITY)) return false;

  const int maxWidth = width;
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, height / lineHeight);

  const char* src = text.c_str();
  const auto n = static_cast<uint32_t>(text.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    // Half again rather than double: the smaller block is the likelier find on
    // the fragmented heap this guards against.
    if (lines.size() == lines.capacity() && !reserveLines(lines.capacity() + lines.capacity() / 2)) return false;
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
    return true;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = src[i];
    if (c == '\n' || c == '\0') {
      if (!flushLine(i + 1)) return false;
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && src[i] != ' ' && src[i] != '\t' && src[i] != '\r' && src[i] != '\n' && src[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A natural
    // stop lands on whitespace or the terminating NUL, never on a continuation
    // byte, so this is a no-op there.
    while (i - tokenStart > 1 && (src[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(renderer, src + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      if (!flushLine(tokenStart)) return false;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line (any
    // previous content was flushed above): split it at the widest fitting UTF-8
    // boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (src[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(renderer, src + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (src[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      if (!flushLine(rest)) return false;
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(renderer, src + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart && !flushLine(n)) return false;

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  return true;
}

int PagedTextBody::usedHeight(const GfxRenderer& renderer, const int page) const {
  const int lineHeight = renderer.getLineHeight(fontId);
  if (!pages.empty()) {
    if (page < 0 || page >= totalPages) return 0;
    // A line's yPos is its top; one line height below the lowest is where its text ends.
    int bottom = 0;
    for (const auto& element : pages[page]->elements) {
      if (element) bottom = std::max(bottom, element->yPos + lineHeight);
    }
    return std::min(bottom, boxHeight);
  }
  const int firstLine = page * linesPerPage;
  const int shown = std::min(linesPerPage, static_cast<int>(lines.size()) - firstLine);
  return std::max(0, shown) * lineHeight;
}

// Draws page `page`: a styled Page when the HTML layout succeeded, otherwise the
// wrapped line spans (copied into a stack buffer for NUL termination).
void PagedTextBody::draw(GfxRenderer& renderer, const int page, const int x, const int y) const {
  if (page < 0 || page >= totalPages) return;
  if (!pages.empty()) {
    pages[page]->render(renderer, fontId, x, y);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = page * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, text.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, y + (i - firstLine) * lineHeight, buf);
  }
}
