// The CJK line-breaking rules (spec section 9.1), checked on every line the host pipeline lays out
// for the CJK corpus books: kinsoku at the edges of the lines the layout broke, no hyphen in CJK
// text, and every line laid out again from its dump: each CJK mark at the width its book's punctuation table gives
// it, each CJK-Latin gap, and push-in on the lines that needed it (spec sections 5.2-5.4).
//
// The books run left-aligned, so every gap on a line is its natural gap: zero at a CJK gap or a
// glued seam, the space advance at a word gap. Each book's paragraphs open with an indented first
// line (CSS text-indent), which tells a paragraph's first line from a line the layout broke.
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/CjkLineBreak.h"
#include "Epub/CjkPunctuation.h"
#include "Epub/TypographyProfile.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

struct DumpWord {
  int x = 0;
  int style = 0;
  int sizePct = 100;
  std::string text;
};

// A ruby group: the box over words [first, first + count), at x for w (the `R` record).
struct DumpRuby {
  size_t first = 0;
  size_t count = 0;
  int x = 0;
  int w = 0;
  std::string text;
};

struct DumpLine {
  std::vector<DumpWord> words;
  std::vector<DumpRuby> rubies;
  int x = 0;                    // the LINE record's x= (0 for a table cell line)
  float sizeMultiplier = 1.0f;  // the block's font-size multiplier (LINE mult=); table cell lines have none
  bool followsLine = false;     // the line before it in `lines` is the previous line of the same flow
  std::string shape;            // x, alignment and size of the LINE record: lines of one shape share a text width
  bool justified = false;       // align=0
  int align = -1;               // the LINE record's align= (0 justified, 1 left, 2 right); -1 for a cell's LN
  bool cell = false;            // a table cell line (LN record)
  int y = 0;
};

uint32_t firstCodepoint(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  return utf8NextCodepoint(&p);
}

uint32_t lastCodepoint(const std::string& text) {
  size_t i = text.size();
  while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xC0) == 0x80) --i;
  if (i == 0) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str() + i - 1);
  return utf8NextCodepoint(&p);
}

// The text lines of a dump in reading order: body lines (LINE) and table cell lines (LN). A spine,
// table, row or cell starts a new flow; a page break does not.
std::vector<DumpLine> parseLines(const std::string& dump) {
  std::vector<DumpLine> lines;
  bool inFlow = false;
  std::istringstream in(dump);
  std::string raw;
  while (std::getline(in, raw)) {
    const size_t start = raw.find_first_not_of(' ');
    if (start == std::string::npos) continue;
    const std::string line = raw.substr(start);
    if (line.rfind("LINE ", 0) == 0 || line.rfind("LN ", 0) == 0) {
      DumpLine next;
      next.followsLine = inFlow;
      next.cell = line.rfind("LN ", 0) == 0;
      const size_t mult = line.find(" mult=");
      if (mult != std::string::npos) next.sizeMultiplier = std::stof(line.substr(mult + 6));
      std::istringstream record(line);
      std::string field;
      std::string x;
      std::string align;
      std::string size;
      while (record >> field) {
        if (field.rfind("x=", 0) == 0) x = field;
        if (field.rfind("align=", 0) == 0) align = field;
        if (field.rfind("mult=", 0) == 0) size = field;
        if (field.rfind("y=", 0) == 0) next.y = std::stoi(field.substr(2));
      }
      if (!next.cell && !x.empty()) next.x = std::stoi(x.substr(2));
      next.shape = x + " " + align + " " + size;
      next.justified = align == "align=0";
      if (!next.cell && !align.empty()) next.align = std::stoi(align.substr(6));
      lines.push_back(next);
      inFlow = true;
    } else if (line.rfind("W x=", 0) == 0 && !lines.empty()) {
      DumpWord word;
      const size_t textAt = line.find(" t=");
      std::istringstream fields(line.substr(0, textAt));
      std::string field;
      while (fields >> field) {
        if (field.rfind("x=", 0) == 0) word.x = std::stoi(field.substr(2));
        if (field.rfind("s=", 0) == 0) word.style = std::stoi(field.substr(2));
        if (field.rfind("z=", 0) == 0) word.sizePct = std::stoi(field.substr(2));
      }
      word.text = textAt == std::string::npos ? "" : line.substr(textAt + 3);
      lines.back().words.push_back(word);
    } else if (line.rfind("R first=", 0) == 0 && !lines.empty()) {
      DumpRuby ruby;
      const size_t textAt = line.find(" t=");
      std::istringstream fields(line.substr(0, textAt));
      std::string field;
      while (fields >> field) {
        if (field.rfind("first=", 0) == 0) ruby.first = std::stoul(field.substr(6));
        if (field.rfind("count=", 0) == 0) ruby.count = std::stoul(field.substr(6));
        if (field.rfind("x=", 0) == 0) ruby.x = std::stoi(field.substr(2));
        if (field.rfind("w=", 0) == 0) ruby.w = std::stoi(field.substr(2));
      }
      ruby.text = textAt == std::string::npos ? "" : line.substr(textAt + 3);
      lines.back().rubies.push_back(ruby);
    } else if (line.rfind("SPINE ", 0) == 0 || line.rfind("TABLE ", 0) == 0 || line.rfind("ROW ", 0) == 0 ||
               line.rfind("CELL ", 0) == 0) {
      inFlow = false;
    }
  }
  return lines;
}

std::string lineText(const DumpLine& line) {
  std::string text;
  for (const auto& w : line.words) text += w.text + "|";
  return text;
}

// As ParsedText measures a word: the block's multiplier times the word's own size, rounded.
int wordWidth(const GfxRenderer& renderer, const DumpLine& line, const DumpWord& w) {
  const int raw = renderer.getTextAdvanceX(1, w.text.c_str(), static_cast<EpdFontFamily::Style>(w.style));
  const float scale = line.sizeMultiplier * (w.sizePct / 100.0f);
  return scale == 1.0f ? raw : static_cast<int>(raw * scale + 0.5f);
}

constexpr int kAlignLeft = 1;
constexpr int kAlignRight = 2;

// A line laid out again from its dump: ParsedText's rules, restated (spec 5.2-5.4). Each token has a box (a CJK mark
// its table width, anything else its natural width) and each boundary a gap; push-in takes width off the line-edge
// marks, then the other marks, then the CJK-Latin gaps, as much as the line's need says. The dump gives token 0's
// position and every position where the rules do not say (a word space, a source space beside Han and Latin).
class LineLayout {
 public:
  LineLayout(const GfxRenderer& renderer, const DumpLine& line, const TypographyProfile& profile)
      : renderer_(renderer), line_(line), table_(profile.punctuation), space_(renderer.getSpaceWidth(1)) {
    const size_t count = line.words.size();
    natural_.resize(count);
    for (size_t k = 0; k < count; ++k) natural_[k] = wordWidth(renderer_, line_, line_.words[k]);
    cls_.assign(count, CjkPunctuation::Class::None);
    for (size_t k = 0; k < count; ++k) cls_[k] = markClass(k);

    forced_ = natural_;
    minimum_ = natural_;
    atEdge_.assign(count, false);
    for (size_t k = 0; k < count; ++k) {
      const CjkPunctuation::Class mark = cls_[k];
      if (mark == CjkPunctuation::Class::None) continue;
      const bool opening = mark == CjkPunctuation::Class::Opening;
      atEdge_[k] = opening ? k == 0 : k + 1 == count;
      CjkPunctuation::Class neighbour = CjkPunctuation::Class::None;
      if (!atEdge_[k]) {
        neighbour = opening ? edgeClass(k - 1, lastCodepoint(line_.words[k - 1].text))
                            : edgeClass(k + 1, firstCodepoint(line_.words[k + 1].text));
      }
      const CjkPunctuation::Width w = CjkPunctuation::widthOf(table_, mark, neighbour, atEdge_[k]);
      forced_[k] = (natural_[k] * w.forced + 4) / 8;
      minimum_[k] = (natural_[k] * w.minimum + 4) / 8;
    }

    gap_.assign(count, Gap::Dumped);
    quarter_.assign(count, 0);
    for (size_t k = 1; k < count; ++k) {
      const uint32_t left = lastCodepoint(line_.words[k - 1].text);
      const uint32_t right = firstCodepoint(line_.words[k].text);
      if (utf8IsUnspacedCjk(left) && utf8IsUnspacedCjk(right)) {
        gap_[k] = Gap::None;
      } else if (profile.cjkLatinGap && CjkPunctuation::takesCjkLatinGap(left, right)) {
        // A distance of the space advance or more is the source's own space, which stays as it is.
        const int distance = line_.words[k].x - line_.words[k - 1].x - natural_[k - 1];
        const int em = this->em(CjkPunctuation::isHanOrKana(left) ? k - 1 : k);
        if (distance < space_ && em > 0) {
          gap_[k] = Gap::Quarter;
          quarter_[k] = (em * CjkPunctuation::kCjkLatinGap + 4) / 8;
        }
      }
    }

    for (size_t k = 0; k < count; ++k) {
      if (cls_[k] == CjkPunctuation::Class::None) continue;
      (atEdge_[k] ? edgeTotal_ : markTotal_) += std::max(0, forced_[k] - minimum_[k]);
    }
    for (size_t k = 1; k < count; ++k) {
      if (gap_[k] == Gap::Quarter) gapTotal_ += quarter_[k] - quarter_[k] / 2;
    }
  }

  // The most the tiers can take off the line.
  int shrinkable() const { return edgeTotal_ + markTotal_ + gapTotal_; }

  // The draw x of every token when the line has to give `need` px; empty when the line cannot sit like that.
  std::vector<int> xAt(const int need) const {
    const Placed placed = place(need);
    return placed.ok ? placed.x : std::vector<int>{};
  }
  // The left edge of box 0, the right edge of the last box, and what the tiers took.
  int startAt(const int need) const { return line_.words.empty() ? 0 : place(need).left.front(); }
  int rightAt(const int need) const {
    if (line_.words.empty()) return 0;
    const Placed placed = place(need);
    return placed.left.back() + placed.width.back();
  }
  int appliedAt(const int need) const {
    const Shares shares = sharesAt(need);
    return shares.edge ? edgeTotal_ + shares.marks + shares.gaps : shares.marks + shares.gaps;
  }

  // The right edge a justified line ends at: the last token's draw x, its ink shift and its box at the line-end width
  // (or the line-end minimum, when the line was pushed in).
  int endsAt(const bool lineEndPushedIn) const {
    const size_t last = line_.words.size() - 1;
    const int width = lineEndPushedIn ? minimum_[last] : forced_[last];
    return line_.words[last].x + shift(last, width) + width;
  }

  bool startsWithMark() const { return !cls_.empty() && cls_.front() != CjkPunctuation::Class::None; }

  // Token k's box with its mark at its minimum width, wherever the dump drew it: the least room it can take.
  int leftAtMinimum(const size_t k) const { return line_.words[k].x + shift(k, minimum_[k]); }
  int rightAtMinimum(const size_t k) const { return leftAtMinimum(k) + minimum_[k]; }

 private:
  using Class = CjkPunctuation::Class;
  // How token k sits against token k-1: no gap (unspaced CJK), a quarter em (Han or kana against a Latin letter or
  // digit, no source space), or wherever the dump drew it (a word space, glued Latin, a mark beside a Latin letter).
  enum class Gap : uint8_t { None, Quarter, Dumped };
  struct Shares {
    bool edge = false;  // the line-edge marks gave everything
    int marks = 0;      // the tier of the other marks
    int gaps = 0;       // the tier of the CJK-Latin gaps
  };
  struct Placed {
    std::vector<int> left;   // the box's left edge
    std::vector<int> width;  // the box's width
    std::vector<int> x;      // the draw x
    bool ok = true;
  };

  int em(const size_t k) const {
    DumpWord one = line_.words[k];
    one.text = "\xE4\xB8\x80";  // 一
    return wordWidth(renderer_, line_, one);
  }
  bool fullWidth(const size_t k) const { return natural_[k] * 5 >= em(k) * 4; }
  Class edgeClass(const size_t k, const uint32_t cp) const {
    if (CjkPunctuation::isCurlyQuote(cp) && !fullWidth(k)) return Class::None;
    return CjkPunctuation::classOf(cp);
  }
  // The class of token k when it is one full-width mark whose width the table sets, else None.
  Class markClass(const size_t k) const {
    const std::string& text = line_.words[k].text;
    const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || *p != 0) return Class::None;
    const Class c = edgeClass(k, cp);
    if (c == Class::IdeographicSpace || c == Class::AmbiguousQuote) return Class::None;
    return c != Class::None && fullWidth(k) ? c : Class::None;
  }

  // How far right of its draw x a token's box starts, its ink placement (spec 5.2): an opening mark by what it lost,
  // a closing mark, 。 and 、 by nothing, other marks by half.
  int shift(const size_t k, const int width) const {
    const int removed = natural_[k] - width;
    switch (cls_[k]) {
      case Class::Opening:
        return removed;
      case Class::Closing:
      case Class::FullStop:
      case Class::Comma:
      case Class::None:
        return 0;
      default:
        return removed / 2;
    }
  }

  // What each tier takes when the line gives `need`: the line-edge marks all of theirs as soon as it gives anything,
  // the other marks and then the gaps what is left, each at most its total.
  Shares sharesAt(const int need) const {
    Shares shares;
    if (need <= 0) return shares;
    shares.edge = true;
    const int left = std::max(0, need - edgeTotal_);
    shares.marks = std::min(left, markTotal_);
    shares.gaps = std::min(std::max(0, left - markTotal_), gapTotal_);
    return shares;
  }

  // Items share a tier in line order by cumulative rounding: with t the tier's share, T its total and C the sum of
  // the amounts before, an item of `amount` gives ((C + amount) * t) / T - (C * t) / T.
  static int give(const int before, const int amount, const int share, const int total) {
    return total > 0 ? ((before + amount) * share) / total - (before * share) / total : 0;
  }

  Placed place(const int need) const {
    const size_t count = line_.words.size();
    const Shares shares = sharesAt(need);
    Placed placed;
    placed.left.assign(count, 0);
    placed.width.assign(count, 0);
    placed.x.assign(count, 0);
    int markBefore = 0;
    int gapBefore = 0;
    std::vector<int> gapGive(count, 0);
    for (size_t k = 0; k < count; ++k) {
      int width = natural_[k];
      if (cls_[k] != Class::None) {
        const int amount = std::max(0, forced_[k] - minimum_[k]);
        int taken = 0;
        if (atEdge_[k]) {
          taken = shares.edge ? amount : 0;
        } else {
          taken = give(markBefore, amount, shares.marks, markTotal_);
          markBefore += amount;
        }
        width = forced_[k] - taken;
      }
      placed.width[k] = width;
      if (k > 0 && gap_[k] == Gap::Quarter) {
        const int amount = quarter_[k] - quarter_[k] / 2;
        gapGive[k] = give(gapBefore, amount, shares.gaps, gapTotal_);
        gapBefore += amount;
      }
    }
    for (size_t k = 0; k < count; ++k) {
      const int dumpedLeft = line_.words[k].x + shift(k, placed.width[k]);
      if (k == 0) {
        placed.left[k] = dumpedLeft;
      } else {
        const int previousRight = placed.left[k - 1] + placed.width[k - 1];
        switch (gap_[k]) {
          case Gap::None:
            placed.left[k] = previousRight;
            break;
          case Gap::Quarter:
            placed.left[k] = previousRight + quarter_[k] - gapGive[k];
            break;
          case Gap::Dumped:
            placed.left[k] = dumpedLeft;
            // No gap next to a mark (spec 5.4); a word space is the space advance or more.
            if (dumpedLeft != previousRight && dumpedLeft - previousRight < space_) placed.ok = false;
            break;
        }
      }
      placed.x[k] = placed.left[k] - shift(k, placed.width[k]);
    }
    return placed;
  }

  const GfxRenderer& renderer_;
  const DumpLine& line_;
  TypographyProfile::Punctuation table_;
  int space_;
  std::vector<int> natural_;
  std::vector<Class> cls_;
  std::vector<int> forced_;   // the width at the token's place on the line (a mark's table width)
  std::vector<int> minimum_;  // and the least push-in can leave it
  std::vector<bool> atEdge_;  // a mark at the place that sets its line-edge width
  std::vector<Gap> gap_;
  std::vector<int> quarter_;  // a Quarter gap's width
  int edgeTotal_ = 0;
  int markTotal_ = 0;
  int gapTotal_ = 0;
};

// Where a block's text runs. Every LINE record in the corpus has x=0 (the books set no side margins), so the bounds are
// the viewport; a book with a whole-em grid rounds the width down to whole ems of the line's own em and centres it.
struct TextBounds {
  int left = 0;
  int right = 0;
};

TextBounds textBoundsOf(const GfxRenderer& renderer, const DumpLine& line, const TypographyProfile& profile) {
  EXPECT_EQ(line.x, 0) << "the text bounds assume the corpus books set no side margins: " << lineText(line);
  const int viewport = pipeline_harness::Profile{}.viewportWidth;
  if (!profile.wholeEmGrid) return TextBounds{0, viewport};
  DumpWord one;
  one.text = "\xE4\xB8\x80";  // 一
  const int em = wordWidth(renderer, line, one);
  const int cut = viewport % em;
  return TextBounds{cut / 2, cut / 2 + viewport - cut};
}

// The push-in (px the line gave) that reproduces the dumped line: every dumped x where the layout puts it, and the
// need exactly what the line's natural width overran its text width by. -1 when no push-in does.
int pushInOf(const LineLayout& layout, const DumpLine& line, const TextBounds& bounds) {
  for (int need = 0; need <= layout.shrinkable(); ++need) {
    const std::vector<int> x = layout.xAt(need);
    if (x.size() != line.words.size()) continue;
    bool sits = true;
    for (size_t k = 0; k < x.size() && sits; ++k) sits = x[k] == line.words[k].x;
    if (!sits) continue;
    const int natural = layout.rightAt(need) + layout.appliedAt(need) - layout.startAt(need);
    const int available = bounds.right - (line.align == kAlignLeft ? layout.startAt(need) : bounds.left);
    if (need == std::max(0, natural - available)) return need;
  }
  return -1;
}

// The block each line belongs to: a body line's shape (x, alignment and size, so one text width and left edge), a
// table cell line's own cell.
std::vector<std::string> blockKeys(const std::vector<DumpLine>& lines) {
  std::vector<std::string> keys;
  size_t cell = 0;
  for (const auto& line : lines) {
    if (line.cell && !line.followsLine) ++cell;
    keys.push_back(line.cell ? "cell " + std::to_string(cell) : line.shape);
  }
  return keys;
}

// A line's left: its first word's left edge, or a ruby box that overhangs it further left.
int lineLeft(const GfxRenderer& renderer, const DumpLine& line, const TypographyProfile& profile) {
  int left = LineLayout(renderer, line, profile).startAt(0);
  for (const auto& ruby : line.rubies) left = std::min(left, ruby.x);
  return left;
}

// Whether word k is in a ruby group of the line.
bool inRubyGroup(const DumpLine& line, const size_t k) {
  for (const auto& ruby : line.rubies) {
    if (k >= ruby.first && k < ruby.first + ruby.count) return true;
  }
  return false;
}

// Where each block's leftmost line starts. A line that starts further right is a paragraph's indented first line
// (or a centred line); with the CJK grid every line of a block starts a few pixels in. A line that opens with a CJK
// mark does not count: a pushed-in opening mark there would pull the edge in. A block with no other line takes the
// edge of all of them.
std::map<std::string, int> blockLeftEdges(const GfxRenderer& renderer, const std::vector<DumpLine>& lines,
                                          const std::vector<std::string>& keys, const TypographyProfile& profile) {
  std::map<std::string, int> edges;
  std::map<std::string, int> withMarkFirst;
  const auto lower = [](std::map<std::string, int>& into, const std::string& key, const int left) {
    const auto [at, inserted] = into.emplace(key, left);
    if (!inserted) at->second = std::min(at->second, left);
  };
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].words.empty()) continue;
    const int left = lineLeft(renderer, lines[i], profile);
    lower(LineLayout(renderer, lines[i], profile).startsWithMark() ? withMarkFirst : edges, keys[i], left);
  }
  for (const auto& [key, left] : withMarkFirst) {
    if (edges.find(key) == edges.end()) edges.emplace(key, left);
  }
  return edges;
}

struct Case {
  std::string epub;
  std::string language;
  bool mixesLatin = false;    // puts Latin letters or digits against Han or kana with no space
  bool forcesPushIn = false;  // has lines the push-in must narrow marks on to fit
  bool hyphenation = false;
};

void PrintTo(const Case& c, std::ostream* os) {
  *os << fs::path(c.epub).filename().string() << (c.hyphenation ? " [hyphenation]" : "");
}

class CjkRules : public testing::TestWithParam<Case> {
 protected:
  void SetUp() override {
    const Case c = GetParam();
    // One directory per test, not per book: ctest -j runs a book's three tests side by side, and one
    // test's remove_all would pull the cache from under another's build.
    const std::string test = testing::UnitTest::GetInstance()->current_test_info()->name();
    const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" /
                          ("cjk_rules_" + test.substr(0, test.find('/')) + "_" + fs::path(c.epub).stem().string() +
                           (c.hyphenation ? "_hyph" : "") + (alignment() == 0 ? "_justified" : ""));
    fs::remove_all(cacheDir);
    fs::create_directories(cacheDir);
    pipeline_harness::Profile run;
    run.paragraphAlignment = alignment();
    run.hyphenationEnabled = c.hyphenation;
    profile = TypographyProfile::forLanguage(c.language);
    std::ostringstream dump;
    ASSERT_TRUE(pipeline_harness::runAndDump(c.epub, cacheDir.string(), run, dump)) << dump.str();
    lines = parseLines(dump.str());
    ASSERT_FALSE(lines.empty());
  }

  int wordWidth(const DumpLine& line, const DumpWord& w) const { return ::wordWidth(renderer, line, w); }
  // The reader's paragraph alignment: left (1), where a line's gaps are its natural gaps.
  virtual uint8_t alignment() const { return 1; }

  GfxRenderer renderer;
  TypographyProfile profile;
  std::vector<DumpLine> lines;
};

// The same books laid out justified.
class CjkJustifiedRules : public CjkRules {
 protected:
  uint8_t alignment() const override { return 0; }
};

// At a line break with unspaced CJK on either side, the line after it does not start with closing
// punctuation and the line before it does not end with an opening bracket (spec Appendix A.1): a
// line starting with " after a CJK line, or ending with " before one, is caught too.
TEST_P(CjkRules, KinsokuHoldsAtEveryCjkLineBreak) {
  const std::vector<std::string> keys = blockKeys(lines);
  const std::map<std::string, int> leftEdge = blockLeftEdges(renderer, lines, keys, profile);
  size_t breaks = 0;   // line breaks with unspaced CJK on either side
  size_t checked = 0;  // those that are layout breaks, not a paragraph's start
  for (size_t i = 1; i < lines.size(); ++i) {
    const DumpLine& prev = lines[i - 1];
    const DumpLine& line = lines[i];
    if (!line.followsLine || prev.words.empty() || line.words.empty()) continue;
    const uint32_t end = lastCodepoint(prev.words.back().text);
    const uint32_t start = firstCodepoint(line.words.front().text);
    if (!utf8IsUnspacedCjk(end) && !utf8IsUnspacedCjk(start)) continue;
    ++breaks;
    // A paragraph's indented first line is not a layout break.
    if (lineLeft(renderer, line, profile) > leftEdge.at(keys[i])) continue;
    ++checked;
    EXPECT_FALSE(CjkLineBreak::isNoLineEnd(end))
        << "line ends with an opening bracket: " << lineText(prev) << " before: " << lineText(line);
    EXPECT_FALSE(CjkLineBreak::isNoLineStart(start))
        << "line starts with closing punctuation: " << lineText(line) << " after: " << lineText(prev);
  }
  // Paragraph starts are the fewer of a book's line breaks: a check that skips most of them checks nothing.
  EXPECT_GE(checked * 2, breaks) << "only " << checked << " of " << breaks << " CJK line breaks checked";
}

// CJK scripts are not hyphenated: no line ends with a hyphen after a CJK character.
TEST_P(CjkRules, NoHyphenAfterCjkText) {
  for (const auto& line : lines) {
    if (line.words.empty()) continue;
    const std::string& last = line.words.back().text;
    if (last.size() < 2 || last.back() != '-') continue;
    EXPECT_FALSE(utf8IsCjkBreakable(lastCodepoint(last.substr(0, last.size() - 1))))
        << "hyphen after CJK text: " << lineText(line);
  }
}

// Every line sits where ParsedText's rules, restated, put it: each CJK mark at its table width, each CJK-Latin gap at
// a quarter em, and on a line that would not fit, push-in taking the line-edge marks, then the other marks, then the
// gaps, exactly as much as the line needs (spec 5.2-5.4). No line is wider than its text width, and a line that shrank
// could not have kept its natural width. A line with ruby, whose bases the ruby widens, and a table cell's line, whose
// width is not known, are checked pair by pair: no unspaced CJK pair overlaps with each mark at its minimum width.
TEST_P(CjkRules, LinesSitWhereTheirWidthsPutThem) {
  size_t checked = 0;
  size_t pushedIn = 0;
  size_t restated = 0;  // lines that could be laid out again: no ruby, not a table cell
  for (const auto& line : lines) {
    if (line.words.empty()) continue;
    if (line.rubies.empty() && !line.cell) ++restated;
    const LineLayout layout(renderer, line, profile);
    if ((line.align == kAlignLeft || line.align == kAlignRight) && line.rubies.empty()) {
      const TextBounds bounds = textBoundsOf(renderer, line, profile);
      const int need = pushInOf(layout, line, bounds);
      ++checked;
      EXPECT_GE(need, 0) << "not where its widths put it: " << lineText(line);
      if (need < 0) continue;
      EXPECT_LE(layout.rightAt(need), bounds.right) << "wider than the text width: " << lineText(line);
      if (need > 0) ++pushedIn;
      continue;
    }
    for (size_t i = 0; i + 1 < line.words.size(); ++i) {
      const DumpWord& left = line.words[i];
      const DumpWord& right = line.words[i + 1];
      if (!utf8IsUnspacedCjk(lastCodepoint(left.text)) || !utf8IsUnspacedCjk(firstCodepoint(right.text))) continue;
      if (inRubyGroup(line, i) || inRubyGroup(line, i + 1)) continue;  // the base is spread there
      EXPECT_GE(layout.leftAtMinimum(i + 1) - layout.rightAtMinimum(i), 0)
          << "\"" << right.text << "\" overlaps \"" << left.text << "\" in: " << lineText(line);
    }
  }
  // Ruby lines and table cells are checked pair by pair; of the others only a few centred or justified headings are not
  // laid out again.
  EXPECT_GE(checked * 4, restated * 3) << "only " << checked << " of " << restated << " lines laid out again";
  if (GetParam().forcesPushIn) EXPECT_GT(pushedIn, 0u) << "no line was pushed in";
}

// Between Han or kana and a Latin letter or European digit with no space in the source, a Chinese or Japanese book
// sets a quarter em of the CJK character's size, at least half of it on a line that was pushed in; other books set
// nothing (spec 5.4). A gap of the space advance or more is the source's own space. A line with a ruby box cannot be
// laid out again (LinesSitWhereTheirWidthsPutThem; the ruby widening of its groups is not restated, so the line's
// push-in cannot be told apart even for tokens outside the groups), so it may hold any gap a push-in could leave.
TEST_P(CjkRules, CjkLatinGapsSitWhereTheSpecPutsThem) {
  const int space = renderer.getSpaceWidth(1);
  size_t checked = 0;
  for (const auto& line : lines) {
    if (line.align != kAlignLeft && line.align != kAlignRight) continue;  // a justified line stretches its gaps
    bool pushedIn = !line.rubies.empty();
    if (line.rubies.empty()) {
      const int need = pushInOf(LineLayout(renderer, line, profile), line, textBoundsOf(renderer, line, profile));
      EXPECT_GE(need, 0) << "not where its widths put it: " << lineText(line);
      pushedIn = need != 0;
    }
    for (size_t k = 0; k + 1 < line.words.size(); ++k) {
      const uint32_t left = lastCodepoint(line.words[k].text);
      const uint32_t right = firstCodepoint(line.words[k + 1].text);
      if (!CjkPunctuation::takesCjkLatinGap(left, right)) continue;
      // The base of a ruby group is widened and drawn into its share: its x says nothing about the gap.
      if (inRubyGroup(line, k) || inRubyGroup(line, k + 1)) continue;
      const int gap = line.words[k + 1].x - line.words[k].x - wordWidth(line, line.words[k]);
      if (gap >= space) continue;  // the source's own space
      ++checked;
      DumpWord one = line.words[CjkPunctuation::isHanOrKana(left) ? k : k + 1];
      one.text = "\xE4\xB8\x80";  // 一, in the CJK token's style and size
      const int em = wordWidth(line, one);
      const int quarter = (em * CjkPunctuation::kCjkLatinGap + 4) / 8;
      if (profile.cjkLatinGap && !pushedIn) {
        EXPECT_EQ(gap, quarter) << "gap in: " << lineText(line);
      } else if (profile.cjkLatinGap) {
        EXPECT_GE(gap, quarter / 2) << "gap in: " << lineText(line);
        EXPECT_LE(gap, quarter) << "gap in: " << lineText(line);
      } else {
        EXPECT_EQ(gap, 0) << "gap in: " << lineText(line);
      }
    }
  }
  if (GetParam().mixesLatin) EXPECT_GT(checked, 0u) << "no CJK-Latin boundary checked";
}

// Every ruby box lies inside its block and the page, no two boxes on a line overlap, and a box covers no word
// outside its group but kana and CJK punctuation outside any group (spec 7.2): never Han, Latin or another
// group's base.
TEST_P(CjkRules, RubyStaysOverItsBaseAndOffTheHan) {
  const std::vector<std::string> keys = blockKeys(lines);
  const std::map<std::string, int> leftEdge = blockLeftEdges(renderer, lines, keys, profile);
  const int viewportWidth = pipeline_harness::Profile{}.viewportWidth;
  const auto overhangable = [](const uint32_t cp) {
    return utf8IsKana(cp) || CjkPunctuation::classOf(cp) != CjkPunctuation::Class::None;
  };
  size_t boxes = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const DumpLine& line = lines[i];
    if (line.rubies.empty()) continue;
    std::vector<DumpRuby> sorted = line.rubies;
    std::sort(sorted.begin(), sorted.end(), [](const DumpRuby& a, const DumpRuby& b) { return a.x < b.x; });
    for (size_t k = 0; k < sorted.size(); ++k) {
      const DumpRuby& box = sorted[k];
      ++boxes;
      EXPECT_GE(box.x, leftEdge.at(keys[i]))
          << "ruby " << box.text << " leaves its block on the left: " << lineText(line);
      if (!line.cell) {
        EXPECT_LE(box.x + box.w, viewportWidth - line.x)
            << "ruby " << box.text << " leaves the page on the right: " << lineText(line);
      }
      if (k + 1 < sorted.size()) {
        EXPECT_LE(box.x + box.w, sorted[k + 1].x)
            << "ruby " << box.text << " overlaps " << sorted[k + 1].text << ": " << lineText(line);
      }
      for (size_t word = 0; word < line.words.size(); ++word) {
        if (word >= box.first && word < box.first + box.count) continue;
        const int wordLeft = line.words[word].x;
        const int wordRight = wordLeft + wordWidth(line, line.words[word]);
        if (wordRight <= box.x || wordLeft >= box.x + box.w) continue;
        const std::string& text = line.words[word].text;
        EXPECT_TRUE(!inRubyGroup(line, word) && overhangable(firstCodepoint(text)) && overhangable(lastCodepoint(text)))
            << "ruby " << box.text << " covers \"" << text << "\" in: " << lineText(line);
      }
    }
  }
  const std::string book = fs::path(GetParam().epub).stem().string();
  if (book == "test_cjk_line_breaking" || book == "test_cjk_ja_soseki") {
    ASSERT_GE(boxes, 20u) << book << " has too few ruby boxes";
  } else if (boxes == 0) {
    GTEST_SKIP() << book << " has no ruby";
  }
}

// The line has a breakable gap between two characters of unspaced CJK text, the gaps a justified CJK line stretches.
bool hasCjkGap(const DumpLine& line) {
  for (size_t i = 0; i + 1 < line.words.size(); ++i) {
    const uint32_t left = lastCodepoint(line.words[i].text);
    const uint32_t right = firstCodepoint(line.words[i + 1].text);
    if (utf8IsUnspacedCjk(left) && utf8IsUnspacedCjk(right) && CjkLineBreak::isGapBetween(left, right)) return true;
  }
  return false;
}

// A justified line with CJK gaps that is not its paragraph's last ends flush (spec 5.1), all at one right edge,
// and in a Chinese or Japanese book the text width is a whole number of ems (spec 5.5). Lines with one LINE record
// shape (x, alignment, size) share a text width; the shape's left edge is where its leftmost line starts. A line
// is not its paragraph's last when the next line of its flow has the same shape, starts at that left edge (not
// indented) and follows without a margin. Lines justified at word spaces (Latin, Korean) keep their old rounding
// and are not checked: the Korean book is not instantiated here.
TEST_P(CjkJustifiedRules, JustifiedLinesEndFlushOnAWholeEmGrid) {
  ASSERT_EQ(profile.justify, TypographyProfile::Justify::BetweenCharacters);

  const std::vector<std::string> keys = blockKeys(lines);
  std::map<std::string, int> leftEdge = blockLeftEdges(renderer, lines, keys, profile);  // shape -> its left edge
  std::map<std::string, int> lineStep;  // shape -> its smallest step down the page
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].words.empty()) continue;
    if (i > 0 && lines[i].followsLine && lines[i - 1].shape == lines[i].shape && lines[i].y > lines[i - 1].y) {
      const auto [step, fresh] = lineStep.emplace(lines[i].shape, lines[i].y - lines[i - 1].y);
      if (!fresh) step->second = std::min(step->second, lines[i].y - lines[i - 1].y);
    }
  }

  std::map<std::string, std::vector<size_t>> candidates;  // shape -> its lines to check
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    const DumpLine& line = lines[i];
    const DumpLine& next = lines[i + 1];
    if (line.words.empty() || next.words.empty() || !line.justified || !hasCjkGap(line)) continue;
    if (!next.followsLine || next.shape != line.shape) continue;
    if (lineLeft(renderer, next, profile) != leftEdge[line.shape]) continue;  // the next paragraph's start
    if (next.y > line.y && next.y - line.y > lineStep[line.shape]) continue;  // a margin: a new paragraph
    candidates[line.shape].push_back(i);
  }
  ASSERT_FALSE(candidates.empty()) << "no justified CJK line to check";

  for (const auto& [shape, indices] : candidates) {
    const TextBounds bounds = textBoundsOf(renderer, lines[indices.front()], profile);
    for (const size_t i : indices) {
      const DumpLine& line = lines[i];
      const LineLayout layout(renderer, line, profile);
      // The line ends at the text width on its last token, or a ruby box that holds the group's overhang inside.
      bool flush = layout.endsAt(false) == bounds.right || layout.endsAt(true) == bounds.right;
      for (const auto& ruby : line.rubies) {
        if (ruby.first + ruby.count == line.words.size() && ruby.x + ruby.w == bounds.right) flush = true;
      }
      EXPECT_TRUE(flush) << "not flush with " << bounds.right << ": " << lineText(line);
    }
    if (profile.wholeEmGrid) {
      DumpWord one;
      one.text = "\xE4\xB8\x80";  // 一
      const int em = ::wordWidth(renderer, lines[indices.front()], one);
      // The dumped left edge against the right edge the lines were just checked to end at.
      EXPECT_EQ((bounds.right - leftEdge[shape]) % em, 0)
          << "text width " << bounds.right - leftEdge[shape] << " is not whole ems of " << em << " (" << shape << ")";
    }
  }
}

std::vector<Case> cjkBooks() {
  struct Book {
    const char* name;
    const char* language;
    bool mixesLatin;
    bool forcesPushIn;
  };
  std::vector<Case> cases;
  for (const Book& book : std::vector<Book>{{"test_cjk_line_breaking", "ja", true, false},
                                            {"test_cjk_ja_soseki", "ja", false, false},
                                            {"test_cjk_ko_hyeon", "ko", false, false},
                                            {"test_cjk_zh_honglou", "zh-Hant", false, false},
                                            {"test_cjk_zh_punctuation", "zh-Hans", true, false}}) {
    const std::string path = (fs::path(CORPUS_DIR) / (std::string(book.name) + ".epub")).string();
    cases.push_back(Case{path, book.language, book.mixesLatin, book.forcesPushIn, false});
    cases.push_back(Case{path, book.language, book.mixesLatin, book.forcesPushIn, true});
  }
  return cases;
}

// The books a justified line stretches between characters in: Chinese and Japanese. Korean justifies at word spaces.
std::vector<Case> booksJustifiedBetweenCharacters() {
  std::vector<Case> cases = cjkBooks();
  cases.erase(std::remove_if(cases.begin(), cases.end(), [](const Case& c) { return c.language == "ko"; }),
              cases.end());
  return cases;
}

std::string caseName(const testing::TestParamInfo<Case>& info) {
  return fs::path(info.param.epub).stem().string() + (info.param.hyphenation ? "_hyph" : "");
}

INSTANTIATE_TEST_SUITE_P(CjkCorpus, CjkRules, testing::ValuesIn(cjkBooks()), caseName);
INSTANTIATE_TEST_SUITE_P(CjkCorpus, CjkJustifiedRules, testing::ValuesIn(booksJustifiedBetweenCharacters()), caseName);

}  // namespace

// A source line break next to an element that is not inline formatting is a word gap, not a CJK
// segment break to drop (test_cjk_block_seams.epub; not in cjkBooks(), since these spaces are
// correct). A list item whose text sits on its own indented line keeps the space after its marker
// that its one-line control has, and the text of <dl>, <section> and <figure>, which the parser lays
// out in one block, keeps a space at each seam.
TEST(CjkBlockSeams, ALineBreakAtABlockOrListItemStaysASpace) {
  const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" / "cjk_block_seams";
  fs::remove_all(cacheDir);
  fs::create_directories(cacheDir);
  pipeline_harness::Profile profile;
  profile.paragraphAlignment = 1;  // left: a line's gaps are its natural gaps
  std::ostringstream dump;
  ASSERT_TRUE(pipeline_harness::runAndDump((fs::path(CORPUS_DIR) / "test_cjk_block_seams.epub").string(),
                                           cacheDir.string(), profile, dump));
  const std::vector<DumpLine> lines = parseLines(dump.str());
  const GfxRenderer renderer;

  // The gap between words[i] and words[i + 1] of the line where `left` is followed by `right`, or
  // -1 when no line has them side by side.
  const auto gapBetween = [&](const std::string& left, const std::string& right) {
    for (const auto& line : lines) {
      for (size_t i = 0; i + 1 < line.words.size(); ++i) {
        if (line.words[i].text == left && line.words[i + 1].text == right) {
          return line.words[i + 1].x - line.words[i].x - wordWidth(renderer, line, line.words[i]);
        }
      }
    }
    return -1;
  };
  // The gap between an item's marker and the first character of its text, 第 followed by `second`.
  const auto markerGap = [&](const std::string& second) {
    for (const auto& line : lines) {
      for (size_t i = 1; i + 1 < line.words.size(); ++i) {
        if (line.words[i].text == "第" && line.words[i + 1].text == second) {
          return line.words[i].x - line.words[i - 1].x - wordWidth(renderer, line, line.words[i - 1]);
        }
      }
    }
    return -1;
  };

  // Pretty-printed item and one-line control: the <ul> pair, then the <ol> pair. The <ol> markers
  // (1. and 2.) differ in width, so each pair compares the gap after the marker.
  ASSERT_GT(markerGap("二"), 0) << "the <ul> control item is missing";
  EXPECT_EQ(markerGap("一"), markerGap("二")) << "the pretty-printed <ul> item is glued to its marker";
  ASSERT_GT(markerGap("四"), 0) << "the <ol> control item is missing";
  EXPECT_EQ(markerGap("三"), markerGap("四")) << "the pretty-printed <ol> item is glued to its marker";

  // When the two sides share a line, a space separates them.
  if (const int gap = gapBetween("語", "説"); gap != -1) {
    EXPECT_GT(gap, 0) << "用語 and 説明文 glued";
  }
  if (const int gap = gapBetween("章", "図"); gap != -1) {
    EXPECT_GT(gap, 0) << "節の文章 and 図の説明 glued";
  }
}

// In CJK text the quotation marks are CJK punctuation: a source line break between one and unspaced
// CJK text vanishes, as one between two CJK characters does, so 樂業。\n”石頭 reads 樂業。”石頭 and ”
// cannot start a line (the fixture's case 10, after Hong lou meng's hard-wrapped source). With
// quotes on both sides, the line break stays a space.
TEST(CjkQuoteSeams, ALineBreakNextToAQuoteInCjkTextVanishes) {
  const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" / "cjk_quote_seams";
  fs::remove_all(cacheDir);
  fs::create_directories(cacheDir);
  pipeline_harness::Profile profile;
  profile.paragraphAlignment = 1;  // left: a line's gaps are its natural gaps
  std::ostringstream dump;
  ASSERT_TRUE(pipeline_harness::runAndDump((fs::path(CORPUS_DIR) / "test_cjk_line_breaking.epub").string(),
                                           cacheDir.string(), profile, dump));
  const std::vector<DumpLine> lines = parseLines(dump.str());
  const GfxRenderer renderer;

  // The gap before every word starting with `right` whose left neighbour on its line ends with `left`.
  const auto gapsAt = [&](const uint32_t left, const uint32_t right) {
    std::vector<int> gaps;
    for (const auto& line : lines) {
      for (size_t i = 0; i + 1 < line.words.size(); ++i) {
        const DumpWord& l = line.words[i];
        const DumpWord& r = line.words[i + 1];
        if (lastCodepoint(l.text) == left && firstCodepoint(r.text) == right) {
          gaps.push_back(r.x - l.x - wordWidth(renderer, line, l));
        }
      }
    }
    return gaps;
  };

  const struct {
    uint32_t left;
    uint32_t right;
    const char* seam;
  } glued[] = {
      {0x3002, 0x201D, "。 + ”"}, {0xFF1A, '"', "： + \""}, {0x201D, 0x5144, "” + 兄"}, {0x5B57, 0x201C, "字 + “"}};
  for (const auto& s : glued) {
    const std::vector<int> gaps = gapsAt(s.left, s.right);
    ASSERT_FALSE(gaps.empty()) << "no line has the seam " << s.seam;
    for (const int gap : gaps) EXPECT_EQ(gap, 0) << "the line break at " << s.seam << " stayed a space";
  }
  const std::vector<int> control = gapsAt(0x201D, 0x201C);  // ” + “
  ASSERT_FALSE(control.empty()) << "no line has the seam ” + “";
  for (const int gap : control) EXPECT_GT(gap, 0) << "the line break between two quotes lost its space";
}
