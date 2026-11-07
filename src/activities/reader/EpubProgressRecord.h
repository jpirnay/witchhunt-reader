#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

// The EPUB reader's progress.bin record, little-endian:
//   spine(2) page(2) pageCount(2) percent(1) [paragraph(2) [chapterPage(2) chapterTotal(2)
//   printedPageLength(1) printedPage(n) [visibleOffset(4)]]]
//
// The paragraph is the page's entry in the section's paragraph LUT (the same index KOReader sync
// anchors on). It is written when the reader closes a book, so a book reopened under a different
// layout -- settings changed outside the book, or a firmware whose section layout version moved --
// lands on the paragraph it was left at instead of a page number rescaled by page count.
//
// The tail after it (Shown) is what the reader's status bar showed for that page. It is written on
// the same close, for readers of the file that cannot lay the book out themselves: the sleep
// screen's overlay would otherwise count pages per spine item and look up the printed page in the
// section cache on its own, and disagree with the screen the user just left.
//
// The visible offset is where the page starts in the chapter's text (VisibleText.h), from the same
// close. It says where the book was left under any layout, down to the page, and every chapter has
// one, where a Calibre book's <body><div><p> counts no paragraphs.
//
// Older files are shorter, and every other reader of the file (home progress badge, older
// firmware) reads a fixed prefix, so the optional tail is invisible to them.
struct EpubProgressRecord {
  // A printed-page label longer than this is left out rather than cut mid-label.
  static constexpr size_t kMaxPrintedPage = 15;

  struct Shown {
    uint16_t chapterPage = 0;   // 1-based page within the whole TOC chapter (#325, ChapterPageSpan)
    uint16_t chapterTotal = 0;  // pages in that chapter, possibly partly estimated; 0: unknown
    std::string printedPage;    // as the status bar shows it, e.g. "(42)" or "(7/8)"; empty: none
  };

  int spineIndex = 0;
  int page = 0;
  int pageCount = 0;  // 0: unknown (written mid-build or by a sync restore)
  uint8_t percent = 0;
  std::optional<uint16_t> paragraph;
  std::optional<Shown> shown;
  std::optional<uint32_t> visibleOffset;

  static constexpr size_t kMaxSize = 14 + kMaxPrintedPage + 4;

  // Returns the number of bytes written to `out`: 7 or 9 as before without `shown` or an offset,
  // otherwise the full tail, with the paragraph slot written as 0 (the LUT's "none") when there is
  // no paragraph, and an offset written behind an empty `shown` if there is none.
  size_t encode(uint8_t (&out)[kMaxSize]) const {
    put16(out, spineIndex);
    put16(out + 2, page);
    put16(out + 4, pageCount);
    out[6] = percent;
    const uint16_t anchor = paragraph ? *paragraph : 0;
    if (!shown && !visibleOffset) {
      if (anchor == 0) return 7;
      put16(out + 7, anchor);
      return 9;
    }
    const Shown tail = shown.value_or(Shown{});
    put16(out + 7, anchor);
    put16(out + 9, tail.chapterPage);
    put16(out + 11, tail.chapterTotal);
    const size_t labelLength = tail.printedPage.size() <= kMaxPrintedPage ? tail.printedPage.size() : 0;
    out[13] = static_cast<uint8_t>(labelLength);
    memcpy(out + 14, tail.printedPage.data(), labelLength);
    const size_t end = 14 + labelLength;
    if (!visibleOffset) return end;
    put32(out + end, *visibleOffset);
    return end + 4;
  }

  // Accepts every size the reader has ever written: 4 (spine, page), 6 (+ pageCount),
  // 7 (+ percent), 9 (+ paragraph), 14+ (+ shown) and 18+ (+ visibleOffset). A 5-byte file is a torn write and is
  // rejected, as it always was. A record torn anywhere after the percent keeps what came before the
  // tear: an 8-byte one its first 7, a cut inside `shown` everything but `shown`.
  static std::optional<EpubProgressRecord> decode(const uint8_t* data, const size_t size) {
    if (size < 4 || size == 5) return std::nullopt;
    EpubProgressRecord r;
    r.spineIndex = get16(data);
    r.page = get16(data + 2);
    if (size >= 6) r.pageCount = get16(data + 4);
    if (size >= 7) r.percent = data[6];
    // 0 is "no paragraph opened yet" in the LUT, never an anchor (Section::getParagraphIndexForPage).
    if (size >= 9 && get16(data + 7) != 0) r.paragraph = get16(data + 7);
    if (size >= 14) {
      const size_t labelLength = data[13];
      if (labelLength <= kMaxPrintedPage && size >= 14 + labelLength) {
        Shown s;
        s.chapterPage = get16(data + 9);
        s.chapterTotal = get16(data + 11);
        s.printedPage.assign(reinterpret_cast<const char*>(data + 14), labelLength);
        r.shown = std::move(s);
        if (size >= 14 + labelLength + 4) r.visibleOffset = get32(data + 14 + labelLength);
      }
    }
    return r;
  }

 private:
  static void put16(uint8_t* out, const int value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  }
  static uint16_t get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }
  static void put32(uint8_t* out, const uint32_t value) {
    put16(out, static_cast<int>(value & 0xFFFF));
    put16(out + 2, static_cast<int>(value >> 16));
  }
  static uint32_t get32(const uint8_t* in) {
    return static_cast<uint32_t>(get16(in)) | (static_cast<uint32_t>(get16(in + 2)) << 16);
  }
};
