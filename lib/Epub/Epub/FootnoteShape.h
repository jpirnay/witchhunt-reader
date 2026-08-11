#pragma once

#include <cstddef>

// Shared "does this link reference a footnote?" test.
//
// Two very different code paths need the SAME answer and must not drift:
//   - FootnotePreviews::gather decides which links to collect previews for.
//   - ChapterHtmlSlimParser decides whether to latch sawFootnote_, the signal the reader
//     uses to trigger that gather mid-build.
// When the trigger was broader than the gather, a Calibre book whose spine 1 is an HTML
// table of contents ran the whole-book two-pass scan (~2.8 s behind a popup) on open and
// produced an empty cache: 20 chapter links looked like footnotes to the trigger and like
// nothing to the gatherer.
namespace FootnoteShape {

// True when a whitespace-separated attribute token list contains an exact token
// (`epub:type="noteref backlink"` contains "noteref", `"noterefs"` does not).
bool hasAttributeToken(const char* value, const char* token);

// True when the link carries EPUB 3 / ARIA note-reference semantics.
bool isNoterefTagged(const char* epubType, const char* role);

// True when the trimmed link text looks like a footnote marker: 1-4 codepoints, each one a
// digit, an enclosed number, a Han numeral, a reference symbol, or wrapping punctuation.
// Catches "*", "12", "[3]", "†" and their non-Latin equivalents — "٣" (Arabic-Indic), "¹"
// (superscript), "①", "１", "※", "【二】" — while a word link ("see chapter 2", "Chapter One",
// "Глава Один", "第一章") never qualifies. Leading and trailing space codepoints are trimmed,
// including NBSP and the CJK ideographic space; interior spaces reject.
//
// The set is deliberately generous in the non-Latin direction: a false positive costs one
// wasted gather, while a false negative costs a book its footnote previews permanently.
// It is NOT exhaustive over Unicode — a bare Han numeral ("一") is also a plausible chapter
// title, so a table of contents written that way still triggers a gather.
bool isMarkerText(const char* text, size_t len);

}  // namespace FootnoteShape
