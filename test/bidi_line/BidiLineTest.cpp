// BidiLineTest: the line pass (BidiLine::layoutLine) that turns one line's tokens into pieces in visual order, the
// controls open across lines, a token's own drawn form, and the workspace a build's arena lends.
//
// A line is rendered as `t<token>:<display>` items, left to right, joined by ` S<n> ` for a word space (token n's)
// and ` G ` for a glued boundary. Expected values were checked against minibidi on 2026-11-02.

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

// `glued` lists the tokens without a word space before them (TokenBoundary::hasSpace false); the rest have one.
BidiLine::Line lay(const std::vector<std::string>& words, const uint8_t level, const BidiLine::OpenControls& open = {},
                   const std::vector<size_t>& glued = {}) {
  std::vector<BidiLine::Token> tokens;
  for (size_t t = 0; t < words.size(); t++) {
    tokens.push_back({words[t], std::find(glued.begin(), glued.end(), t) == glued.end()});
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

TEST(BidiLine, AFallbackLineReusesItsCopies) {
  // Words past the short-string buffer, on a line past the limit: the second line allocates no new copies.
  const std::vector<std::string> words = repeated("אבגדהוזחטי", 26);
  std::vector<BidiLine::Token> tokens;
  for (const auto& w : words) tokens.push_back({w, true});
  BidiLine::Line line;
  ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), line));
  ASSERT_TRUE(line.fallback);
  ASSERT_EQ(line.fallbackWords.size(), 26u);
  const void* const order = line.fallbackOrder.data();
  const void* const firstWord = line.fallbackWords.front().data();
  const void* const lastWord = line.fallbackWords.back().data();
  ASSERT_TRUE(BidiLine::layoutLine(tokens.data(), tokens.size(), {}, 1, scratch(), line));
  EXPECT_EQ(line.fallbackOrder.data(), order);
  EXPECT_EQ(line.fallbackWords.front().data(), firstWord);
  EXPECT_EQ(line.fallbackWords.back().data(), lastWord);
  expectWellFormed(line, words);
}

TEST(BidiLine, AFirstLineReservesForItsTokensNotTheWorstCase) {
  // 3 tokens of 10 bytes: about 3 pieces and 33 bytes of forms, not 4 bytes a code point and a piece a slot.
  const auto line = lay({"abcdefghij", "klmnopqrst", "uvwxyzabcd"}, 1);
  EXPECT_LE(line.pieces.capacity(), 3u);
  EXPECT_LT(line.display.capacity(), 66u);
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
