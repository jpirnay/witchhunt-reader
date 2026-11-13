// The shaping rules, checked on every page the host pipeline lays out for the shaped books: the four Indic ones and the
// Myanmar one (test/epubs/make_test_shaped_indic.py), the Arabic, Persian and Urdu ones and the Hebrew one with Hindi
// words (make_test_shaped_arabic.py), each with its font (test/fixtures/fonts) as the face of every font and style, the
// Arabic one again 1,600 px wide, where its lines take the line pass's fallback, and the Myanmar one 1,600 px wide,
// where more of its phrases stay whole.
//
// A line with a logical order (a bidi line) stores its words in visual order: each rule reads them in the order it is
// about. Glue (wordContinues) is between visual neighbours; reading order is TextBlock::logicalPosition, and a token is
// what the book's text says it is. So every page line and table cell line is read back against the chapter's text
// first: its words, in reading order, must reproduce it (rule 6), and that reading gives each word its place in the
// text, the space before it, and the tokens around it.
//  1. a glyph form sits only on a word whose text holds an Arabic letter, U+0900-0DFF or a Myanmar code point, and
//     never on a digit (bidi class EN or AN), a Bidi_Mirrored code point or a bidi control: bidi orders those, a glyph
//     run does not;
//  2. a shaped word is as wide as it was measured: a word glued to a word with a glyph form (or one with a glyph form
//     glued to the word before it) starts where that word ends, the run's advance or a piece's text width, and a word
//     with a glyph form ends at or before the spaced word after it (both in stored order: between visual neighbours);
//  3. no line starts, in reading order, with an Arabic mark, a dependent sign, a Myanmar sign, a ZWJ or a ZWNJ, no
//     line that ends inside a word (before the hyphen of a word split across lines) ends its Indic text with a virama,
//     and a word split across lines without a hyphen is cut where Myanmar's rule allows it (myanmar::cutAllowed);
//  4. each glyph form is what IndicShaper::shapeToken makes of its word's text, at its drawn size, in the book's
//     language, with the glued tokens kept apart from its token (another style or size, or a token a ligature took) as
//     context, whole, where the word is its token's edge: one Glyphs piece over the text, the stored records, any other
//     piece a joiner or control the line pass absorbed into it;
//  5. every word is drawn inside the page, to the pixel: the host stub has no kerning, so the breaker's measure is the
//     draw's;
//  6. the pieces cover the text: the words of each line, in reading order, are the chapter's text, but for its spaces,
//     its bidi controls, the hyphen a split adds and the tokens the book lists as consumed, a ligature in the token
//     before them drawing their letters; and no two glued glyph words of one style are one run;
//  7. each letter is drawn once: a word's glyph forms hold the glyphs the whole word gets in one shaping call, as a
//     multiset: a ligature across a style glue is formed alike by both tokens, drawn once, and the other token keeps
//     nothing;
//  8. an Arabic token reads right to left: of two of its words in reading order on a right-to-left line, the later
//     lies to the left.
// And a line without Indic or Arabic text keeps its bytes: the same with the book's face as without one. And the
// build's word cache changes no line: a long chapter on a lent arena whose tail the parse overwrites dumps the same
// without it. Each book's counts are printed against floors: glyph words, merged words (the cases' tokens that merge),
// kept-apart pairs, glyph words glued to punctuation or digits, words past one shaping call's code points, words split
// across lines, consumed tokens, right-to-left pairs, fallback lines with glyph forms and lines past the logical
// order's 255 pieces. No section of the books is laid out degraded.
//
// Every rule walks every page line and every table cell line. forEachPage frees each page when its callback returns,
// so a book is read into copies first, and the rules run on those.
#include <BidiUtils.h>
#include <BufferedFileIO.h>
#include <GlyphRun.h>
#include <OtShaper.h>
#include <ShapingFace.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "ArabicScripts.h"
#include "Epub.h"
#include "Epub/BidiLine.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/WordShapeCache.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "IndicScripts.h"
#include "IndicShaper.h"
#include "LogObserver.h"
#include "MyanmarScript.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"
#include "TextShaping.h"

// minibidi's UAX #9 classes and its mirror table (rule 1). Last: its class names are bare enumerators.
extern "C" {
#include "minibidi.h"
}

namespace fs = std::filesystem;

namespace {

struct Book {
  const char* name;      // the corpus book
  const char* label;     // the case's name
  const char* font;      // its font, in test/fixtures/fonts
  const char* language;  // its dc:language
  uint16_t viewportWidth = 460;
  std::vector<std::string> mergedWords;     // the words the cases' tokens merge into, each once in the book
  std::vector<std::string> consumedTokens;  // tokens a glued neighbour's ligature draws: no word of their own
  int minGlyphWords = 0;
  int minKeptApart = 0;
  int minGluedToPunctuation = 0;
  int minPastTheCap = 0;
  int minSplitWords = 0;
  int minUnhyphenatedSplits = 0;  // words split across lines without a hyphen (Myanmar)
  int minLatinLines = 2;          // lines of blocks without shaped letters, which must keep their bytes
  int consumed = 0;               // the tokens a glued neighbour's ligature draws, exactly
  int minBesideDigits = 0;
  int minRtlPairs = 0;
  int minFallbackLines = 0;
  int minPastTheGuard = 0;
  // Rule 7's words drawn in part as their code points, exactly: a token's leading marks, a run the line pass could not
  // keep whole. A letter a ligature took, drawn again as its code points, would add one.
  int codePointWords = 0;
};

void PrintTo(const Book& b, std::ostream* os) { *os << b.label; }

constexpr int kFontId = 1;  // the harness profile's body font
constexpr uint32_t kZwnj = 0x200C;
constexpr uint32_t kZwj = 0x200D;
// TheWordCacheChangesNoLine's chapter: chapter 2 this many times over.
constexpr int kLongChapterRepeats = 16;
// A logical order numbers a line's words in a byte (TextBlock): a line of more pieces keeps none.
constexpr size_t kMaxOrderedPieces = 255;
pipeline_harness::Profile profileOf(const Book& book) {
  pipeline_harness::Profile p;
  p.viewportWidth = book.viewportWidth;
  p.viewportHeight = 760;
  p.shapingFontFile = book.font;
  return p;
}

std::string corpusBook(const char* name) { return (fs::path(CORPUS_DIR) / (std::string(name) + ".epub")).string(); }

// Calls visit(block, x) for every text line of the page: each page line at its element's x, each table cell line at
// its cell's, as PageTableFragment::render places them.
template <typename Visit>
void forEachLine(const Page& page, Visit&& visit) {
  for (const auto& element : page.elements) {
    if (element->getTag() == TAG_PageLine) {
      visit(*static_cast<const PageLine&>(*element).getBlock(), static_cast<int>(element->xPos));
      continue;
    }
    if (element->getTag() != TAG_PageTable) continue;
    const auto& table = static_cast<const PageTableFragment&>(*element);
    const uint8_t columns = table.getColumnCount();
    for (const auto& row : table.getRows()) {
      uint8_t col = 0;
      for (const auto& cell : row.cells) {
        if (col >= columns) break;
        const int cellX = element->xPos +
                          static_cast<int>(static_cast<uint32_t>(table.getTotalWidth()) * col / columns) +
                          TABLE_CELL_PADDING;
        for (const auto& line : cell.lines) visit(*line, cellX);
        col = static_cast<uint8_t>(col + std::min<uint8_t>(cell.colSpan ? cell.colSpan : 1, columns - col));
      }
    }
  }
}

std::vector<uint32_t> codepointsOf(const std::string& text) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*p) out.push_back(utf8NextCodepoint(&p));
  return out;
}

bool isIndic(const uint32_t cp) { return cp >= 0x0900 && cp <= 0x0DFF; }

// An Arabic run letter: a member that is not a mark.
bool isArabicLetter(const uint32_t cp) { return arabic::isRunMember(cp) && !arabic::isMark(cp); }

bool holdsIndic(const std::string& text) {
  const std::vector<uint32_t> cps = codepointsOf(text);
  return std::any_of(cps.begin(), cps.end(), isIndic);
}

bool holdsArabicLetter(const std::string& text) {
  const std::vector<uint32_t> cps = codepointsOf(text);
  return std::any_of(cps.begin(), cps.end(), isArabicLetter);
}

bool holdsMyanmar(const std::string& text) {
  const std::vector<uint32_t> cps = codepointsOf(text);
  return std::any_of(cps.begin(), cps.end(), myanmar::isRunMember);
}

// Text a font that shapes draws differently: Indic, an Arabic letter, or Myanmar.
bool holdsShapedLetter(const std::string& text) {
  return holdsIndic(text) || holdsArabicLetter(text) || holdsMyanmar(text);
}

// What a run of the word shaped whole would hold, drawn as its code points instead: an Arabic or Myanmar run member, or
// an Indic code point (a danda a token of its own draws as its code point is part of the run when the word is one
// token).
bool drawsShapedScript(const uint32_t cp) { return arabic::isRunMember(cp) || isIndic(cp) || myanmar::isRunMember(cp); }

// A Myanmar sign: a run member that starts no syllable, but for the section marks (U+104A, 104B), which a space may
// come before.
bool isMyanmarSign(const uint32_t cp) {
  return myanmar::isRunMember(cp) && !myanmar::startsSyllable(cp) && cp != 0x104A && cp != 0x104B;
}

// Bidi orders these; a glyph run must never hold one (rule 1).
bool isDigit(const uint32_t cp) {
  const uchar cls = bidi_class(cp);
  return cls == EN || cls == AN;
}
bool isMirrored(const uint32_t cp) { return mirror(cp) != cp; }

bool isAsciiDigitOrArabicDigit(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9);
}

// Punctuation alone: ASCII other than letters and digits, General Punctuation (dashes, quotes), or the Arabic comma.
bool isPunctuation(const std::string& text) {
  const std::vector<uint32_t> cps = codepointsOf(text);
  return !cps.empty() && std::all_of(cps.begin(), cps.end(), [](const uint32_t cp) {
    const bool ascii =
        cp > 0x20 && cp < 0x7F && !(cp >= '0' && cp <= '9') && !(cp >= 'A' && cp <= 'Z') && !(cp >= 'a' && cp <= 'z');
    return ascii || (cp >= 0x2010 && cp <= 0x205E) || cp == 0x060C;
  });
}

// `text` without the bidi controls and soft hyphens no line draws: the text the book's is compared with.
std::string withoutControls(const std::string& text) {
  std::string out;
  for (const uint32_t cp : codepointsOf(text)) {
    if (!BidiLine::isControl(cp) && cp != 0x00AD) utf8AppendCodepoint(out, cp);
  }
  return out;
}

struct Glyph {
  uint16_t glyph;
  int16_t advance;
  int8_t dx;
  int8_t dy;
  bool operator==(const Glyph& o) const { return glyph == o.glyph && advance == o.advance && dx == o.dx && dy == o.dy; }
};

std::ostream& operator<<(std::ostream& os, const Glyph& g) {
  return os << g.glyph << ":" << g.advance << ":" << static_cast<int>(g.dx) << "," << static_cast<int>(g.dy);
}

std::vector<Glyph> recordsOf(const GlyphRun& run) {
  std::vector<Glyph> out;
  forEachGlyph(run, 0, false, [&out](const GlyphRecord& r, int) { out.push_back({r.glyph, r.advance, r.dx, r.dy}); });
  return out;
}

struct Piece {
  IndicShaper::PieceKind kind;
  uint16_t begin;
  uint16_t end;
  std::vector<Glyph> glyphs;
};

class RecordingSink final : public IndicShaper::PieceSink {
 public:
  std::vector<Piece> pieces;
  bool beginPiece(const IndicShaper::PieceKind kind, const uint16_t begin, const uint16_t end) override {
    pieces.push_back({kind, begin, end, {}});
    return true;
  }
  bool addGlyph(const GlyphRecord& r) override {
    if (pieces.empty()) return false;
    pieces.back().glyphs.push_back({r.glyph, r.advance, r.dx, r.dy});
    return true;
  }
  bool addBytes(const char*, size_t) override { return true; }
};

// --- A book read back: its lines as copies, each word placed in its chapter's text ---

struct Word {
  std::string text;   // as the line holds it
  std::string plain;  // without the bidi controls (withoutControls)
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  uint8_t sizePct = 100;
  int32_t font = 0;  // its word font's ID, 0 for the block's
  float fontScale = 1.0f;
  bool continues = false;
  bool glyphs = false;
  std::vector<Glyph> records;
  uint8_t logical = 0;
  TextBlock::WordBox box;
  uint32_t ppem26_6 = 0;  // the size its glyph form is drawn at: the face's ppem times its scale
};

// Where a word is in its chapter's text (rule 6).
struct Place {
  int block = -1;
  size_t begin = 0;
  size_t end = 0;
  bool spaceBefore = false;  // the text has a space before it
  std::string gapBefore;     // a consumed token right before it, glued to the word before
  std::string gapAfter;      // consumed tokens right after it, at its line's end
  bool splitHyphen = false;  // the hyphen a split adds: the word "-", or the end of a word drawn as its code points
};

struct Line {
  int spine = 0;
  bool ordered = false;           // TextBlock::hasLogicalOrder
  std::vector<Word> words;        // in stored (visual) order
  std::vector<uint16_t> reading;  // the stored indices in reading order
  std::vector<Place> places;      // per stored word
  bool reversed = false;          // no logical order, but read right to left: more pieces than a logical order holds
  int gaps = 0;                   // consumed tokens placed on the line
};

// Whether two words are in one style, size and font: the pieces of one token always are, so glued words that differ
// were kept apart.
bool sameStyle(const Word& a, const Word& b) {
  return a.style == b.style && a.sizePct == b.sizePct && a.font == b.font && a.fontScale == b.fontScale;
}

std::string lineText(const Line& line) {
  std::string text;
  for (size_t k = 0; k < line.words.size(); ++k) {
    const uint16_t i = line.reading.empty() ? static_cast<uint16_t>(k) : line.reading[k];
    if (k > 0) text += ' ';
    text += line.words[i].text;
  }
  return text;
}

std::string where(const Line& line, const uint16_t i) {
  return "spine " + std::to_string(line.spine) + ", word " + std::to_string(i) + " '" + line.words[i].text +
         "' in: " + lineText(line);
}

// The chapter's text, block by block (a paragraph, a heading, a table cell): its body without the tags, every run of
// white space one space. The corpus books name their spine items OEBPS/ch<N>.xhtml, in spine order.
std::vector<std::string> chapterBlocks(const Book& book, const fs::path& work, const int spine) {
  Epub source(corpusBook(book.name), (work / "source").string());
  size_t size = 0;
  uint8_t* bytes = source.readItemContentsToBytes("OEBPS/ch" + std::to_string(spine + 1) + ".xhtml", &size, false);
  const std::string xhtml = bytes ? std::string(reinterpret_cast<const char*>(bytes), size) : std::string();
  free(bytes);
  std::vector<std::string> blocks;
  size_t at = xhtml.find("<body");
  if (at == std::string::npos) return blocks;
  at = xhtml.find('>', at) + 1;
  const size_t end = xhtml.find("</body>", at);
  static const char* const kBlockTags[] = {"p",  "h1", "h2", "h3", "h4", "h5", "h6",    "div",        "td",
                                           "th", "tr", "li", "ul", "ol", "br", "table", "blockquote", "aside"};
  std::string cur;
  const auto flush = [&] {
    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
    if (!cur.empty()) blocks.push_back(withoutControls(cur));
    cur.clear();
  };
  for (size_t i = at; i < end && i < xhtml.size();) {
    const char c = xhtml[i];
    if (c == '<') {
      const size_t close = xhtml.find('>', i);
      size_t n = i + 1;
      if (n < close && xhtml[n] == '/') ++n;
      size_t e = n;
      while (e < close && std::isalnum(static_cast<unsigned char>(xhtml[e]))) ++e;
      std::string name = xhtml.substr(n, e - n);
      for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (std::find_if(std::begin(kBlockTags), std::end(kBlockTags), [&](const char* tag) { return name == tag; }) !=
          std::end(kBlockTags)) {
        flush();
      }
      i = close + 1;
      continue;
    }
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      if (!cur.empty() && cur.back() != ' ') cur += ' ';
      ++i;
      continue;
    }
    if (c == '&') {
      const size_t semi = xhtml.find(';', i);
      const std::string entity = xhtml.substr(i + 1, semi - i - 1);
      if (entity == "amp") cur += '&';
      if (entity == "lt") cur += '<';
      if (entity == "gt") cur += '>';
      if (entity == "quot") cur += '"';
      if (!entity.empty() && entity[0] == '#') {
        const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
        utf8AppendCodepoint(
            cur, static_cast<uint32_t>(std::strtoul(entity.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10)));
      }
      i = semi + 1;
      continue;
    }
    cur += c;
    ++i;
  }
  flush();
  return blocks;
}

// Matches the line's words, in `order`, against `src` from `pos`, placing each. Returns where the line ends in `src`,
// or npos when its words are not the text there.
size_t matchLine(Line& line, const std::vector<uint16_t>& order, const std::string& src, size_t pos, const int block,
                 const std::vector<std::string>& consumed) {
  const auto gapAt = [&](const size_t at) -> const std::string* {
    for (const std::string& token : consumed) {
      if (src.compare(at, token.size(), token) == 0) return &token;
    }
    return nullptr;
  };
  // Where `text` starts from `at`, past one space; npos when it does not.
  const auto textAt = [&](size_t at, const std::string& text, bool* space) {
    *space = at < src.size() && src[at] == ' ';
    if (*space) ++at;
    return src.compare(at, text.size(), text) == 0 ? at : std::string::npos;
  };
  for (size_t k = 0; k < order.size(); ++k) {
    Place& place = line.places[order[k]];
    place = Place{};
    place.block = block;
    const std::string& text = line.words[order[k]].plain;
    if (text.empty()) {  // controls only
      place.begin = place.end = pos;
      continue;
    }
    bool space = false;
    size_t at = textAt(pos, text, &space);
    // A consumed token follows a glyph run, its ligature's (on the line before, for the line's first word).
    if (at == std::string::npos && (k == 0 || line.words[order[k - 1]].glyphs)) {
      if (const std::string* gap = gapAt(pos)) {
        at = textAt(pos + gap->size(), text, &space);
        // Glued to the word before it: past a space, the word's after it; else between the two (and the line's
        // first word's, its ligature on the line before).
        if (at != std::string::npos && space && k > 0) {
          line.places[order[k - 1]].gapAfter += *gap;
        } else if (at != std::string::npos) {
          place.gapBefore = *gap;
        }
      }
    }
    if (at == std::string::npos && k + 1 == order.size() && text.back() == '-') {
      // The hyphen a split adds, which the text does not hold there.
      const std::string head = text.substr(0, text.size() - 1);
      // The word "-" alone is glued to the word it ends.
      space = false;
      at = head.empty() ? pos : textAt(pos, head, &space);
      if (at != std::string::npos) {
        place.splitHyphen = true;
        place.spaceBefore = space && !head.empty();
        place.begin = at;
        place.end = at + head.size();
        pos = place.end;
        continue;
      }
    }
    if (at == std::string::npos) return std::string::npos;
    place.spaceBefore = space;
    place.begin = at;
    place.end = at + text.size();
    pos = place.end;
  }
  // A consumed token ending the line: its own line, holding nothing else, was not emitted, or it is the line's last.
  if (!order.empty() && line.words[order.back()].glyphs) {
    while (const std::string* gap = gapAt(pos)) {
      line.places[order.back()].gapAfter += *gap;
      pos += gap->size();
    }
  }
  return pos;
}

// The orders a line may read in: by its logical order; without one, as stored, else right to left (a line of more
// pieces than a logical order holds).
std::vector<std::vector<uint16_t>> readingOrders(const Line& line) {
  std::vector<uint16_t> stored(line.words.size());
  for (size_t i = 0; i < stored.size(); ++i) stored[i] = static_cast<uint16_t>(i);
  if (!line.ordered) return {stored, std::vector<uint16_t>(stored.rbegin(), stored.rend())};
  std::sort(stored.begin(), stored.end(),
            [&](const uint16_t a, const uint16_t b) { return line.words[a].logical < line.words[b].logical; });
  return {stored};
}

struct BookReading {
  bool built = false;
  bool degraded = false;
  std::vector<Line> lines;
  std::map<int, std::vector<std::string>> blocks;  // per spine
  struct Ref {
    size_t line;
    uint16_t word;
  };
  std::vector<Ref> stream;  // every word, line by line, each line in reading order
  std::vector<int> token;   // per stream word: its token's number
  std::vector<std::string> failures;

  const Word& word(const size_t s) const { return lines[stream[s].line].words[stream[s].word]; }
  const Place& place(const size_t s) const { return lines[stream[s].line].places[stream[s].word]; }
  const Line& lineOf(const size_t s) const { return lines[stream[s].line]; }
  // Whether stream word s follows word s - 1 in the text with nothing between (a consumed token aside).
  bool glued(const size_t s) const {
    if (s == 0) return false;
    const Place& a = place(s - 1);
    const Place& b = place(s);
    if (a.block != b.block || lineOf(s - 1).spine != lineOf(s).spine || b.spaceBefore || a.splitHyphen) return false;
    return a.end + a.gapAfter.size() + b.gapBefore.size() == b.begin;
  }
  // The consumed tokens between stream words s - 1 and s.
  std::string gapBetween(const size_t s) const { return place(s - 1).gapAfter + place(s).gapBefore; }
  std::string tokenText(const int t) const {
    std::string text;
    for (size_t s = 0; s < stream.size(); ++s) {
      if (token[s] == t) text += word(s).text;
    }
    return text;
  }
};

// Reads the book under `profile` into copies, and places every line's words in its chapter's text. With `serialized`,
// also each line's bytes as the section file holds them, line by line.
BookReading readBook(const Book& book, const pipeline_harness::Profile& profile, const fs::path& work,
                     const char* cache = "cache", std::vector<std::string>* serialized = nullptr) {
  BookReading out;
  out.built = pipeline_harness::forEachPage(
      corpusBook(book.name), (work / cache).string(), profile,
      [&](const int spine, const Section& section, const Page& page, GfxRenderer& r) {
        out.degraded = out.degraded || section.isShapingDegraded();
        forEachLine(page, [&](const TextBlock& block, const int x) {
          if (serialized != nullptr) {
            const fs::path lineFile = work / "line.bin";
            HalFile file;
            bool written = file.openForWrite(lineFile.string());
            if (written) {
              serialization::BufferedFileWriter writer(file);
              written = block.serialize(writer) && writer.flush();
            }
            file.close();
            std::ifstream in(lineFile, std::ios::binary);
            serialized->emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            EXPECT_TRUE(written && !serialized->back().empty());
          }
          Line line;
          line.spine = spine;
          line.ordered = block.hasLogicalOrder();
          for (uint16_t i = 0; i < block.wordCount(); ++i) {
            Word w;
            w.text.assign(block.wordText(i), block.wordTextLen(i));
            w.plain = withoutControls(w.text);
            w.style = block.wordStyle(i);
            w.sizePct = block.wordSizePct(i);
            if (const TextBlock::WordFont* font = block.wordFont(i)) {
              w.font = font->fontId;
              w.fontScale = font->scale;
            }
            w.continues = block.wordContinues(i);
            w.glyphs = block.hasGlyphForm(i);
            if (w.glyphs) w.records = recordsOf(block.glyphForm(i));
            w.logical = block.logicalPosition(i);
            w.box = block.wordBox(r, i, kFontId, x, 0);
            uint32_t facePpem = 0;
            r.shapingFace(w.box.fontId, static_cast<EpdFontFamily::Style>(w.box.style & EpdFontFamily::BOLD_ITALIC),
                          &facePpem);
            // Records are in drawn pixels: the word is shaped at the size it is drawn at.
            w.ppem26_6 = static_cast<uint32_t>(
                static_cast<float>(facePpem) * r.fontBaseScale(w.box.fontId) * w.box.scale + 0.5f);
            line.words.push_back(std::move(w));
          }
          line.places.resize(line.words.size());
          out.lines.push_back(std::move(line));
        });
        TextShaping::releaseBuildMemory();
      });
  if (!out.built) return out;

  // Each line from where the line before ended, else from wherever a block's unread text holds it (an RTL table
  // stores its cells right to left).
  std::map<int, std::vector<size_t>> read;  // per spine, per block: how far its text is read
  int cursor = 0;
  for (Line& line : out.lines) {
    if (out.blocks.count(line.spine) == 0) {
      out.blocks[line.spine] = chapterBlocks(book, work, line.spine);
      read[line.spine].assign(out.blocks[line.spine].size(), 0);
      cursor = 0;
    }
    const std::vector<std::string>& blocks = out.blocks[line.spine];
    std::vector<size_t>& done = read[line.spine];
    const std::vector<std::vector<uint16_t>> orders = readingOrders(line);
    // Whether the line after this one goes on in `src` from `at`: a line that ends inside a word is followed by the
    // rest of it.
    const auto nextGoesOn = [&](const std::string& src, const size_t at, const int b) {
      if (&line == &out.lines.back()) return false;
      Line next = *(&line + 1);
      for (const std::vector<uint16_t>& order : readingOrders(next)) {
        if (matchLine(next, order, src, at, b, book.consumedTokens) != std::string::npos) return true;
      }
      return false;
    };
    // Of the places that hold the line, the first that ends it where a line may end (at a space or the block's end,
    // after a consumed token or a split's hyphen), else one the next line goes on from (a glued word broken between
    // its tokens), else the first.
    bool placed = false;
    for (size_t o = 0; o < orders.size() && !placed; ++o) {
      int best = -1;
      size_t bestBlock = 0;
      size_t bestEnd = 0;
      std::vector<Place> bestPlaces;
      for (size_t n = 0; n <= blocks.size(); ++n) {
        const size_t b = n == 0 ? static_cast<size_t>(cursor) : n - 1;
        if (b >= blocks.size() || (n > 0 && b == static_cast<size_t>(cursor)) || done[b] >= blocks[b].size()) {
          continue;
        }
        const size_t end = matchLine(line, orders[o], blocks[b], done[b], static_cast<int>(b), book.consumedTokens);
        if (end == std::string::npos) continue;
        const Place& last = line.places[orders[o].back()];
        const bool atABreak =
            end == blocks[b].size() || blocks[b][end] == ' ' || !last.gapAfter.empty() || last.splitHyphen;
        const int score = atABreak ? 2 : nextGoesOn(blocks[b], end, static_cast<int>(b)) ? 1 : 0;
        if (score > best) {
          best = score;
          bestBlock = b;
          bestEnd = end;
          bestPlaces = line.places;
        }
        if (score == 2) break;
      }
      if (best < 0) continue;
      line.places = bestPlaces;
      done[bestBlock] = bestEnd;
      cursor = static_cast<int>(bestBlock);
      line.reading = orders[o];
      line.reversed = o == 1;
      placed = true;
    }
    if (!placed) {
      const std::string& src = blocks.empty() ? std::string() : blocks[std::min<size_t>(cursor, blocks.size() - 1)];
      const size_t from = done.empty() ? 0 : std::min(done[std::min<size_t>(cursor, done.size() - 1)], src.size());
      out.failures.push_back("spine " + std::to_string(line.spine) + ": a line its text does not hold: " +
                             lineText(line) + "\n    the text read on: " + src.substr(from, 200));
      line.reading = orders[0];
      continue;
    }
    for (const Place& p : line.places) line.gaps += (p.gapBefore.empty() ? 0 : 1) + (p.gapAfter.empty() ? 0 : 1);
  }
  for (const auto& [spine, blocks] : out.blocks) {
    for (size_t b = 0; b < blocks.size(); ++b) {
      size_t at = read[spine][b];
      while (at < blocks[b].size() && blocks[b][at] == ' ') ++at;
      if (at < blocks[b].size()) {
        out.failures.push_back("spine " + std::to_string(spine) + ": text no line holds: " + blocks[b].substr(at, 200));
      }
    }
  }

  for (size_t l = 0; l < out.lines.size(); ++l) {
    for (const uint16_t i : out.lines[l].reading) out.stream.push_back({l, i});
  }
  // A token: words in one style, glued in the text, with no consumed token between them.
  out.token.resize(out.stream.size());
  int t = -1;
  for (size_t s = 0; s < out.stream.size(); ++s) {
    if (!out.glued(s) || !out.gapBetween(s).empty() || !sameStyle(out.word(s - 1), out.word(s))) ++t;
    out.token[s] = t;
  }
  return out;
}

class ShapingRules : public testing::TestWithParam<Book> {
 protected:
  fs::path work;

  void SetUp() override {
    // Per test as well as per book: ctest runs the cases of one book side by side.
    work = fs::temp_directory_path() / (std::string("epub_shaping_rules_") + GetParam().label + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override {
    TextShaping::releaseBuildMemory();
    fs::remove_all(work);
  }

  // The book read back; the reading's failures (rule 6) fail the caller.
  BookReading read() const {
    BookReading reading = readBook(GetParam(), profileOf(GetParam()), work);
    EXPECT_TRUE(reading.built);
    return reading;
  }
  void logCount(const char* what, const int count, const int floor) const {
    std::printf("[ ShapingRules ] %s: %d %s (floor %d)\n", GetParam().label, count, what, floor);
  }
};

// A word's request for IndicShaper: its face and drawn size, in the book's language.
IndicShaper::Request requestFor(const Book& book, const Word& w, GfxRenderer& r) {
  IndicShaper::Request request;
  uint32_t facePpem = 0;
  request.face = r.shapingFace(w.box.fontId,
                               static_cast<EpdFontFamily::Style>(w.box.style & EpdFontFamily::BOLD_ITALIC), &facePpem);
  request.ppem26_6 = w.ppem26_6;
  request.languageTags = ot::languageTagsFor(book.language);
  return request;
}

IndicShaper::Outcome shape(const std::string_view text, const IndicShaper::Request& request, RecordingSink& sink) {
  IndicShaper::CallMemory memory(nullptr, text.size());
  return IndicShaper::shapeToken(text, request, memory, sink);
}

// Whether a piece of `text` holds only what the line pass never draws: joiners and bidi controls.
bool undrawn(const std::string_view text) {
  const std::vector<uint32_t> cps = codepointsOf(std::string(text));
  return std::all_of(cps.begin(), cps.end(),
                     [](const uint32_t cp) { return cp == kZwnj || cp == kZwj || BidiLine::isControl(cp); });
}

}  // namespace

// Rule 1.
TEST_P(ShapingRules, GlyphFormsSitOnShapedLettersOnly) {
  const BookReading book = read();
  int glyphWords = 0;
  for (const Line& line : book.lines) {
    for (uint16_t i = 0; i < line.words.size(); ++i) {
      const Word& w = line.words[i];
      if (!w.glyphs) continue;
      ++glyphWords;
      EXPECT_TRUE(holdsShapedLetter(w.text))
          << "a glyph form on a word without Indic, Arabic or Myanmar letters: " << where(line, i);
      for (const uint32_t cp : codepointsOf(w.text)) {
        EXPECT_FALSE(isDigit(cp)) << "a glyph form over a digit: " << where(line, i);
        EXPECT_FALSE(isMirrored(cp)) << "a glyph form over a mirrored code point: " << where(line, i);
        EXPECT_FALSE(BidiLine::isControl(cp)) << "a glyph form over a bidi control: " << where(line, i);
      }
    }
  }
  logCount("glyph words", glyphWords, GetParam().minGlyphWords);
  EXPECT_GE(glyphWords, GetParam().minGlyphWords);
}

// Rule 2.
TEST_P(ShapingRules, ShapedWordsAreAsWideAsMeasured) {
  const BookReading book = read();
  int glued = 0;
  int spaced = 0;
  for (const Line& line : book.lines) {
    for (uint16_t i = 1; i < line.words.size(); ++i) {
      const Word& before = line.words[i - 1];
      const Word& w = line.words[i];
      if (!before.glyphs && !w.glyphs) continue;
      if (w.continues) {
        ++glued;
        EXPECT_EQ(before.box.x + before.box.width, w.box.x)
            << "a glued word apart from a shaped one: " << where(line, i);
      } else if (before.glyphs) {
        ++spaced;
        EXPECT_LE(before.box.x + before.box.width, w.box.x)
            << "a shaped word over the spaced word after it: " << where(line, i);
      }
    }
  }
  logCount("glued pairs with a shaped word", glued, 1);
  logCount("shaped words before a spaced one", spaced, 1);
  EXPECT_GT(glued, 0);
  EXPECT_GT(spaced, 0);
}

// Rule 3.
TEST_P(ShapingRules, NoLineBreaksInsideASyllable) {
  const BookReading book = read();
  int lines = 0;
  for (const Line& line : book.lines) {
    const size_t n = line.reading.size();
    if (n == 0) continue;
    ++lines;
    const uint16_t first = line.reading.front();
    const std::vector<uint32_t> cps = codepointsOf(line.words[first].plain);
    if (!cps.empty()) {
      EXPECT_FALSE(indic::isDependentSign(cps.front())) << "a line starts with a sign: " << where(line, first);
      EXPECT_FALSE(arabic::isMark(cps.front())) << "a line starts with an Arabic mark: " << where(line, first);
      EXPECT_FALSE(isMyanmarSign(cps.front())) << "a line starts with a Myanmar sign: " << where(line, first);
      EXPECT_NE(kZwj, cps.front()) << "a line starts with a ZWJ: " << where(line, first);
      EXPECT_NE(kZwnj, cps.front()) << "a line starts with a ZWNJ: " << where(line, first);
    }
    // A line that ends inside a word ends with the split's hyphen, a piece glued to the word's Indic part.
    const uint16_t last = line.reading.back();
    if (n >= 2 && line.words[last].plain == "-" && line.places[last].splitHyphen) {
      const uint16_t part = line.reading[n - 2];
      const std::vector<uint32_t> head = codepointsOf(line.words[part].plain);
      if (holdsIndic(line.words[part].plain) && !head.empty()) {
        EXPECT_FALSE(indic::isVirama(head.back())) << "a line ends with a virama: " << where(line, part);
      }
    }
  }
  // A word split across lines without a hyphen (a Myanmar phrase wider than the line): the cut is Myanmar's.
  int unhyphenated = 0;
  for (size_t s = 1; s < book.stream.size(); ++s) {
    if (&book.lineOf(s - 1) == &book.lineOf(s) || !book.glued(s)) continue;
    const std::vector<uint32_t> head = codepointsOf(book.word(s - 1).plain);
    const std::vector<uint32_t> tail = codepointsOf(book.word(s).plain);
    if (head.empty() || tail.empty()) continue;
    size_t next = 1;
    while (next < tail.size() && tail[next] == myanmar::DOT_BELOW) ++next;
    if (myanmar::isRunMember(tail.front())) ++unhyphenated;
    EXPECT_TRUE(myanmar::cutAllowed(head.back(), tail.front(), next < tail.size() ? tail[next] : 0))
        << "a word cut inside a Myanmar syllable: " << where(book.lineOf(s), book.stream[s].word);
  }
  logCount("lines", lines, 1);
  logCount("Myanmar words cut without a hyphen", unhyphenated, GetParam().minUnhyphenatedSplits);
  EXPECT_GT(lines, 0);
  EXPECT_GE(unhyphenated, GetParam().minUnhyphenatedSplits);
}

// Rule 4.
TEST_P(ShapingRules, GlyphFormsAreTheEnginesShaping) {
  const Book& param = GetParam();
  const BookReading book = read();
  const pipeline_harness::ShapingFaceScope face(param.font);
  GfxRenderer r;
  int checked = 0;
  for (size_t s = 0; s < book.stream.size(); ++s) {
    const Word& w = book.word(s);
    if (!w.glyphs) continue;
    const Line& line = book.lineOf(s);
    const uint16_t i = book.stream[s].word;
    ++checked;
    IndicShaper::Request request = requestFor(param, w, r);
    ASSERT_NE(nullptr, request.face) << where(line, i);
    // The glued tokens kept apart, whole, as the layout passed them, where the word is its token's edge.
    std::string before;
    std::string after;
    const int token = book.token[s];
    if ((s == 0 || book.token[s - 1] != token) && s > 0 && book.glued(s)) {
      const std::string gap = book.gapBetween(s);
      before = gap.empty() ? book.tokenText(book.token[s - 1]) : gap;
    }
    if (!book.place(s).gapAfter.empty()) {
      after = book.place(s).gapAfter;  // the consumed token ending its line
    } else if (s + 1 < book.stream.size() && book.token[s + 1] != token && book.glued(s + 1)) {
      const std::string gap = book.gapBetween(s + 1);
      after = gap.empty() ? book.tokenText(book.token[s + 1]) : gap;
    }
    request.before = before;
    request.after = after;
    RecordingSink sink;
    ASSERT_EQ(IndicShaper::Outcome::Formed, shape(w.text, request, sink)) << where(line, i);
    int runs = 0;
    for (const Piece& piece : sink.pieces) {
      if (piece.kind == IndicShaper::PieceKind::Glyphs) {
        ++runs;
        EXPECT_EQ(piece.glyphs, w.records) << where(line, i) << " (context '" << before << "', '" << after << "')";
        continue;
      }
      EXPECT_TRUE(undrawn(std::string_view(w.text).substr(piece.begin, piece.end - piece.begin)))
          << "a drawn piece inside a glyph word: " << where(line, i);
    }
    EXPECT_EQ(1, runs) << where(line, i);
    TextShaping::releaseBuildMemory();
  }
  logCount("glyph forms", checked, param.minGlyphWords);
  EXPECT_GE(checked, param.minGlyphWords);
}

// Rule 5.
TEST_P(ShapingRules, EveryWordIsDrawnInsideThePage) {
  const int width = GetParam().viewportWidth;
  const BookReading book = read();
  int checked = 0;
  int drift = 0;
  for (const Line& line : book.lines) {
    for (uint16_t i = 0; i < line.words.size(); ++i) {
      const TextBlock::WordBox& box = line.words[i].box;
      ++checked;
      drift = std::max({drift, -box.x, box.x + box.width - width});
      EXPECT_GE(box.x, 0) << where(line, i);
      EXPECT_LE(box.x + box.width, width) << where(line, i);
    }
  }
  logCount("words", checked, 1);
  logCount("px of width drift past the page's edges", drift, 0);
  EXPECT_EQ(0, drift);
  EXPECT_GT(checked, 0);
}

// Rule 6.
TEST_P(ShapingRules, ThePiecesCoverTheText) {
  const BookReading book = read();
  for (const std::string& failure : book.failures) ADD_FAILURE() << failure;
  int consumed = 0;
  for (size_t s = 0; s < book.stream.size(); ++s) {
    const Line& line = book.lineOf(s);
    const uint16_t i = book.stream[s].word;
    // A consumed token's letters are drawn by the glyph run before it.
    const Place& place = book.place(s);
    if (!place.gapBefore.empty()) {
      ++consumed;
      EXPECT_TRUE(s > 0 && book.word(s - 1).glyphs) << "a consumed token after no glyph run: " << where(line, i);
    }
    if (!place.gapAfter.empty()) {
      ++consumed;
      EXPECT_TRUE(book.word(s).glyphs) << "a consumed token after no glyph run: " << where(line, i);
    }
    // A run is one piece: two glued glyph words of one token are two scripts' runs.
    if (s > 0 && book.token[s - 1] == book.token[s] && book.word(s - 1).glyphs && book.word(s).glyphs &&
        &book.lineOf(s - 1) == &line) {
      EXPECT_NE(holdsIndic(book.word(s - 1).plain), holdsIndic(book.word(s).plain))
          << "one run in two glyph words: " << where(line, i);
    }
  }
  logCount("consumed tokens", consumed, GetParam().consumed);
  EXPECT_EQ(GetParam().consumed, consumed);
}

// Rule 7.
TEST_P(ShapingRules, EachLetterIsDrawnOnce) {
  const Book& param = GetParam();
  const BookReading book = read();
  const pipeline_harness::ShapingFaceScope face(param.font);
  GfxRenderer r;
  // The words of the text, by spine, block and position: the stream words each holds.
  std::map<std::tuple<int, int, size_t>, std::vector<size_t>> words;
  for (size_t s = 0; s < book.stream.size(); ++s) {
    const Place& place = book.place(s);
    if (place.block < 0) continue;
    const std::string& src = book.blocks.at(book.lineOf(s).spine)[place.block];
    const size_t start = src.rfind(' ', place.begin == 0 ? 0 : place.begin - 1);
    words[{book.lineOf(s).spine, place.block, start == std::string::npos ? 0 : start + 1}].push_back(s);
  }
  // The glyphs a space or a joiner maps to: a joiner the line pass absorbed into a glyph word (a Text piece of its
  // own, drawing nothing) has none, where the word shaped whole holds it, as nothing to draw.
  std::multiset<uint16_t> undrawnGlyphs;
  uint32_t facePpem = 0;
  if (const ot::Face* faceNow = r.shapingFace(kFontId, EpdFontFamily::REGULAR, &facePpem)->current()) {
    for (const uint32_t cp : {0x0020u, kZwnj, kZwj}) {
      uint32_t glyph = 0;
      if (faceNow->nominalGlyph(cp, &glyph)) undrawnGlyphs.insert(static_cast<uint16_t>(glyph));
    }
  }
  const auto drawable = [&](std::multiset<uint16_t>& glyphs) {
    for (const uint16_t g : undrawnGlyphs) glyphs.erase(g);
  };
  int checked = 0;
  int asCodePoints = 0;
  for (const auto& [key, members] : words) {
    bool glyphs = false;
    bool split = false;
    bool codePoints = false;
    for (const size_t s : members) {
      glyphs = glyphs || book.word(s).glyphs;
      // With its hyphen, or without one, on two lines (Myanmar).
      split = split || book.place(s).splitHyphen || &book.lineOf(s) != &book.lineOf(members.front());
      if (!book.word(s).glyphs) {
        const std::vector<uint32_t> cps = codepointsOf(book.word(s).plain);
        codePoints = codePoints || std::any_of(cps.begin(), cps.end(), drawsShapedScript);
      }
    }
    // A word split across lines is shaped in its parts.
    if (!glyphs || split) continue;
    // Part of it drawn as its code points (a token's leading marks, a run the line pass could not keep whole).
    if (codePoints) {
      ++asCodePoints;
      continue;
    }
    const auto& [spine, block, start] = key;
    const std::string& src = book.blocks.at(spine)[block];
    const size_t end = src.find(' ', start);
    const std::string whole = src.substr(start, end == std::string::npos ? std::string::npos : end - start);
    ++checked;
    const size_t first = *std::find_if(members.begin(), members.end(), [&](size_t s) { return book.word(s).glyphs; });
    IndicShaper::Request request = requestFor(param, book.word(first), r);
    RecordingSink sink;
    ASSERT_EQ(IndicShaper::Outcome::Formed, shape(whole, request, sink)) << whole;
    std::multiset<uint16_t> expected;
    for (const Piece& piece : sink.pieces) {
      for (const Glyph& g : piece.glyphs) expected.insert(g.glyph);
    }
    std::multiset<uint16_t> drawn;
    for (const size_t s : members) {
      for (const Glyph& g : book.word(s).records) drawn.insert(g.glyph);
    }
    TextShaping::releaseBuildMemory();
    drawable(expected);
    drawable(drawn);
    if (drawn == expected) continue;
    std::vector<uint16_t> missing;
    std::vector<uint16_t> extra;
    std::set_difference(expected.begin(), expected.end(), drawn.begin(), drawn.end(), std::back_inserter(missing));
    std::set_difference(drawn.begin(), drawn.end(), expected.begin(), expected.end(), std::back_inserter(extra));
    ADD_FAILURE() << "a word drawn with " << missing.size() << " of its glyphs missing and " << extra.size()
                  << " extra: '" << whole << "' in: " << lineText(book.lineOf(first));
  }
  logCount("words checked whole", checked, 1);
  logCount("words drawn in part as their code points", asCodePoints, param.codePointWords);
  EXPECT_EQ(param.codePointWords, asCodePoints);
  EXPECT_GT(checked, 0);
}

// Rule 8.
TEST_P(ShapingRules, AnArabicTokenReadsRightToLeft) {
  const BookReading book = read();
  int pairs = 0;
  for (size_t s = 1; s < book.stream.size(); ++s) {
    if (book.token[s - 1] != book.token[s] || &book.lineOf(s - 1) != &book.lineOf(s)) continue;
    const Line& line = book.lineOf(s);
    // A right-to-left line: its first word in reading order is right of its last.
    const bool rightToLeft =
        line.reading.size() >= 2 && line.words[line.reading.front()].box.x > line.words[line.reading.back()].box.x;
    if (!rightToLeft || !holdsArabicLetter(book.tokenText(book.token[s]))) continue;
    // A mark drawn as a piece of its own sits on the letter before it, not beside it.
    const auto marksOnly = [](const std::string& text) {
      const std::vector<uint32_t> cps = codepointsOf(text);
      return std::all_of(cps.begin(), cps.end(), [](const uint32_t cp) {
        return BidiUtils::isTransparentMark(cp) || cp == kZwnj || cp == kZwj || BidiLine::isControl(cp);
      });
    };
    if (marksOnly(book.word(s - 1).text) || marksOnly(book.word(s).text)) continue;
    ++pairs;
    EXPECT_LT(book.word(s).box.x, book.word(s - 1).box.x)
        << "a token's later word right of an earlier one: " << where(line, book.stream[s].word);
  }
  logCount("right-to-left pairs", pairs, GetParam().minRtlPairs);
  EXPECT_GE(pairs, GetParam().minRtlPairs);
}

// A line of a block (a paragraph, a heading, a table cell) without Indic or Arabic text is laid out as if no font
// could shape: the book's Latin lines are byte for byte the same, as the section file holds them, with its face and
// without one. A Latin line of a block that shapes is not: the block's other words, shaped, break its lines elsewhere.
TEST_P(ShapingRules, TheLatinLinesKeepTheirBytes) {
  const auto latinLines = [&](const pipeline_harness::Profile& profile, const char* cache) {
    std::vector<std::string> bytes;
    const BookReading book = readBook(GetParam(), profile, work, cache, &bytes);
    EXPECT_TRUE(book.built);
    EXPECT_TRUE(book.failures.empty()) << book.failures.front();
    std::vector<std::string> lines;
    for (size_t l = 0; l < book.lines.size() && l < bytes.size(); ++l) {
      const Line& line = book.lines[l];
      const auto placed =
          std::find_if(line.places.begin(), line.places.end(), [](const Place& p) { return p.block >= 0; });
      if (placed == line.places.end() || holdsShapedLetter(book.blocks.at(line.spine)[placed->block])) continue;
      const bool latin = std::any_of(line.words.begin(), line.words.end(), [](const Word& w) {
        const std::vector<uint32_t> cps = codepointsOf(w.text);
        return std::any_of(cps.begin(), cps.end(),
                           [](const uint32_t cp) { return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'); });
      });
      if (latin) lines.push_back(bytes[l]);
    }
    return lines;
  };
  pipeline_harness::Profile unshaped = profileOf(GetParam());
  unshaped.shapingFontFile = nullptr;
  const std::vector<std::string> shapedLines = latinLines(profileOf(GetParam()), "shaped");
  const std::vector<std::string> unshapedLines = latinLines(unshaped, "unshaped");
  logCount("Latin lines", static_cast<int>(shapedLines.size()), GetParam().minLatinLines);
  EXPECT_GE(static_cast<int>(shapedLines.size()), GetParam().minLatinLines);
  EXPECT_EQ(unshapedLines, shapedLines);
}

// The cases the rules must have met, against their floors; and no section laid out degraded.
TEST_P(ShapingRules, TheCasesAreOnThePages) {
  const Book& param = GetParam();
  const BookReading book = read();
  EXPECT_FALSE(book.degraded) << "a section laid out degraded";
  int merged = 0;
  int keptApartPairs = 0;
  int gluedToPunctuation = 0;
  int besideDigits = 0;
  int pastTheCap = 0;
  int split = 0;
  int fallbackLines = 0;
  int pastTheGuard = 0;
  for (const Line& line : book.lines) {
    const size_t n = line.words.size();
    size_t slots = 0;
    bool glyphs = false;
    for (uint16_t i = 0; i < n; ++i) {
      const Word& w = line.words[i];
      for (const uint32_t cp : codepointsOf(w.plain)) slots += arabic::isMark(cp) || cp == kZwj || cp == kZwnj ? 0 : 1;
      if (i > 0 && !w.continues) ++slots;  // the word space
      if (i > 0 && w.continues && !sameStyle(line.words[i - 1], w) && holdsShapedLetter(line.words[i - 1].text) &&
          holdsShapedLetter(w.text)) {
        ++keptApartPairs;
      }
      if (!w.glyphs) continue;
      glyphs = true;
      if (std::find(param.mergedWords.begin(), param.mergedWords.end(), w.text) != param.mergedWords.end()) ++merged;
      const bool punctuationBefore = i > 0 && w.continues && isPunctuation(line.words[i - 1].text);
      const bool punctuationAfter = i + 1 < n && line.words[i + 1].continues && isPunctuation(line.words[i + 1].text);
      if (punctuationBefore || punctuationAfter) ++gluedToPunctuation;
      const auto holdsDigit = [](const Word& o) {
        const std::vector<uint32_t> cps = codepointsOf(o.plain);
        return std::any_of(cps.begin(), cps.end(), isAsciiDigitOrArabicDigit);
      };
      if ((i > 0 && w.continues && holdsDigit(line.words[i - 1])) ||
          (i + 1 < n && line.words[i + 1].continues && holdsDigit(line.words[i + 1]))) {
        ++besideDigits;
      }
      if (codepointsOf(w.text).size() > IndicShaper::kMaxRunCodepoints) ++pastTheCap;
    }
    // What the line pass held: the consumed tokens' letters count as its slots too.
    for (const Place& place : line.places) {
      for (const uint32_t cp : codepointsOf(place.gapBefore + place.gapAfter)) slots += arabic::isMark(cp) ? 0 : 1;
    }
    if (glyphs && slots > BidiLine::kMaxCodepoints) ++fallbackLines;
    // The pieces the line was placed with, before the consumed ones were dropped: past a byte, no logical order.
    if (n + static_cast<size_t>(line.gaps) > kMaxOrderedPieces) {
      ++pastTheGuard;
      EXPECT_FALSE(line.ordered) << "a logical order on a line of " << n + line.gaps << " pieces: " << lineText(line);
    } else {
      EXPECT_FALSE(line.reversed) << "a line read right to left with no logical order: " << lineText(line);
    }
  }
  for (size_t s = 0; s + 1 < book.stream.size(); ++s) {
    const bool unhyphenated = &book.lineOf(s) != &book.lineOf(s + 1) && book.glued(s + 1);
    if (book.word(s).glyphs && (book.place(s + 1).splitHyphen || unhyphenated) && book.token[s] == book.token[s + 1]) {
      ++split;
    }
  }
  const auto mergedFloor = static_cast<int>(param.mergedWords.size());
  logCount("merged words", merged, mergedFloor);
  logCount("kept-apart pairs", keptApartPairs, param.minKeptApart);
  logCount("glyph words glued to punctuation", gluedToPunctuation, param.minGluedToPunctuation);
  logCount("glyph words glued to digits", besideDigits, param.minBesideDigits);
  logCount("words past the cap", pastTheCap, param.minPastTheCap);
  logCount("words split across lines", split, param.minSplitWords);
  logCount("lines with glyph forms past the line pass's slots", fallbackLines, param.minFallbackLines);
  logCount("lines past a logical order's pieces", pastTheGuard, param.minPastTheGuard);
  EXPECT_EQ(mergedFloor, merged);
  EXPECT_GE(keptApartPairs, param.minKeptApart);
  EXPECT_GE(gluedToPunctuation, param.minGluedToPunctuation);
  EXPECT_GE(besideDigits, param.minBesideDigits);
  EXPECT_GE(pastTheCap, param.minPastTheCap);
  EXPECT_GE(split, param.minSplitWords);
  EXPECT_GE(fallbackLines, param.minFallbackLines);
  EXPECT_GE(pastTheGuard, param.minPastTheGuard);
}

namespace {

// The SCT lines of the builds in progress (Section's arena and word cache lines).
std::vector<std::string>* gSctLines = nullptr;
void collectSct(const char*, const char* origin, const char* message) {
  if (gSctLines != nullptr && std::strcmp(origin, "SCT") == 0) gSctLines->emplace_back(message);
}

// The first of `lines` holding `marker`, from the marker on; empty when none does.
std::string lineWith(const std::vector<std::string>& lines, const char* marker) {
  for (const std::string& line : lines) {
    const size_t at = line.find(marker);
    if (at != std::string::npos) return line.substr(at);
  }
  return {};
}

struct WordCacheLine {
  unsigned bytes = 0, hits = 0, misses = 0, inserts = 0, bypassed = 0, oversize = 0, clears = 0;
};

bool parseWordCacheLine(const std::string& line, WordCacheLine* out) {
  return std::sscanf(line.c_str(),
                     "word cache: bytes=%u hits=%u misses=%u inserts=%u bypassed=%u oversize=%u clears=%u", &out->bytes,
                     &out->hits, &out->misses, &out->inserts, &out->bypassed, &out->oversize, &out->clears) == 7;
}

// Restores the word cache's arming however the test leaves.
struct WordCacheOff {
  WordCacheOff() { WordShapeCache::setEnabled(false); }
  ~WordCacheOff() { WordShapeCache::setEnabled(true); }
};

// A paragraph that sets alone a syllable the book's cases glue between two others, when it shapes otherwise without
// them as context (rule 4's teeth): the Bengali case's bold হে. Shaped with its neighbours, its form must never be
// replayed for it alone, so a glued token must stay out of the word cache.
std::string keptApartAlone(const Book& book) {
  return std::string(book.name) == "test_shaped_bengali" ? "<p><b>\xE0\xA6\xB9\xE0\xA7\x87</b></p>\n" : "";
}

// One chapter of the book's chapter 2 `repeats` times over, as the long timing book repeats the declaration
// (make_test_shaped_indic.py --long): its words come again, with every case of chapter 2 among them, and
// keptApartAlone() after the first. Written beside `work` with the book's stylesheet and language; the path.
std::string longChapterBook(const Book& book, const fs::path& work, const int repeats) {
  const auto item = [&](const char* href) {
    Epub source(corpusBook(book.name), (work / "source").string());
    size_t size = 0;
    uint8_t* bytes = source.readItemContentsToBytes(href, &size, false);
    std::string out = bytes ? std::string(reinterpret_cast<const char*>(bytes), size) : std::string();
    free(bytes);
    return out;
  };
  const std::string chapter = item("OEBPS/ch2.xhtml");
  const std::string css = item("OEBPS/style.css");
  const size_t open = chapter.find("<body>");
  const size_t close = chapter.find("</body>");
  EXPECT_NE(std::string::npos, open);
  EXPECT_NE(std::string::npos, close);
  EXPECT_FALSE(css.empty());
  if (open == std::string::npos || close == std::string::npos) return {};
  const size_t bodyAt = open + std::strlen("<body>");
  const std::string body = chapter.substr(bodyAt, close - bodyAt);
  std::string xhtml = chapter.substr(0, bodyAt);
  for (int r = 0; r < repeats; ++r) {
    xhtml += body;
    // Early, while the cache is as it was armed: right after the first time its glued case is laid out.
    if (r == 0) xhtml += keptApartAlone(book);
  }
  xhtml += chapter.substr(close);

  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
          "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
          "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
  zip.add("content.opf",
          std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
                      "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
                      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">long</dc:identifier>"
                      "<dc:title>Long</dc:title><dc:language>") +
              book.language +
              "</dc:language></metadata>\n<manifest>\n"
              "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
              "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n"
              "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
  zip.add("chapter.xhtml", xhtml);
  zip.add("style.css", css);
  const fs::path path = work / "long.epub";
  zip.write(path.string());
  return path.string();
}

}  // namespace

// The word cache changes no line. A long chapter laid out on a lent arena sized so that the parse lane's page and call
// blocks reach the cache's tail and empty it more than once is the same with the cache as with the cache disarmed; and
// the cache did replay words. Bengali's glued bold syllable, set alone too, keeps a context-shaped form out of it.
TEST_P(ShapingRules, TheWordCacheChangesNoLine) {
  const Book& book = GetParam();
  const std::string path = longChapterBook(book, work, kLongChapterRepeats);
  ASSERT_FALSE(path.empty());
  pipeline_harness::Profile profile = profileOf(book);
  // Pages three times as tall as the harness's: a page block holds three times the lines, and the parse lane rises well
  // past the room the cache keeps below its tail.
  profile.viewportHeight = 2280;
  const auto run = [&](const char* cache, const size_t arenaBytes, std::vector<std::string>* lines) {
    pipeline_harness::Profile lent = profile;
    lent.lentArenaBytes = arenaBytes;
    std::ostringstream out;
    gSctLines = lines;
    setLogObserver(collectSct);
    const bool ok = pipeline_harness::runAndDump(path, (work / cache).string(), lent, out);
    setLogObserver(nullptr);
    gSctLines = nullptr;
    EXPECT_TRUE(ok) << out.str();
    return out.str();
  };

  // What the parse holds when the cache arms (the SAX state, the feed chunk, the ruleset), on a roomy arena.
  std::vector<std::string> probeLines;
  run("probe", 52272, &probeLines);
  unsigned cap = 0, highWater = 0, setup = 0, extract = 0, resident = 0, parse = 0;
  ASSERT_EQ(6, std::sscanf(lineWith(probeLines, "arena: ").c_str(),
                           "arena: cap=%u highWater=%u lanes(setup=%u extract=%u resident=%u parse=%u", &cap,
                           &highWater, &setup, &extract, &resident, &parse))
      << lineWith(probeLines, "arena: ");
  // An arena the cache arms on, sized from the parse lane: the cache arms with the cursor within `slack` of what the
  // parse holds (early on a page), and its tail starts kRoomBelow above that, halfway between the arming line and the
  // lane's peak, so a full page's page and call blocks reach it. Page after page, it is emptied and filled again.
  ASSERT_GT(parse, WordShapeCache::kRoomBelow + 1024) << "the lane must reach the tail for the test to mean anything";
  const size_t slack = (parse - WordShapeCache::kRoomBelow) / 2;
  const size_t arenaBytes = resident + WordShapeCache::kWordCacheBytes + WordShapeCache::kRoomBelow + slack;

  std::vector<std::string> onLines, offLines;
  const std::string with = run("on", arenaBytes, &onLines);
  std::string without;
  {
    const WordCacheOff off;
    without = run("off", arenaBytes, &offLines);
  }

  WordCacheLine on, disarmed;
  ASSERT_TRUE(parseWordCacheLine(lineWith(onLines, "word cache: "), &on)) << lineWith(onLines, "word cache: ");
  ASSERT_TRUE(parseWordCacheLine(lineWith(offLines, "word cache: "), &disarmed));
  std::printf(
      "[ ShapingRules ] %s: arena %zu B (resident %u, parse lane %u): word cache hits=%u misses=%u "
      "inserts=%u bypassed=%u oversize=%u clears=%u\n",
      book.label, arenaBytes, resident, parse, on.hits, on.misses, on.inserts, on.bypassed, on.oversize, on.clears);
  EXPECT_EQ(WordShapeCache::kWordCacheBytes, on.bytes);
  EXPECT_GE(on.clears, 2u);  // emptied, armed again, and emptied again
  EXPECT_GT(on.hits, 0u);
  EXPECT_EQ(0u, disarmed.bytes);
  EXPECT_EQ(0u, disarmed.hits);
  EXPECT_EQ(0u, disarmed.inserts);
  EXPECT_EQ(0u, disarmed.oversize);  // never armed: nothing was offered
  EXPECT_GT(disarmed.bypassed, 0u);
  EXPECT_EQ(without, with);

  // The S3 boards' 256 KB PSRAM arena: the large tail, never reached by the parse lane, changes no line either, and
  // replays at least what S did.
  std::vector<std::string> largeLines;
  const std::string large = run("large", 256 * 1024, &largeLines);
  WordCacheLine psram;
  ASSERT_TRUE(parseWordCacheLine(lineWith(largeLines, "word cache: "), &psram)) << lineWith(largeLines, "word cache: ");
  std::printf("[ ShapingRules ] %s: PSRAM arena: word cache hits=%u misses=%u inserts=%u clears=%u\n", book.label,
              psram.hits, psram.misses, psram.inserts, psram.clears);
  EXPECT_EQ(WordShapeCache::kLargeWordCacheBytes, psram.bytes);
  EXPECT_EQ(0u, psram.clears);
  EXPECT_EQ(0u, psram.bypassed);
  EXPECT_GE(psram.hits, on.hits);
  EXPECT_EQ(without, large);
}

namespace {

// The Allah ligature's span token, alone, with the lam before it and as the heh alone (make_test_shaped_arabic.py).
const char* const kAllahAfterLam = "\xD9\x84\xD9\x91\xD9\xB0\xD9\x87";
const char* const kAllahAfterAlef = "\xD9\x84\xD9\x84\xD9\x91\xD9\xB0\xD9\x87";
const char* const kAllahHeh = "\xD9\x87";

Book arabicBook(const char* label, const uint16_t viewportWidth) {
  Book book{"test_shaped_arabic", label, "NotoNaskhArabic-Book-Regular.ttf", "ar", viewportWidth};
  // al-arabiyya (a span split), kataba (a coloured fatha), qul (a bold damma)
  book.mergedWords = {"\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9", "\xD9\x83\xD9\x8E\xD8\xAA\xD8\xA8",
                      "\xD9\x82\xD9\x8F\xD9\x84"};
  // Allah split by a span after its alef, after its first lam or before its heh: each token is shaped with the whole
  // other token as context, so the ligature forms alike on both and is drawn once, by the token that holds its first
  // letter; the other keeps nothing. No letter is lost or drawn twice, and no case is an exception to rule 7.
  book.consumedTokens = {kAllahAfterAlef, kAllahAfterLam, kAllahHeh};
  book.minGlyphWords = 450;
  book.minKeptApart = 12;
  book.minGluedToPunctuation = 30;
  book.minPastTheCap = 1;
  book.consumed = 204;  // 200 of the stress paragraph's Allah splits and four of the book's cases
  book.minBesideDigits = 2;
  book.minRtlPairs = 30;
  // The paragraph of span-split ligatures passes the line pass's slots even at 460 px.
  book.minFallbackLines = 4;
  // kataba with its second syllable bold (the token's leading fatha), and the ligature's first token with a qamats.
  book.codePointWords = 2;
  return book;
}

std::vector<Book> shapedBooks() {
  std::vector<Book> books;
  const auto indic = [&](const char* name, const char* font, const char* language, std::vector<std::string> merged,
                         const int keptApart, const int punctuation, const int pastTheCap, const int split) {
    Book book{name, name, font, language};
    book.mergedWords = std::move(merged);
    book.minGlyphWords = 150;
    book.minKeptApart = keptApart;
    book.minGluedToPunctuation = punctuation;
    book.minPastTheCap = pastTheCap;
    book.minSplitWords = split;
    books.push_back(book);
  };
  // hindi, ki; bangla, ki; tamil, ki; malayalam, ki
  indic("test_shaped_devanagari", "NotoSansDevanagari-Book-Regular.ttf", "hi",
        {"\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80", "\xE0\xA4\x95\xE0\xA4\xBF"}, 3, 29,
        0, 0);
  indic("test_shaped_bengali", "NotoSansBengali-Book-Regular.ttf", "bn",
        {"\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\x82\xE0\xA6\xB2\xE0\xA6\xBE", "\xE0\xA6\x95\xE0\xA6\xBF"}, 6, 22, 0, 0);
  // Its cases end niti<sup>ka</sup> with the danda glued: a token of its own after the superscript, drawn as its code
  // point, which the word shaped whole takes into its run.
  books.back().codePointWords = 1;
  indic("test_shaped_tamil", "NotoSansTamil-Book-Regular.ttf", "ta",
        {"\xE0\xAE\xA4\xE0\xAE\xAE\xE0\xAE\xBF\xE0\xAE\xB4\xE0\xAF\x8D", "\xE0\xAE\x95\xE0\xAE\xBF"}, 3, 36, 0, 0);
  indic("test_shaped_malayalam", "NotoSansMalayalam-Book-Regular.ttf", "ml",
        {"\xE0\xB4\xAE\xE0\xB4\xB2\xE0\xB4\xAF\xE0\xB4\xBE\xE0\xB4\xB3\xE0\xB4\x82", "\xE0\xB4\x95\xE0\xB4\xBF"}, 3, 24,
        2, 2);
  // ko (a bold e), myanma (a bold killed na), and ka with a ZWJ before its medial ya, each with its little section.
  const std::vector<std::string> myanmarMerged = {
      "\xE1\x80\x80\xE1\x80\xB1\xE1\x80\xAC\xE1\x81\x8A",
      "\xE1\x80\x99\xE1\x80\xBC\xE1\x80\x94\xE1\x80\xBA\xE1\x80\x99\xE1\x80\xAC\xE1\x81\x8A",
      "\xE1\x80\x80\xE2\x80\x8D\xE1\x80\xBB\xE1\x81\x8A"};
  // Lines end between syllables inside phrases (the Myanmar design's PR 3): at 460 px the long phrases are cut, so few
  // words stay past one call's code points.
  indic("test_shaped_myanmar", "NotoSansMyanmar-Book-Regular.ttf", "my", myanmarMerged, 4, 2, 3, 30);
  books.back().minUnhyphenatedSplits = 30;
  // 1,600 px wide, where more phrases stay whole on their lines, and the two Latin lines are one.
  indic("test_shaped_myanmar", "NotoSansMyanmar-Book-Regular.ttf", "my", myanmarMerged, 4, 2, 4, 8);
  books.back().label = "test_shaped_myanmar_1600";
  books.back().viewportWidth = 1600;
  books.back().minUnhyphenatedSplits = 8;
  books.back().minLatinLines = 1;

  Book arabic = arabicBook("test_shaped_arabic", 460);
  arabic.minSplitWords = 1;  // the table's narrow cell
  books.push_back(arabic);
  Book wide = arabicBook("test_shaped_arabic_1600", 1600);
  // Its lines take the line pass's fallback, the paragraph of span-split ligatures one past a logical order's byte.
  wide.minFallbackLines = 6;
  wide.minPastTheGuard = 1;
  books.push_back(wide);

  Book persian{"test_shaped_persian", "test_shaped_persian", "NotoNaskhArabic-Book-Regular.ttf", "fa"};
  // farsi (a span split), ketab-e (a coloured kasra), khane-ye (a bold hamza)
  persian.mergedWords = {"\xD9\x81\xD8\xA7\xD8\xB1\xD8\xB3\xDB\x8C", "\xDA\xA9\xD8\xAA\xD8\xA7\xD8\xA8\xD9\x90",
                         "\xD8\xAE\xD8\xA7\xD9\x86\xD9\x87\xD9\x94"};
  persian.minGlyphWords = 230;
  persian.minKeptApart = 5;
  persian.minGluedToPunctuation = 30;
  persian.minRtlPairs = 30;
  books.push_back(persian);

  Book urdu{"test_shaped_urdu", "test_shaped_urdu", "NotoNaskhArabic-Book-Regular.ttf", "ur"};
  // urdu (a span split), huquq-e (a coloured zer), duniya (a bold pesh)
  urdu.mergedWords = {"\xD8\xA7\xD8\xB1\xD8\xAF\xD9\x88", "\xD8\xAD\xD9\x82\xD9\x88\xD9\x82\xD9\x90",
                      "\xD8\xAF\xD9\x8F\xD9\x86\xDB\x8C\xD8\xA7"};
  urdu.minGlyphWords = 280;
  urdu.minKeptApart = 4;
  urdu.minGluedToPunctuation = 20;
  urdu.minBesideDigits = 2;
  urdu.minRtlPairs = 20;
  books.push_back(urdu);

  Book hebrew{"test_shaped_bidi_indic", "test_shaped_bidi_indic", "NotoSansDevanagari-Book-Regular.ttf", "he"};
  // hindi, ki
  hebrew.mergedWords = {"\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80",
                        "\xE0\xA4\x95\xE0\xA4\xBF"};
  // The Hindi cases after the split Latin word keep their glyph forms: the store stayed in step.
  hebrew.minGlyphWords = 40;
  hebrew.minKeptApart = 3;
  hebrew.minGluedToPunctuation = 5;
  books.push_back(hebrew);
  return books;
}

std::string bookLabel(const testing::TestParamInfo<Book>& info) { return info.param.label; }

INSTANTIATE_TEST_SUITE_P(ShapedBooks, ShapingRules, testing::ValuesIn(shapedBooks()), bookLabel);

}  // namespace
