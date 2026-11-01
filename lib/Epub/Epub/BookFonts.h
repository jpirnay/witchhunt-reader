#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class BuildArena;
class Epub;

/// A book's embedded fonts (the Publisher Fonts setting), made ready for the TTF engine: found once per book from
/// its CSS and a sample of its spine's text, and copied out of the EPUB as plain TrueType or CFF files to
/// `<book cache>/fonts/`, where they load like a `.ttf` family on the card.
///
/// The body text's family, and up to kMaxAccents accent families, ranked by the text set in them (headings, a
/// dedication, a typed letter, words inside a paragraph). The glyph store holds them together, or the accent families
/// beside a TrueType reader
/// font (TTF spec, section 11.1). What is refused, so the book keeps the reader's font (or, for an accent family,
/// the next one takes its place): a family that is generic or not embedded; WOFF, WOFF2 and font collections;
/// fonts encrypted rather than obfuscated; faces over kMaxFaceBytes. TrueType and CFF (`OTTO`) faces are taken.
namespace BookFonts {

/// Bump when what prepare() decides or writes changes: every book is examined again.
/// v2: the state file holds a Family's name to its end, without a length byte.
/// v3: the faces sit in fonts/ itself as <style>.ttf, not in a folder named after the family; the state file is
/// fonts.bin beside it.
/// v4: the state file also holds the body's font-family and the accent family (fonts/accent/), and a book without
/// @font-face rules is probed too, for its body's generic family.
/// v5: up to kMaxAccents accent families, in fonts/accent<n>/.
/// v6: CFF faces are copied too (they were refused), so a book examined as having none is examined again.
constexpr uint8_t kVersion = 6;
/// Accent families copied at most: the glyph store's eight style blocks hold a body of four styles and three
/// one-style families, and its face table (32 faces) a book's body sizes and a few of each.
constexpr uint8_t kMaxAccents = 3;
/// A face bigger than this is not copied. Not for the font area: a face too large for flash mode is drawn in SD mode
/// (its tables in flash, its outlines read from the copy on the card), so the size left to bound is the copy itself,
/// extracted from the book once, behind the open's popup, and kept on the card. A full CJK face, as books embed them,
/// is 8-20 MB; one over this is taken for an error rather than minutes of extraction.
constexpr size_t kMaxFaceBytes = 24 * 1024 * 1024;
/// prepare() reads the head of every spine item, from the first, or of kProbeMaxItems spread evenly over a longer
/// spine: about kProbeBudgetBytes in all, each head kProbeBudgetBytes over the items read, between
/// kProbeMinItemBytes and kProbeMaxItemBytes. What a head holds stands for its whole item, and the items read for the
/// spine: a book's front matter, the first few items, is no sample of its text.
constexpr size_t kProbeBudgetBytes = 512 * 1024;
constexpr int kProbeMaxItems = 64;
constexpr size_t kProbeMinItemBytes = 8 * 1024;
constexpr size_t kProbeMaxItemBytes = 64 * 1024;
/// A block (p, div, li, dd, blockquote, pre, h1-h6) whose text is in one family counts as this much text at least: a
/// heading at every chapter's start is not out-ranked by a few lines of inline text.
constexpr uint32_t kBlockMinBytes = 64;
/// An accent family with less text than kMinFamilyBytes and than kMinFamilyPerMille of the book's (as the probe
/// estimates them) takes no slot, although one is free: each family loaded costs the reader 2-3 KB of heap. Both, so a
/// short book keeps its small families.
constexpr uint32_t kMinFamilyBytes = 256;
constexpr uint32_t kMinFamilyPerMille = 1;

enum class State : uint8_t {
  Pending,  ///< not examined yet (or by another kVersion), or the last attempts failed for a passing reason
  None,     ///< examined: the book's body keeps the reader's font
  Family,   ///< examined: its body family is on the card, in familyDir()
};

/// The families prepare() found, besides the body family's State.
struct Families {
  uint32_t body = 0;  ///< the body text's font-family (CssStyle::fontFamily); 0 for the reader's font
  /// The accent families' identities (CssFontFamily), the most used first, each in familyDir(, n); 0 past the last.
  uint32_t accents[kMaxAccents] = {};
};

/// What prepare() found for the book whose cache is at `cachePath`. `*name` gets the name the book declares for
/// its body family (when the State is Family, else ""), or with `accent` 0..kMaxAccents-1 for that accent family
/// ("" for none).
State read(const std::string& cachePath, std::string* name = nullptr, Families* families = nullptr, int accent = -1);
/// Where a Family's files are: `<cachePath>/fonts`, one `<style>.ttf` per style (EpdFontFamily numbering); accent
/// family n's, named the same, in `<cachePath>/fonts/accent<n>`. The state file is `<cachePath>/fonts.bin`.
std::string familyDir(const std::string& cachePath, int accent = -1);

/// The reader's memory ladder (ImageMemoryLadder) leaves the book's least used accent families out when a page and its
/// images do not fit beside them: the accent families it keeps, from then on, while the font settings stay those it
/// was found under (`settingsKey`, the caller's hash of them). kMaxAccents when it keeps them all: none was set, or
/// it was set under other settings, which forgets it. In `<cachePath>/fontlimit.bin`.
uint8_t accentLimit(const std::string& cachePath, uint32_t settingsKey);
/// Keeps `accents` (kMaxAccents: all, which forgets the limit).
void setAccentLimit(const std::string& cachePath, uint32_t settingsKey, uint8_t accents);

/// Finds the book's body and accent families and copies their font files to the card. `scratch`, when set, hosts
/// the inflate ring and the parser state (the lent framebuffer at book open); otherwise the heap. The CSS must be
/// loaded (Epub::load without skipping it). Writes the state file: Family or None, or, when a read failed, another
/// failed attempt, so a passing shortage is retried at the next open and a broken book is not retried for ever.
/// Returns the state the next read() gives.
State prepare(const Epub& epub, BuildArena* scratch);

// --- Exposed for tests ---

struct Found {
  /// The body family: the family most of the text is set in (CssStyle::fontFamily; families compare by identity),
  /// an inline accent's text counting for its paragraph's, over the spine items' heads prepare() reads, each weighted
  /// to the book (probeWeight256). 0 for the reader's font.
  uint32_t body = 0;
  /// The accent families' identities: the families text is set in, inline text included, other than the body's,
  /// among those `records` (CssParser::loadFontFaces) has a face of, the one with the most text first; 0 past the last.
  /// A block whose text is in one family counts as kBlockMinBytes of it at least. A family with negligible text
  /// (kMinFamilyBytes, kMinFamilyPerMille) is left out.
  uint32_t accents[kMaxAccents] = {};
  /// The body family's text and each accent family's, by style (EpdFontFamily numbering: bit 0 bold, bit 1 italic),
  /// as the accents are ranked by it.
  uint32_t bodyStyles[4] = {};
  uint32_t accentStyles[kMaxAccents][4] = {};
  uint16_t itemsRead = 0;  ///< the spine items read (one that cannot be read is passed over)
  uint32_t bytesRead = 0;  ///< the bytes of them parsed
};
/// `sum` plus `bytes` times `weight256` (8 fractional bits: 256 is 1.0), rounded; at most UINT32_MAX.
uint32_t addWeighted(uint32_t sum, uint32_t bytes, uint32_t weight256);
/// The weight of what was read of a spine item, in 8 fractional bits: the item's size over the bytes read of it (1
/// when its size is not known), times the spine's items over those read. At most UINT32_MAX.
uint32_t probeWeight256(size_t itemBytes, size_t readBytes, int spineCount, int sampledCount);
/// `*failed` is set when no spine item could be read, or the probe had not the memory to decide.
Found detectFamilies(const Epub& epub, BuildArena* scratch, bool* failed, std::string_view records = {});

/// The IDPF obfuscation key (EPUB OCF): the SHA-1 of the package's unique identifier with its XML white space
/// removed.
bool idpfKey(std::string uniqueIdentifier, uint8_t (&key)[20]);
/// Adobe's: the 16 bytes of the book's `urn:uuid:` identifier. False when it is not one.
bool adobeKey(std::string_view uuid, uint8_t (&key)[16]);

}  // namespace BookFonts
