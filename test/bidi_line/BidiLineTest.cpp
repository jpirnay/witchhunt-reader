// BidiLineTest: the line pass (BidiLine::layoutLine) that turns one line's tokens into pieces in visual order, the
// controls open across lines, a token's own drawn form, and the workspace a build's arena lends.
//
// A line is rendered as `t<token>:<display>` items, left to right, joined by ` S<n> ` for a word space (token n's)
// and ` G ` for a glued boundary. Expected values were checked against minibidi on 2026-11-02.

#include <Arduino.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BidiLine.h"
#include "BidiUtils.h"
#include "BuildArena.h"
#include "Utf8.h"

namespace {

using BidiLine::Gap;
using CP = std::vector<uint32_t>;

constexpr uint32_t LRM = 0x200E, LRE = 0x202A, RLE = 0x202B, PDF = 0x202C, LRO = 0x202D, RLO = 0x202E;
constexpr uint32_t LRI = 0x2066, RLI = 0x2067, FSI = 0x2068, PDI = 0x2069;

// The controls are spelled out rather than written into the literals, so the source holds no invisible characters.
std::string u(const uint32_t cp) {
  std::string utf8;
  utf8AppendCodepoint(cp, utf8);
  return utf8;
}

BidiLine::Scratch& scratch() {
  static const std::unique_ptr<BidiLine::Scratch> s = std::make_unique<BidiLine::Scratch>();
  return *s;
}

BidiLine::OpenControls openWith(const std::initializer_list<uint32_t> cps) {
  BidiLine::OpenControls open;
  for (const uint32_t cp : cps) open.cps[open.count++] = cp;
  return open;
}

CP openCps(const BidiLine::OpenControls& open) { return CP(open.cps, open.cps + open.count); }

CP decode(const std::string_view utf8) {
  const std::string copy(utf8);
  CP cps;
  auto* p = reinterpret_cast<const unsigned char*>(copy.c_str());
  while (*p) cps.push_back(utf8NextCodepoint(&p));
  return cps;
}

std::string_view displayOf(const BidiLine::Line& line, const BidiLine::Piece& piece) {
  return std::string_view(line.display).substr(piece.displayBegin, piece.displayLength);
}

// What every line must hold, whatever its content: each form is followed by a NUL; a token's pieces, in logical order,
// cover its bytes exactly; the reading indices are a permutation, and `reordered` says whether it is the identity.
void expectWellFormed(const BidiLine::Line& line, const std::vector<std::string>& words) {
  std::vector<const BidiLine::Piece*> logical;
  for (const auto& piece : line.pieces) {
    ASSERT_LT(piece.token, words.size());
    ASSERT_LT(piece.displayBegin + piece.displayLength, line.display.size());
    EXPECT_EQ(line.display[piece.displayBegin + piece.displayLength], '\0');
    logical.push_back(&piece);
  }
  std::sort(logical.begin(), logical.end(), [](const BidiLine::Piece* a, const BidiLine::Piece* b) {
    return a->token != b->token ? a->token < b->token : a->begin < b->begin;
  });
  bool reordered = false;
  for (size_t r = 0; r < logical.size(); r++) {
    const BidiLine::Piece& piece = *logical[r];
    EXPECT_EQ(piece.readingIndex, r);
    if (piece.readingIndex != static_cast<size_t>(&piece - line.pieces.data())) reordered = true;
    const bool first = r == 0 || logical[r - 1]->token != piece.token;
    const bool last = r + 1 == logical.size() || logical[r + 1]->token != piece.token;
    const size_t begin = first ? 0 : logical[r - 1]->end;
    EXPECT_EQ(piece.begin, begin) << "token " << piece.token;
    if (last) {
      EXPECT_EQ(piece.end, words[piece.token].size()) << "token " << piece.token;
    }
    EXPECT_LT(piece.begin, piece.end);
  }
  EXPECT_EQ(line.reordered, reordered);
}

// Each token's cut points (BidiLine::Token::cuts), as ParsedText hands over a shaped token's piece edges; a token past
// the list, or with an empty one, has none.
using Cuts = std::vector<std::vector<uint16_t>>;

// `glued` lists the tokens without a word space before them (TokenBoundary::hasSpace false); the rest have one.
BidiLine::Line lay(const std::vector<std::string>& words, const uint8_t level, const BidiLine::OpenControls& open = {},
                   const std::vector<size_t>& glued = {}, const Cuts& cuts = {}) {
  std::vector<BidiLine::Token> tokens;
  for (size_t t = 0; t < words.size(); t++) {
    BidiLine::Token token{words[t], std::find(glued.begin(), glued.end(), t) == glued.end()};
    if (t < cuts.size() && !cuts[t].empty()) {
      token.cutCount = static_cast<uint16_t>(cuts[t].size());
      token.cuts = cuts[t].data();
    }
    tokens.push_back(token);
  }
  BidiLine::Line line;
  EXPECT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), open, level, scratch(), line));
  expectWellFormed(line, words);
  return line;
}

std::string render(const BidiLine::Line& line) {
  std::string out;
  for (const auto& piece : line.pieces) {
    if (piece.gapBefore == Gap::Space) out += " S" + std::to_string(piece.spaceToken) + " ";
    if (piece.gapBefore == Gap::Glued) out += " G ";
    out += "t" + std::to_string(piece.token) + ":" + std::string(displayOf(line, piece));
  }
  return out;
}

std::vector<int> readings(const BidiLine::Line& line) {
  std::vector<int> r;
  for (const auto& piece : line.pieces) r.push_back(piece.readingIndex);
  return r;
}

std::vector<int> tokensOf(const BidiLine::Line& line) {
  std::vector<int> t;
  for (const auto& piece : line.pieces) t.push_back(piece.token);
  return t;
}

// Each piece's bytes [begin, end) of its token, left to right.
using Bytes = std::vector<std::pair<int, int>>;
Bytes bytesOf(const BidiLine::Line& line) {
  Bytes bytes;
  for (const auto& piece : line.pieces) bytes.emplace_back(piece.begin, piece.end);
  return bytes;
}

std::vector<int> segmentPiecesOf(const BidiLine::Line& line) {
  std::vector<int> counts;
  for (const auto& piece : line.pieces) counts.push_back(piece.segmentPieces);
  return counts;
}

// Token t's pieces, left to right, each as `form [begin,end) xN`: its drawn form, its bytes and its segmentPieces.
std::vector<std::string> piecesOfToken(const BidiLine::Line& line, const size_t t) {
  std::vector<std::string> out;
  for (const auto& piece : line.pieces) {
    if (piece.token != t) continue;
    out.push_back(std::string(displayOf(line, piece)) + " [" + std::to_string(piece.begin) + "," +
                  std::to_string(piece.end) + ") x" + std::to_string(piece.segmentPieces));
  }
  return out;
}

std::vector<std::string> repeated(const std::string& text, const size_t n) { return std::vector<std::string>(n, text); }

// The form tokenForm makes, or nothing when it returns false; checks that a false leaves `out` as it was.
std::optional<std::string> form(const std::string_view token, const uint8_t level,
                                const BidiLine::OpenControls& open = {}) {
  std::string out = "untouched";
  if (BidiLine::tokenForm(token, open, level, scratch(), out)) return out;
  EXPECT_EQ(out, "untouched");
  return std::nullopt;
}

const CP kPointedShalomDrawn = {0x05DD, 0x05D5, 0x05B9, 0x05DC, 0x05E9, 0x05B8, 0x05C1};

// While set, BidiUtils' heap workspace (the one nothrow allocation of its size) is refused.
bool refuseWorkspace = false;

}  // namespace

// malloc-backed, as libstdc++'s own; its operator delete frees with std::free.
void* operator new(const size_t bytes, const std::nothrow_t&) noexcept {
  if (refuseWorkspace && bytes == BidiUtils::kWorkspaceBytes) return nullptr;
  return std::malloc(bytes == 0 ? 1 : bytes);
}

TEST(BidiLine, AMixedLineInARightToLeftParagraph) {
  const auto line = lay({"שלום", "(world)", "1998."}, 1);
  EXPECT_EQ(render(line), "t2:.1998 S2 t1:(world) S1 t0:םולש");
  EXPECT_EQ(readings(line), (std::vector<int>{2, 1, 0}));
  EXPECT_TRUE(line.reordered);
  EXPECT_FALSE(line.fallback);
}

TEST(BidiLine, AMixedLineInALeftToRightParagraph) {
  const auto line = lay({"He", "said", "שלום."}, 0);
  EXPECT_EQ(render(line), "t0:He S1 t1:said S2 t2:םולש.");
  EXPECT_FALSE(line.reordered);
  ASSERT_EQ(line.pieces.size(), 3u);
  EXPECT_EQ(line.pieces[2].begin, 0u);
  EXPECT_EQ(line.pieces[2].end, 9u);
}

TEST(BidiLine, BracketsAroundHebrewInEnglish) {
  const auto line = lay({"The", "word", "(שלום)", "means"}, 0);
  EXPECT_EQ(render(line), "t0:The S1 t1:word S2 t2:(םולש) S3 t3:means");
}

TEST(BidiLine, TheMarkupParagraphOfTestRtlMarkup) {
  const auto line = lay({"מילה", u(FSI) + u(LRO) + "bdo", "text" + u(PDF) + u(PDI), "ו" + u(FSI) + "bdi",
                         "text" + u(PDI), "ו" + u(LRI) + "Latin", "123" + u(PDI)},
                        1);
  EXPECT_EQ(render(line), "t5:Latin S6 t6:123 G t5:ו S5 t3:bdi S4 t4:text G t3:ו S3 t1:bdo S2 t2:text S1 t0:הלימ");
  EXPECT_EQ(readings(line), (std::vector<int>{7, 8, 6, 4, 5, 3, 1, 2, 0}));
  ASSERT_EQ(line.pieces.size(), 9u);
  const BidiLine::Piece& vav = line.pieces[2];  // "ו" and its LRI
  const BidiLine::Piece& latin = line.pieces[0];
  EXPECT_EQ(vav.token, 5);
  EXPECT_EQ(vav.begin, 0u);
  EXPECT_EQ(vav.end, 5u);
  EXPECT_EQ(latin.token, 5);
  EXPECT_EQ(latin.begin, 5u);
  EXPECT_EQ(latin.end, 10u);
}

TEST(BidiLine, AnIsolateKeepsItsNeighboursOutside) {
  EXPECT_EQ(render(lay({u(RLI) + "שלום" + u(PDI) + ":", "3", "posts"}, 0)), "t0:םולש: S1 t1:3 S2 t2:posts");
}

TEST(BidiLine, AnEmbeddingLetsItsNeighboursIn) {
  const auto line = lay({u(RLE) + "שלום" + u(PDF) + ":", "3", "posts"}, 0);
  EXPECT_EQ(render(line), "t1:3 S1 t0::םולש S2 t2:posts");
  EXPECT_EQ(readings(line), (std::vector<int>{1, 0, 2}));
}

TEST(BidiLine, AnIsolateOpenAtTheLineStartIsOrderedOnItsOwn) {
  EXPECT_EQ(render(lay({"שלום" + u(PDI), "1998"}, 0, openWith({RLI}))), "t0:םולש S1 t1:1998");
  EXPECT_EQ(render(lay({"שלום" + u(PDI), "1998"}, 0)), "t1:1998 S1 t0:םולש");
}

TEST(BidiLine, AnIsolateInsideAWordCutsItWhereItsTextBreaks) {
  // see x<bdi>אב cd</bdi> more: "x" and "cd" meet on the line with "אב" between them in the text, so they are two
  // pieces, and the line keeps the line pass.
  const auto line = lay({"see", "x" + u(FSI) + "אבcd", "ef" + u(PDI), "more"}, 0);
  EXPECT_FALSE(line.fallback);
  EXPECT_EQ(render(line), "t0:see S1 t1:x G t1:cd S2 t2:ef G t1:בא S3 t3:more");
}

TEST(BidiLine, EmbeddingsInsideAWordCutItWhereItsTextBreaks) {
  const auto line = lay({"a" + u(RLE) + "א" + u(LRE) + "b", "c" + u(PDF) + u(PDF)}, 0);
  EXPECT_FALSE(line.fallback);
  EXPECT_EQ(render(line), "t0:a G t0:b S1 t1:c G t0:א");
}

TEST(BidiLine, AnFsiTakesItsDirectionFromOutsideItsNestedIsolates) {
  // The FSI's first strong letter outside its nested LRI is Hebrew, so its content runs right to left around the LRI.
  EXPECT_EQ(render(lay({u(FSI) + u(LRI) + "abc" + u(PDI), "אבג" + u(PDI), "x"}, 0)), "t1:גבא S1 t0:abc S2 t2:x");
}

TEST(BidiLine, AnOverrideOpenAtTheLineStartStillHolds) {
  EXPECT_EQ(render(lay({"ghi" + u(PDF), "jkl"}, 0, openWith({RLO}))), "t0:ihg S1 t1:jkl");
}

TEST(BidiLine, ALeftToRightMarkKeepsPlusSignsWithTheirLetter) {
  EXPECT_EQ(render(lay({"C++" + u(LRM), "שלום"}, 1)), "t1:םולש S1 t0:C++");
  EXPECT_EQ(render(lay({"C++", "שלום"}, 1)), "t1:םולש S1 t0:++C");
}

TEST(BidiLine, ALoneControlTokenGivesNoPiece) { EXPECT_EQ(render(lay({"abc", u(LRI), "def"}, 0)), "t0:abc S2 t2:def"); }

TEST(BidiLine, TwoSeparatorsWithNoPieceBetweenAreTheLaterTokensGap) {
  // Right to left the separators are met the other way round; the gap is still the logically later token's.
  EXPECT_EQ(render(lay({"אבג", u(RLI), "דהו"}, 1)), "t2:והד S2 t0:גבא");
}

TEST(BidiLine, NiqqudFollowsItsLetter) {
  const auto line = lay({"שָׁלוֹם"}, 1);
  ASSERT_EQ(line.pieces.size(), 1u);
  EXPECT_EQ(decode(displayOf(line, line.pieces[0])), kPointedShalomDrawn);
}

TEST(BidiLine, AMarkAfterAJoinerStaysOnItsBase) {
  // alef, ZWJ, qamats, bet, right to left: the qamats follows the alef it was written on, and the ZWJ is not drawn.
  const std::string word = "א" + u(0x200D) + u(0x05B8) + "ב";
  const CP drawn = {0x05D1, 0x05D0, 0x05B8};
  const auto line = lay({word}, 1);
  ASSERT_EQ(line.pieces.size(), 1u);
  EXPECT_EQ(decode(displayOf(line, line.pieces[0])), drawn);
  const auto own = form(word, 1);
  ASSERT_TRUE(own.has_value());
  EXPECT_EQ(decode(*own), drawn);
}

TEST(BidiLine, TheDivineNameIsDrawnAsHeAndGeresh) {
  const auto line = lay({"יהוה"}, 1);
  ASSERT_EQ(line.pieces.size(), 1u);
  EXPECT_EQ(line.pieces[0].begin, 0u);
  EXPECT_EQ(line.pieces[0].end, 8u);
  EXPECT_EQ(decode(displayOf(line, line.pieces[0])), (CP{0x05F3, 0x05D4}));
}

TEST(BidiLine, ANameSplitFromItsMarkFallsBackInTheLinePass) {
  // A coloured-cantillation edition can set a mark in a span of its own: here a token holding only U+0591, glued to
  // the divine name. ParsedText merges such a pair into one token before any line pass
  // (TextShaping::mergesWithPrevious, ShapedLayout.TheDivineNameGluedToAMarkIsDrawnAsHeAndGeresh), so a page never
  // hands the line pass this split. Handed it, the mark leads its token and keeps a slot, and orderLine's mark grouping
  // draws the base before it, the name's geresh, which pulls the geresh away from its he: a shaped cluster in two
  // pieces sends the line to the fallback, upstream's word order. Exempting leading marks from orderLine's grouping
  // would regress the style split instead, where a point in a span of its own must still follow its letter.
  const auto line = lay({"וַיֹּאמֶר", "יְהוָה", u(0x0591), "אֶל", "מֹשֶׁה"}, 1, {}, {2});
  EXPECT_TRUE(line.fallback);
}

TEST(BidiLine, MarksDoNotCountTowardTheLimit) {
  // 17 words of 7 code points (4 letters, 3 marks) and 16 spaces: 135 code points, 84 slots.
  const auto line = lay(repeated("שָׁלוֹם", 17), 1);
  EXPECT_FALSE(line.fallback);
  ASSERT_EQ(line.pieces.size(), 17u);
  for (size_t v = 0; v < 17; v++) {
    const BidiLine::Piece& piece = line.pieces[v];
    EXPECT_EQ(piece.token, 16 - v);
    EXPECT_EQ(decode(displayOf(line, piece)), kPointedShalomDrawn);
    if (v > 0) {
      EXPECT_EQ(piece.gapBefore, Gap::Space);
    }
  }
}

TEST(BidiLine, GluedTokensMeetWithoutASpace) {
  EXPECT_EQ(render(lay({"abc", "(שלום)"}, 0, {}, {1})), "t0:abc G t1:(םולש)");
  EXPECT_EQ(render(lay({"abc", "(שלום)"}, 1, {}, {1})), "t1:(םולש) G t0:abc");
  EXPECT_EQ(render(lay({"שלום", "עולם", "abc"}, 1, {}, {1})), "t2:abc S2 t1:םלוע G t0:םולש");
}

TEST(BidiLine, TheLimitIsCountedInSlots) {
  // 25 words of 4 letters, one of 3 and 25 spaces: 128 slots, still the line pass. One letter more is 129.
  auto words = repeated("אבגד", 25);
  words.push_back("אבג");
  const auto fits = lay(words, 1);
  EXPECT_FALSE(fits.fallback);
  EXPECT_EQ(fits.pieces.size(), 26u);
  words.back() = "אבגד";
  EXPECT_TRUE(lay(words, 1).fallback);
}

TEST(BidiLine, TheLimitIsCountedInUnitsToo) {
  // One letter with 383 points is one slot and 384 units, still the line pass; one point more falls back.
  std::string pointed = "ש";
  for (int i = 0; i < 383; i++) pointed += u(0x05B0 + i % 8);
  const auto fits = lay({pointed}, 1);
  EXPECT_FALSE(fits.fallback);
  ASSERT_EQ(fits.pieces.size(), 1u);
  EXPECT_EQ(displayOf(fits, fits.pieces[0]), pointed);  // a lone letter is its own visual order
  const auto over = lay({pointed + u(0x05B0)}, 1);
  EXPECT_TRUE(over.fallback);
  EXPECT_EQ(over.pieces.size(), 1u);
}

TEST(BidiLine, TheFallbackKeepsAGluedNeighboursGlue) {
  // 27 words: 133 slots. Token 6 is glued to token 5, so they meet without a space on the reversed line.
  const auto line = lay(repeated("אבגד", 27), 1, {}, {6});
  EXPECT_TRUE(line.fallback);
  std::string expected;
  for (int t = 26; t >= 0; t--) {
    if (t < 26) expected += t + 1 == 6 ? " G " : " S" + std::to_string(t + 1) + " ";
    expected += "t" + std::to_string(t) + ":דגבא";
  }
  EXPECT_EQ(render(line), expected);
}

TEST(BidiLine, ALineOverTheLimitFallsBackToUpstreamsOrder) {
  // 26 words of 4 letters and 25 spaces: 129 slots.
  const auto line = lay(repeated("אבגד", 26), 1);
  EXPECT_TRUE(line.fallback);
  EXPECT_TRUE(line.reordered);
  std::string expected;
  for (int t = 25; t >= 0; t--) {
    if (t < 25) expected += " S" + std::to_string(t + 1) + " ";
    expected += "t" + std::to_string(t) + ":דגבא";
  }
  EXPECT_EQ(render(line), expected);
}

TEST(BidiLine, TheFallbackReversesAnEmbeddedLatinPhrase) {
  // Upstream's corner, pinned so that a change shows: computeVisualWordOrder declines past 128 code points too.
  auto words = repeated("אבגד", 24);
  words.push_back("hello");
  words.push_back("world");
  const auto line = lay(words, 1);  // 131 slots
  EXPECT_TRUE(line.fallback);
  std::vector<int> expected;
  for (int t = 25; t >= 0; t--) expected.push_back(t);
  EXPECT_EQ(tokensOf(line), expected);
}

TEST(BidiLine, ALongLeftToRightLineKeepsItsOrder) {
  std::vector<std::string> words;
  for (int i = 0; i < 13; i++) {
    words.push_back("abcd");
    words.push_back("אבגד");
  }
  const auto line = lay(words, 0);  // 129 slots
  EXPECT_TRUE(line.fallback);
  EXPECT_FALSE(line.reordered);
  std::string expected;
  for (int t = 0; t < 26; t++) {
    if (t > 0) expected += " S" + std::to_string(t) + " ";
    expected += "t" + std::to_string(t) + (t % 2 == 0 ? ":abcd" : ":דגבא");
  }
  EXPECT_EQ(render(line), expected);
}

TEST(BidiLine, AFallbackLineReusesItsArrays) {
  // A line past the limit: the second line allocates no new views or order, and the views are the tokens' own text.
  const std::vector<std::string> words = repeated("אבגדהוזחטי", 26);
  std::vector<BidiLine::Token> tokens;
  for (const auto& w : words) tokens.push_back({w, true});
  BidiLine::Line line;
  ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), line));
  ASSERT_TRUE(line.fallback);
  ASSERT_EQ(line.fallbackViews.size(), 26u);
  EXPECT_EQ(line.fallbackViews.front().data(), words.front().data());
  const void* const order = line.fallbackOrder.data();
  const void* const views = line.fallbackViews.data();
  ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), line));
  EXPECT_EQ(line.fallbackOrder.data(), order);
  EXPECT_EQ(line.fallbackViews.data(), views);
  expectWellFormed(line, words);
}

TEST(BidiLine, AFallbackLineOrdersItsWordsAsIfTheirMarksWereNotThere) {
  // The same line with and without a point on each word (the fallback once stripped the marks from copies of the
  // words; now the order is computed over the words themselves): the same token order.
  const std::vector<std::string> plain = repeated("אבגדה", 26);
  std::vector<std::string> pointed = plain;
  pointed[3] = "אָבגדה";
  pointed[20] = "אבגְדה";
  pointed.push_back("hello");
  std::vector<std::string> plainTail = plain;
  plainTail.push_back("hello");
  const auto withMarks = lay(pointed, 1);
  const auto without = lay(plainTail, 1);
  ASSERT_TRUE(withMarks.fallback);
  ASSERT_TRUE(without.fallback);
  EXPECT_EQ(tokensOf(withMarks), tokensOf(without));
}

TEST(BidiLine, AFirstLineReservesForItsTokensNotTheWorstCase) {
  // 3 tokens of 10 bytes: about 3 pieces and 33 bytes of forms, not 4 bytes a code point and a piece a slot.
  const auto line = lay({"abcdefghij", "klmnopqrst", "uvwxyzabcd"}, 1);
  EXPECT_LE(line.pieces.capacity(), 3u);
  EXPECT_LT(line.display.capacity(), 66u);
}

// A token with cut points: ParsedText's shaped pieces (a run's glyphs, the Text around it), each a segment of the
// token. The tokens below are cut where their store pieces would be.

TEST(BidiLine, ATokenWithCutsGivesPiecesInVisualOrder) {
  // (كتاب): the brackets are Text, the word one run. Uncut the pass keeps the token whole; cut, each segment is a
  // piece of its own where UAX #9 puts it, the closing bracket leftmost and mirrored.
  const std::string word = "(كتاب)";
  EXPECT_EQ(render(lay({"قال", word}, 1)), "t1:(باتك) S1 t0:لاق");
  const auto line = lay({"قال", word}, 1, {}, {}, {{}, {1, 9}});
  EXPECT_FALSE(line.fallback);
  EXPECT_EQ(render(line), "t1:( G t1:باتك G t1:) S1 t0:لاق");
  EXPECT_EQ(readings(line), (std::vector<int>{3, 2, 1, 0}));
  EXPECT_EQ(bytesOf(line), (Bytes{{9, 10}, {1, 9}, {0, 1}, {0, 6}}));
  ASSERT_EQ(line.pieces.size(), 4u);
  for (size_t v = 1; v < 3; v++) {
    EXPECT_EQ(line.pieces[v].gapBefore, Gap::Glued);
    EXPECT_EQ(line.pieces[v].spaceToken, 1);
  }

  // Left to right after a Latin word the brackets take its direction, and the segments keep their logical order.
  const auto ltr = lay({"see", word}, 0, {}, {}, {{}, {1, 9}});
  EXPECT_EQ(render(ltr), "t0:see S1 t1:( G t1:باتك G t1:)");
  EXPECT_FALSE(ltr.reordered);
}

TEST(BidiLine, AnRtlTokensPunctuationPieceLandsLeft) {
  // كتاب،: the word's run, then the Arabic comma. Right to left the comma is drawn left of the word, glued to it.
  const auto line = lay({"كتاب،", "قلم"}, 1, {}, {}, {{8}});
  EXPECT_EQ(render(line), "t1:ملق S1 t0:، G t0:باتك");
  EXPECT_EQ(readings(line), (std::vector<int>{2, 1, 0}));
  EXPECT_EQ(bytesOf(line), (Bytes{{0, 6}, {8, 10}, {0, 8}}));
  // So do Arabic-Indic digits after the word, at level 2.
  EXPECT_EQ(render(lay({"كتاب١٩"}, 1, {}, {}, {{8}})), "t0:١٩ G t0:باتك");
}

TEST(BidiLine, AnLtrIndicTokenInAnRtlLineKeepsItsPiecesOrder) {
  // कख-गघ: two Devanagari runs and the hyphen between them, in a Hebrew line. The token is at level 2, so its pieces
  // run left to right in their logical order while the line runs right to left around it.
  const auto line = lay({"אבג", "कख-गघ", "דהו"}, 1, {}, {}, {{}, {6, 7}});
  EXPECT_EQ(render(line), "t2:והד S2 t1:कख G t1:- G t1:गघ S1 t0:גבא");
  EXPECT_EQ(readings(line), (std::vector<int>{4, 1, 2, 3, 0}));
  EXPECT_EQ(bytesOf(line), (Bytes{{0, 6}, {0, 6}, {6, 7}, {7, 13}, {0, 6}}));
}

TEST(BidiLine, AMarkAcrossACutTakesItsOwnSlot) {
  // A cut between bet and its qamats puts the point in another segment, so it does not ride on bet: it takes a slot
  // of its own, W1 gives it bet's level, and orderLine draws it right after bet. It is a piece of its own, glued
  // between bet and the alef before them in the text.
  const std::string word = "אב" + u(0x05B8);
  const auto whole = lay({word}, 1);
  ASSERT_EQ(whole.pieces.size(), 1u);
  EXPECT_EQ(decode(displayOf(whole, whole.pieces[0])), (CP{0x05D1, 0x05B8, 0x05D0}));

  const auto line = lay({word}, 1, {}, {}, {{4}});
  EXPECT_FALSE(line.fallback);
  ASSERT_EQ(line.pieces.size(), 3u);
  EXPECT_EQ(decode(displayOf(line, line.pieces[0])), CP{0x05D1});
  EXPECT_EQ(decode(displayOf(line, line.pieces[1])), CP{0x05B8});
  EXPECT_EQ(decode(displayOf(line, line.pieces[2])), CP{0x05D0});
  EXPECT_EQ(bytesOf(line), (Bytes{{2, 4}, {4, 6}, {0, 2}}));
  EXPECT_EQ(readings(line), (std::vector<int>{1, 2, 0}));
  EXPECT_EQ(line.pieces[1].gapBefore, Gap::Glued);
  EXPECT_EQ(line.pieces[2].gapBefore, Gap::Glued);
  // The qamats lands inside the first segment, which gives two pieces: a caller that draws that segment as one form
  // (a run's glyphs) reads 2 and must degrade it.
  EXPECT_EQ(segmentPiecesOf(line), (std::vector<int>{2, 1, 2}));

  // A joiner across a cut takes its own slot too. It draws nothing, and its bytes stay with its own segment.
  const auto joined = lay({"ب" + u(0x200C) + "ه"}, 1, {}, {}, {{2}});
  EXPECT_FALSE(joined.fallback);
  EXPECT_EQ(render(joined), "t0:ه G t0:ب");
  EXPECT_EQ(bytesOf(joined), (Bytes{{2, 7}, {0, 2}}));
}

TEST(BidiLine, AnUndrawnSegmentIsAbsorbed) {
  // A segment that draws nothing (a joiner before a run, a lone control) gives no piece: its bytes join the piece
  // before it in the text, or, at the token's start, the first one.
  const auto leading = lay({u(0x200C) + "كتاب"}, 1, {}, {}, {{3}});
  EXPECT_EQ(render(leading), "t0:باتك");
  EXPECT_EQ(bytesOf(leading), (Bytes{{0, 11}}));
  const std::string word = "كتاب" + u(0x200F) + "،";
  const auto inner = lay({word}, 1, {}, {}, {{8, 11}});
  EXPECT_EQ(render(inner), "t0:، G t0:باتك");
  EXPECT_EQ(bytesOf(inner), (Bytes{{11, 13}, {0, 11}}));
  // A segment's own undrawn bytes before its first drawn character stay in its piece.
  const auto own = lay({word}, 1, {}, {}, {{8}});
  EXPECT_EQ(render(own), "t0:، G t0:باتك");
  EXPECT_EQ(bytesOf(own), (Bytes{{8, 13}, {0, 8}}));
}

TEST(BidiLine, TheFallbackCutsATokenIntoItsSegments) {
  // Level 1: 26 words, then (كتاب) cut at its brackets with an RLM after it (a segment that draws nothing): 137
  // slots. The token's segments are pieces in the order its own form gives them; the RLM's bytes join the bracket's.
  auto rtl = repeated("אבגד", 26);
  rtl.push_back("(كتاب)" + u(0x200F));
  Cuts rtlCuts(27);
  rtlCuts[26] = {1, 9, 10};
  const auto line = lay(rtl, 1, {}, {}, rtlCuts);
  EXPECT_TRUE(line.fallback);
  std::string expected = "t26:( G t26:باتك G t26:)";
  for (int t = 25; t >= 0; t--) expected += " S" + std::to_string(t + 1) + " t" + std::to_string(t) + ":דגבא";
  EXPECT_EQ(render(line), expected);
  const Bytes bytes = bytesOf(line);
  ASSERT_EQ(bytes.size(), 29u);
  EXPECT_EQ(Bytes(bytes.begin(), bytes.begin() + 3), (Bytes{{9, 13}, {1, 9}, {0, 1}}));

  // Level 0: 26 Latin words, then abcكتاب١٢ cut at its run: 139 slots. The token is at level 0, and its digits (level
  // 2) land between its Latin letters and its word.
  auto ltr = repeated("abcd", 26);
  ltr.push_back("abcكتاب١٢");
  Cuts ltrCuts(27);
  ltrCuts[26] = {3, 11};
  const auto left = lay(ltr, 0, {}, {}, ltrCuts);
  EXPECT_TRUE(left.fallback);
  EXPECT_TRUE(left.reordered);
  std::string expectedLeft;
  for (int t = 0; t < 26; t++) expectedLeft += "t" + std::to_string(t) + ":abcd S" + std::to_string(t + 1) + " ";
  expectedLeft += "t26:abc G t26:١٢ G t26:باتك";
  EXPECT_EQ(render(left), expectedLeft);

  // A token past 128 code points has no form of its own (orderLine refuses it): its segments are in logical order at
  // level 0 and reversed at level 1, each formed alone (a bracket mirrored, a run past the limit as it is).
  std::string run;
  for (int i = 0; i < 130; i++) run += "ب";
  const auto longRtl = lay({"(" + run + ")"}, 1, {}, {}, {{1, 261}});
  EXPECT_TRUE(longRtl.fallback);
  EXPECT_EQ(render(longRtl), "t0:( G t0:" + run + " G t0:)");
  EXPECT_EQ(bytesOf(longRtl), (Bytes{{261, 262}, {1, 261}, {0, 1}}));
  std::string indic;
  for (int i = 0; i < 65; i++) indic += "कख";
  const auto longLtr = lay({indic + "-गघ"}, 0, {}, {}, {{390, 391}});
  EXPECT_TRUE(longLtr.fallback);
  EXPECT_EQ(render(longLtr), "t0:" + indic + " G t0:- G t0:गघ");
  EXPECT_EQ(bytesOf(longLtr), (Bytes{{0, 390}, {390, 391}, {391, 397}}));
}

TEST(BidiLine, TheFallbackGluesOneTokensPieces) {
  // 26 words, token 5 كتاب، cut at its comma and token 6 glued to it: 129 slots. The token's two pieces are glued to
  // each other, and it keeps its glue to token 6 and its word space from token 4.
  auto words = repeated("אבגד", 26);
  words[5] = "كتاب،";
  Cuts cuts(26);
  cuts[5] = {8};
  const auto line = lay(words, 1, {}, {6}, cuts);
  EXPECT_TRUE(line.fallback);
  std::string expected;
  for (int t = 25; t >= 0; t--) {
    if (t < 25) expected += t + 1 == 6 ? " G " : " S" + std::to_string(t + 1) + " ";
    expected += t == 5 ? "t5:، G t5:باتك" : "t" + std::to_string(t) + ":דגבא";
  }
  EXPECT_EQ(render(line), expected);
  const auto comma = std::find_if(line.pieces.begin(), line.pieces.end(), [](const auto& p) { return p.token == 5; });
  ASSERT_LT(comma + 1, line.pieces.end());
  EXPECT_EQ(comma[1].token, 5);
  EXPECT_EQ(comma[1].gapBefore, Gap::Glued);
  EXPECT_EQ(comma[1].spaceToken, 5);
}

TEST(BidiLine, APieceCountsThePiecesThatDrawItsSegment) {
  // Each segment of (كتاب) is one piece.
  EXPECT_EQ(segmentPiecesOf(lay({"قال", "(كتاب)"}, 1, {}, {}, {{}, {1, 9}})), (std::vector<int>{1, 1, 1, 1}));
  // A token without cuts is one segment: an isolate inside it leaves it in three pieces.
  const auto split = lay({"see", "x" + u(FSI) + "אבcd", "ef" + u(PDI), "more"}, 0);
  EXPECT_EQ(render(split), "t0:see S1 t1:x G t1:cd S2 t2:ef G t1:בא S3 t3:more");
  EXPECT_EQ(segmentPiecesOf(split), (std::vector<int>{1, 3, 3, 1, 3, 1}));
  // On the fallback each of its segments is one piece too.
  auto words = repeated("אבגד", 26);
  words.push_back("(كتاب)");
  Cuts cuts(27);
  cuts[26] = {1, 9};
  const auto fallback = lay(words, 1, {}, {}, cuts);
  ASSERT_TRUE(fallback.fallback);
  EXPECT_EQ(segmentPiecesOf(fallback), std::vector<int>(29, 1));
}

TEST(BidiLine, TheFallbackSplitsASegmentWhereAnotherInterruptsIt) {
  // "a", U+0663 and kitab, cut at the run: a Text segment, then the run. Left to right, UAX #9 puts "a" (level 0) left
  // of the run (level 1) and the digit (level 2) right of it: the Text segment's characters land apart, and the line
  // pass gives it a piece on each side, each counting 2. Past 128 slots the fallback gives the same pieces, not the
  // segment's characters together where its form first draws it (the digit on the run's other side).
  const std::string kitab = u(0x0643) + u(0x062A) + u(0x0627) + u(0x0628);
  const std::string drawnKitab = u(0x0628) + u(0x0627) + u(0x062A) + u(0x0643);
  const std::string token = "a" + u(0x0663) + kitab;
  const std::vector<std::string> expected = {"a [0,1) x2", drawnKitab + " [3,11) x1", u(0x0663) + " [1,3) x2"};
  const auto main = lay({"see", token}, 0, {}, {}, {{}, {3}});
  ASSERT_FALSE(main.fallback);
  EXPECT_EQ(piecesOfToken(main, 1), expected);

  auto words = repeated("abcd", 26);
  words.push_back(token);
  Cuts cuts(27);
  cuts[26] = {3};
  const auto fallback = lay(words, 0, {}, {}, cuts);
  ASSERT_TRUE(fallback.fallback);
  EXPECT_EQ(piecesOfToken(fallback, 26), expected);
  const std::string rendered = render(fallback);
  const std::string tail = " S26 t26:a G t26:" + drawnKitab + " G t26:" + u(0x0663);
  ASSERT_GE(rendered.size(), tail.size());
  EXPECT_EQ(rendered.substr(rendered.size() - tail.size()), tail);
  // A fresh line's buffers are reserved to the fallback's bound, and nothing grew them past it (growth would double
  // them): a piece and a 4-byte form with its NUL for each word; for the cut token a piece for each of its six drawn
  // code points, its 11 bytes and a NUL a piece.
  EXPECT_EQ(fallback.pieces.capacity(), 26u + 6u);
  EXPECT_EQ(fallback.display.capacity(), 26u * 5 + 11 + 6);
}

TEST(BidiLine, TheFallbackCountsTheTwoPiecesOfARunAMarkInterrupts) {
  // Beh teh, then a fatha across a cut, right to left: the mark is drawn right after its base, the teh, inside the
  // run's segment, which so gives a piece on each side of it. Past 128 slots the fallback gives the same pieces as the
  // line pass and counts the run's 2, so a caller that draws the run as its glyphs degrades it rather than drawing them
  // on each.
  const std::string word = u(0x0628) + u(0x062A) + u(0x064E);
  const std::vector<std::string> expected = {u(0x062A) + " [2,4) x2", u(0x064E) + " [4,6) x1", u(0x0628) + " [0,2) x2"};
  const auto main = lay({word}, 1, {}, {}, {{4}});
  ASSERT_FALSE(main.fallback);
  EXPECT_EQ(piecesOfToken(main, 0), expected);

  auto words = repeated(u(0x05D0) + u(0x05D1) + u(0x05D2) + u(0x05D3), 26);
  words.push_back(word);
  Cuts cuts(27);
  cuts[26] = {4};
  const auto fallback = lay(words, 1, {}, {}, cuts);
  ASSERT_TRUE(fallback.fallback);
  EXPECT_EQ(piecesOfToken(fallback, 26), expected);
  ASSERT_EQ(fallback.pieces[0].token, 26);
  EXPECT_GT(fallback.pieces[0].segmentPieces, 1);
  EXPECT_EQ(fallback.pieces[1].gapBefore, Gap::Glued);
  EXPECT_EQ(fallback.pieces[2].gapBefore, Gap::Glued);
}

TEST(BidiLine, CutsThatBreakTheirContractLeaveTheTokenUncut) {
  // Unsorted, repeated, at the token's start or end, past it, or inside a code point (byte 2 is kaf's second): each
  // token is laid out as without cuts.
  const std::string word = "(كتاب)";  // 10 bytes
  const auto uncut = lay({"قال", word}, 1);
  const std::vector<std::vector<uint16_t>> broken = {{9, 1}, {1, 1}, {0, 9}, {1, 10}, {1, 400}, {2}};
  for (const auto& cuts : broken) {
    const BidiLine::Token token{word, true, static_cast<uint16_t>(cuts.size()), cuts.data()};
    EXPECT_FALSE(BidiLine::cutsAreValid(token)) << cuts[0];
    const auto line = lay({"قال", word}, 1, {}, {}, {{}, cuts});
    EXPECT_EQ(render(line), render(uncut)) << cuts[0];
    EXPECT_EQ(bytesOf(line), bytesOf(uncut)) << cuts[0];
  }
  const std::vector<uint16_t> kept = {1, 9};
  EXPECT_TRUE(BidiLine::cutsAreValid({word, true, 2, kept.data()}));
  EXPECT_TRUE(BidiLine::cutsAreValid({word, true}));
  EXPECT_FALSE(BidiLine::cutsAreValid({word, true, 1, nullptr}));

  // Past the limit, where each segment would be formed from its own bytes: a cut far past the token's end leaves its
  // form the uncut one, and nothing past the text is read.
  std::string run;
  for (int i = 0; i < 130; i++) run += "ب";
  const std::string longWord = "(" + run + ")";
  const auto longUncut = lay({longWord}, 1);
  const auto longLine = lay({longWord}, 1, {}, {}, {{1, 60000}});
  EXPECT_TRUE(longLine.fallback);
  EXPECT_EQ(render(longLine), render(longUncut));
  EXPECT_EQ(bytesOf(longLine), (Bytes{{0, 262}}));
}

TEST(BidiLine, TheOpenControlsFollowTheText) {
  BidiLine::OpenControls open;
  open.advance(u(RLI) + "abc");
  EXPECT_EQ(openCps(open), (CP{RLI}));
  open.advance("def" + u(PDI));
  EXPECT_EQ(openCps(open), CP{});

  open.advance(u(RLE) + "x" + u(LRI) + "y");
  EXPECT_EQ(openCps(open), (CP{RLE, LRI}));
  open.advance(u(PDF));  // a PDF does not close an isolate
  EXPECT_EQ(openCps(open), (CP{RLE, LRI}));
  open.advance(u(PDI));
  EXPECT_EQ(openCps(open), (CP{RLE}));
  open.advance(u(PDF));
  EXPECT_EQ(openCps(open), CP{});

  BidiLine::OpenControls none;
  none.advance(u(PDI));
  EXPECT_EQ(openCps(none), CP{});

  BidiLine::OpenControls deep;
  for (int i = 0; i < 10; i++) deep.advance(u(RLE));
  EXPECT_EQ(deep.count, 8);
  EXPECT_EQ(deep.overflow, 2);
  for (int i = 0; i < 2; i++) deep.advance(u(PDF));
  EXPECT_EQ(deep.count, 8);
  EXPECT_EQ(deep.overflow, 0);
  for (int i = 0; i < 8; i++) deep.advance(u(PDF));
  EXPECT_EQ(deep.count, 0);
}

TEST(BidiLine, ATokensOwnFormIsMadeAtItsOwnDirection) {
  EXPECT_EQ(form("abc(שלום)", 1), "abc(םולש)");
  EXPECT_EQ(form("שלום", 0), "םולש");
  EXPECT_EQ(form("abc", 1), std::nullopt);
  const auto name = form("יהוה", 1);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(decode(*name), (CP{0x05F3, 0x05D4}));
  EXPECT_EQ(form("abc", 0, openWith({RLO})), "cba");
  EXPECT_EQ(form("C++" + u(LRM), 1), "C++");

  // An open isolate gives the level and is never prepended (prepending the RLI gives ")םולש abc(", the review's B4).
  EXPECT_EQ(form("abc(שלום)", 0, openWith({RLI})), "(םולש)abc");
  EXPECT_EQ(form("abc", 0, openWith({RLI, RLO})), "cba");
  EXPECT_EQ(form("abc", 0, openWith({RLO, LRI})), std::nullopt);  // the override outside the isolate stays outside
}

TEST(BidiLine, DropControlsLeavesTheText) {
  std::string text = "a" + u(LRM) + "b" + u(RLI) + "c" + u(PDF) + "d";
  EXPECT_TRUE(BidiLine::dropControls(text));
  EXPECT_EQ(text, "abcd");
  std::string plain = "abc";
  EXPECT_FALSE(BidiLine::dropControls(plain));
  EXPECT_EQ(plain, "abc");

  for (const uint32_t cp : {LRM, 0x200Fu, 0x061Cu, LRE, RLE, PDF, LRO, RLO, LRI, RLI, FSI, PDI}) {
    EXPECT_TRUE(BidiLine::isControl(cp)) << std::hex << cp;
  }
  for (const uint32_t cp : {0x200Cu, 0x200Du, 0x00ADu, 0x0020u, 0x05D0u}) {
    EXPECT_FALSE(BidiLine::isControl(cp)) << std::hex << cp;
  }
}

TEST(BidiLine, WithoutAWorkspaceTheCallerLaysTheLineOutWithoutBidi) {
  BidiUtils::releaseWorkspace();
  refuseWorkspace = true;
  const auto layOut = [](const std::vector<std::string>& words, BidiLine::Line& line) {
    std::vector<BidiLine::Token> tokens;
    for (const auto& w : words) tokens.push_back({w, true});
    return BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), line);
  };
  BidiLine::Line line;
  EXPECT_FALSE(layOut({"שלום", "(world)", "1998."}, line));
  EXPECT_TRUE(line.pieces.empty());
  EXPECT_FALSE(layOut(repeated("אבגד", 26), line));  // past the limit too
  EXPECT_TRUE(line.pieces.empty());
  EXPECT_TRUE(line.display.empty());
  EXPECT_FALSE(line.fallback);
  EXPECT_EQ(form("שלום", 1), std::nullopt);  // its form is its text
  refuseWorkspace = false;
  EXPECT_TRUE(layOut(repeated("אבגד", 26), line));
  EXPECT_TRUE(line.fallback);
  BidiUtils::releaseWorkspace();
}

TEST(BidiLine, TheArenaLendsTheLineItsTokensWithTheScratch) {
  BidiUtils::releaseWorkspace();
  const std::vector<std::string> words = {"שלום", "(world)", "1998."};
  BuildArena arena(16 * 1024);
  ASSERT_TRUE(arena.valid());
  const size_t before = arena.used();
  {
    const BidiLine::ArenaWorkspace lent(&arena, words.size());
    ASSERT_NE(lent.scratch(), nullptr);
    ASSERT_NE(lent.tokens(), nullptr);
    EXPECT_GE(arena.used(),
              before + BidiUtils::kWorkspaceBytes + sizeof(BidiLine::Scratch) + words.size() * sizeof(BidiLine::Token));
    for (size_t t = 0; t < words.size(); t++) new (lent.tokens() + t) BidiLine::Token{words[t], true};
    BidiLine::Line line;
    ASSERT_TRUE(BidiLine::layoutLine(lent.tokens(), words.size(), {}, 1, *lent.scratch(), line));
    EXPECT_EQ(render(line), "t2:.1998 S2 t1:(world) S1 t0:םולש");
  }
  EXPECT_EQ(arena.used(), before);

  // None asked for: none lent.
  {
    const BidiLine::ArenaWorkspace lent(&arena, 0);
    ASSERT_NE(lent.scratch(), nullptr);
    EXPECT_EQ(lent.tokens(), nullptr);
  }
  EXPECT_EQ(arena.used(), before);

  // All or nothing: an arena with room for the scratch but not for the tokens lends neither, and holds nothing.
  BuildArena tight(BidiUtils::kWorkspaceBytes + sizeof(BidiLine::Scratch) + 64);
  {
    const BidiLine::ArenaWorkspace lent(&tight, 1000);
    EXPECT_EQ(lent.scratch(), nullptr);
    EXPECT_EQ(lent.tokens(), nullptr);
    EXPECT_EQ(tight.used(), 0u);
  }
  BidiUtils::releaseWorkspace();
}

TEST(BidiLine, AnArenaLendsTheWorkspaceAndScratch) {
  BidiUtils::releaseWorkspace();
  const std::vector<std::string> words = {"שלום", "(world)", "1998."};
  std::vector<BidiLine::Token> tokens;
  for (const auto& w : words) tokens.push_back({w, true});

  BuildArena arena(16 * 1024);
  ASSERT_TRUE(arena.valid());
  ASSERT_NE(arena.alloc(100), nullptr);  // something the build already holds
  const size_t before = arena.used();
  {
    const BidiLine::ArenaWorkspace lent(&arena);
    ASSERT_NE(lent.scratch(), nullptr);
    EXPECT_GT(arena.used(), before);
    BidiLine::Line line;
    ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, *lent.scratch(), line));
    EXPECT_EQ(render(line), "t2:.1998 S2 t1:(world) S1 t0:םולש");
    EXPECT_FALSE(BidiUtils::heapWorkspaceHeld());
  }
  EXPECT_EQ(arena.used(), before);

  // The binding ended with the scope: the next pass allocates the heap workspace.
  BidiLine::Line line;
  ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), line));
  EXPECT_TRUE(BidiUtils::heapWorkspaceHeld());
  BidiUtils::releaseWorkspace();

  BuildArena small(1024);  // not even the workspace fits
  {
    const BidiLine::ArenaWorkspace lent(&small);
    EXPECT_EQ(lent.scratch(), nullptr);
  }
  EXPECT_EQ(small.used(), 0u);

  BuildArena medium(4 * 1024);  // the workspace fits, the scratch does not
  {
    const BidiLine::ArenaWorkspace lent(&medium);
    EXPECT_EQ(lent.scratch(), nullptr);
    EXPECT_EQ(medium.used(), 0u);
  }

  const BidiLine::ArenaWorkspace none(nullptr);
  EXPECT_EQ(none.scratch(), nullptr);

  // A scope that ends while a newer block is still open cannot release (logged); it still unbinds the workspace.
  BuildArena nested(16 * 1024);
  BuildArena::Block newer;
  {
    const BidiLine::ArenaWorkspace lent(&nested);
    ASSERT_NE(lent.scratch(), nullptr);
    newer = nested.reserveBlock();
  }
  EXPECT_EQ(nested.releaseFailures(), 1u);
  EXPECT_TRUE(nested.release(newer));
  ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), line));
  EXPECT_TRUE(BidiUtils::heapWorkspaceHeld());
  BidiUtils::releaseWorkspace();
}

// The largest free block the pass's heap checks read (the shim's ESP.getMaxAllocHeap()), for a scope. Nothing is
// refused by it: only the checks see it.
class LargestFreeBlock {
 public:
  explicit LargestFreeBlock(const uint32_t bytes) : saved_(ESP.getMaxAllocHeap()) { ESP.setMaxAllocHeap(bytes); }
  ~LargestFreeBlock() { ESP.setMaxAllocHeap(saved_); }
  LargestFreeBlock(const LargestFreeBlock&) = delete;
  LargestFreeBlock& operator=(const LargestFreeBlock&) = delete;

 private:
  uint32_t saved_;
};

std::vector<BidiLine::Token> tokensOf(const std::vector<std::string>& words) {
  std::vector<BidiLine::Token> tokens;
  for (const auto& w : words) tokens.push_back({w, true});
  return tokens;
}

// The Line's buffers grow behind the heap check (HeapGrowth.h): a line the heap refuses them for is reported as not
// laid out, clear, for the caller's path without bidi. A Line that already holds them needs nothing from the heap.
TEST(BidiLine, ALineTheHeapRefusesItsBuffersIsLaidOutWithoutBidi) {
  const std::vector<std::string> words = {"abc", "אבג", "def"};
  const auto tokens = tokensOf(words);
  BidiLine::Line fresh;
  {
    const LargestFreeBlock none(0);
    EXPECT_FALSE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 0, scratch(), fresh));
  }
  EXPECT_TRUE(fresh.pieces.empty());
  EXPECT_TRUE(fresh.display.empty());

  BidiLine::Line held;
  ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 0, scratch(), held));
  const std::string once = render(held);
  {
    const LargestFreeBlock none(0);
    ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 0, scratch(), held));
  }
  EXPECT_EQ(once, render(held));
  expectWellFormed(held, words);
}

// The fallback's copies, order and pieces the same: refused, the line is clear; held from an earlier fallback line,
// nothing is asked of the heap.
TEST(BidiLine, AFallbackLineTheHeapRefusesItsBuffersIsLaidOutWithoutBidi) {
  const std::vector<std::string> words = repeated("אבגד", 26);  // 129 slots: over the limit
  const auto tokens = tokensOf(words);
  BidiLine::Line fresh;
  {
    const LargestFreeBlock none(0);
    EXPECT_FALSE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), fresh));
  }
  EXPECT_TRUE(fresh.pieces.empty());
  EXPECT_FALSE(fresh.fallback);
  EXPECT_TRUE(fresh.fallbackViews.empty());

  BidiLine::Line held;
  ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), held));
  ASSERT_TRUE(held.fallback);
  const std::string once = render(held);
  {
    const LargestFreeBlock none(0);
    ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), held));
  }
  EXPECT_TRUE(held.fallback);
  EXPECT_EQ(once, render(held));
  expectWellFormed(held, words);
}
