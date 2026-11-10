#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class BuildArena;
class Epub;
class GfxRenderer;

/// The size of a book's running text, for Normalize Font Size: a book whose CSS sets all its text smaller or larger
/// than medium (`.tx { font-size: small }` on every paragraph) is read at the reader's size when its sizes are taken
/// relative to this one (ChapterHtmlSlimParser::setBodySizePct).
///
/// Found once per book, after its CSS is compiled: the chapter parser styles the book's largest spine item without
/// laying anything out (ChapterHtmlSlimParser::setSizeCensus) and counts the characters of its running text by the
/// size they would be set at; the size with the most is the body size. Kept in `<book cache>/bodysize.bin`, forgotten
/// when the book's CSS is compiled again (Epub::parseCssFiles).
namespace BodySize {

/// Bump when what detect() finds changes: every book is measured again.
constexpr uint8_t kVersion = 1;
/// A measurement that failed is tried again at the next open, this many times in all; then the book keeps its sizes.
constexpr uint8_t kMaxAttempts = 3;
/// At most this much of the item is read: the head of a long one stands for it. An X3 styles about 100 KB a second.
constexpr size_t kMaxReadBytes = 96 * 1024;
/// Less running text than this decides nothing: the book keeps its sizes.
constexpr uint32_t kMinChars = 256;
/// A body size this close to 100 % is 100 %: the band Normalize Font Size snaps near-body sizes in already.
constexpr int kNearBodyPct = 10;

/// The characters of running text (not in h1-h6) by the size they are set at, in percent of the reader's size.
struct Census {
  static constexpr int kBuckets = 256;  // the last one holds every size from 255 % up
  uint32_t chars[kBuckets] = {};
  uint32_t total = 0;
  void add(int pct, uint32_t n) {
    const int bucket = pct < 0 ? 0 : (pct >= kBuckets ? kBuckets - 1 : pct);
    chars[bucket] += n;
    total += n;
  }
};

/// The body size `census` gives: the size with the most characters (the smaller on a tie); 100 within kNearBodyPct of
/// 100, with less than kMinChars of text, or outside the sizes a word can take (ParsedText's MIN/MAX_WORD_SIZE_PCT).
uint8_t bodySizeOf(const Census& census);

/// The book's body size in percent, as measured; 0 when it has not been (or by another kVersion), or its last
/// measurement failed and attempts remain.
uint8_t read(const std::string& cachePath);
/// Forgets the measurement: the next open measures the book again.
void forget(const std::string& cachePath);

/// One measurement, written nowhere.
struct Detected {
  uint8_t pct = 100;       // the body size (bodySizeOf)
  int spineIndex = -1;     // the item measured
  uint32_t chars = 0;      // its characters of running text
  uint32_t bodyChars = 0;  // of those, the ones at the size with the most (topPct)
  int topPct = 100;        // that size, before kNearBodyPct and the range made it 100
  size_t bytesRead = 0;    // of the item, inflated
  uint32_t ms = 0;
  bool failed = false;  // no item read, or the parser or the CSS had not the memory: no answer
};
/// Measures the book. Its CSS must be compiled (Epub::load without skipping it). `renderer` and `fontId` serve the
/// parser's metrics only, which no size depends on: any loaded font will do. `scratch`, when set, hosts the CSS rules,
/// the parser state and the inflate ring (the lent framebuffer at book open); otherwise the heap.
Detected detect(const Epub& epub, GfxRenderer& renderer, int fontId, BuildArena* scratch);
/// Measures the book, unless its CSS is not compiled, and writes what was found, or another failed attempt. Returns
/// what read() gives after it.
uint8_t prepare(const Epub& epub, GfxRenderer& renderer, int fontId, BuildArena* scratch);

}  // namespace BodySize
