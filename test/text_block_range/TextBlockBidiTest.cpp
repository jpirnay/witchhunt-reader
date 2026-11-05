// A right-to-left line keeps two things a left-to-right one never needs, in an optional region of its arena (the
// TextBlock bidi region, section v108): a word's display form, the characters it is drawn with in visual order, and the
// line's logical order, each word's place in reading order. A line that needs neither writes the bytes it always did.
// Against the fixed-width shim of this directory: every codepoint 10 px.
#include <GfxRenderer.h>
#include <Memory.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Epub/TokenBoundary.h"
#include "Epub/blocks/TextBlock.h"

namespace {

constexpr int kFontId = 1;
// What TextBlock::serialize writes after the arena: the alignment byte, the size multiplier, the heading font.
constexpr size_t kStyleTrailerBytes = sizeof(CssTextAlign) + sizeof(float) + sizeof(int32_t);

// A line of `words` (views, as the bidi line pass hands them over) 50 px apart, every word REGULAR, with the display
// forms, the logical order and the annotations given; an empty vector stands for none.
std::unique_ptr<TextBlock> line(const std::vector<std::string_view>& words,
                                const std::vector<std::string_view>& forms = {}, const std::vector<uint8_t>& order = {},
                                const std::vector<TextBlock::Annotation>& annotations = {}) {
  const std::vector<EpdFontFamily::Style> styles(words.size(), EpdFontFamily::REGULAR);
  std::vector<int16_t> xpos;
  for (size_t i = 0; i < words.size(); i++) xpos.push_back(static_cast<int16_t>(i * 50));
  TextBlock::WordRange range;
  range.wordViews = words.data();
  range.styles = &styles;
  range.displayForms = forms.empty() ? nullptr : forms.data();
  range.logicalOrder = order.empty() ? nullptr : order.data();
  range.annotations = annotations.empty() ? nullptr : &annotations;
  range.first = 0;
  range.count = words.size();
  return makeUniqueNoThrow<TextBlock>(range, xpos, BlockStyle());
}

std::string bytesOf(const TextBlock& block) {
  FsFile file = HalFile::forReadWrite();
  if (!block.serialize(file) || !file.seek(0)) return {};
  std::string bytes(file.size(), '\0');
  file.read(bytes.data(), bytes.size());
  return bytes;
}

std::unique_ptr<TextBlock> readBack(const std::string& bytes) {
  FsFile file = HalFile::forReadWrite();
  file.write(bytes.data(), bytes.size());
  if (!file.seek(0)) return nullptr;
  return TextBlock::deserialize(file);
}

// Where the region sits in a serialised line without annotations: the header is u16 words, u8 flags, u16 text size,
// u16 region size; the region ends the arena, which the style trailer follows.
struct RegionAt {
  size_t begin = 0;
  size_t end = 0;
};
RegionAt regionOf(const std::string& bytes) {
  uint16_t regionBytes = 0;
  memcpy(&regionBytes, bytes.data() + 5, sizeof(regionBytes));
  RegionAt at;
  at.end = bytes.size() - kStyleTrailerBytes;
  at.begin = at.end - regionBytes;
  return at;
}

// A serialised line without annotations, given `region` as its bidi region by hand: the word count's bit 15 set and
// the header's region size written, inserted after the text size when the line had no region.
std::string withRegion(std::string bytes, const std::string& region) {
  const bool had = (static_cast<uint8_t>(bytes[1]) & 0x80) != 0;
  const RegionAt old =
      had ? regionOf(bytes) : RegionAt{bytes.size() - kStyleTrailerBytes, bytes.size() - kStyleTrailerBytes};
  bytes.replace(old.begin, old.end - old.begin, region);
  const auto size = static_cast<uint16_t>(region.size());
  if (had) {
    memcpy(bytes.data() + 5, &size, sizeof(size));
  } else {
    bytes.insert(5, reinterpret_cast<const char*>(&size), sizeof(size));
  }
  bytes[1] = static_cast<char>(static_cast<uint8_t>(bytes[1]) | 0x80);
  return bytes;
}

// An order-only region: the parts byte, then `order`.
std::string orderRegion(const std::vector<uint8_t>& order) {
  std::string region(1, '\x02');
  region.append(order.begin(), order.end());
  return region;
}

}  // namespace

TEST(TextBlockBidi, AFormIsKeptOnlyWhereItDiffers) {
  const auto block = line({"abc", "שלום"}, {"abc", "םולש"});
  ASSERT_TRUE(block->valid());
  EXPECT_FALSE(block->hasDisplayForm(0));
  EXPECT_TRUE(block->hasDisplayForm(1));
  EXPECT_STREQ("abc", block->wordDrawText(0));
  EXPECT_STREQ("םולש", block->wordDrawText(1));
  EXPECT_STREQ("שלום", block->wordText(1));
}

// The views are the bidi line's way in; the vectors layout's. One path reads both, so the same words give the same
// line: soft hyphens stripped, boundaries kept.
TEST(TextBlockBidi, WordViewsMatchTheVectorRange) {
  const std::vector<std::string> words = {"abc", "שלום", "hy\302\255phen"};  // U+00AD in octal: it ends at 3 digits
  const std::vector<std::string_view> views(words.begin(), words.end());
  const std::vector<EpdFontFamily::Style> styles = {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC};
  const std::vector<TokenBoundary::Boundary> boundaries = {{false, false}, {false, false}, {true, false}};
  const std::vector<int16_t> xpos = {0, 40, 90};

  TextBlock::WordRange fromVectors;
  fromVectors.words = &words;
  fromVectors.styles = &styles;
  fromVectors.boundaries = &boundaries;
  fromVectors.count = words.size();
  TextBlock::WordRange fromViews = fromVectors;
  fromViews.words = nullptr;
  fromViews.wordViews = views.data();

  const TextBlock a(fromVectors, xpos, BlockStyle());
  const TextBlock b(fromViews, xpos, BlockStyle());
  ASSERT_TRUE(a.valid());
  ASSERT_TRUE(b.valid());
  EXPECT_STREQ("hyphen", b.wordText(2));
  const std::string bytes = bytesOf(a);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(bytes, bytesOf(b));
}

// Layout measures a word without its soft hyphens, and the arena keeps it so: a form that is that text is no form.
TEST(TextBlockBidi, ASoftHyphenatedWordDrawnAsItsTextHasNoForm) {
  const auto block = line({"ab\302\255c"}, {"abc"});
  ASSERT_TRUE(block->valid());
  EXPECT_FALSE(block->hasDisplayForm(0));
  EXPECT_STREQ("abc", block->wordDrawText(0));
}

TEST(TextBlockBidi, TheLogicalOrderIsKeptOnlyWhenItReorders) {
  const auto reordered = line({"abc", "def"}, {}, {1, 0});
  ASSERT_TRUE(reordered->valid());
  EXPECT_TRUE(reordered->hasLogicalOrder());
  EXPECT_EQ(1, reordered->logicalPosition(0));
  EXPECT_EQ(0, reordered->logicalPosition(1));

  const auto identity = line({"abc", "def"}, {}, {0, 1});
  ASSERT_TRUE(identity->valid());
  EXPECT_FALSE(identity->hasLogicalOrder());
  EXPECT_EQ(0, identity->logicalPosition(0));
  EXPECT_EQ(1, identity->logicalPosition(1));
}

// Positions repeated, out of range, or on a line past what a byte can number: the line is not built.
TEST(TextBlockBidi, AnOrderThatIsNotAPermutationMakesTheBlockInvalid) {
  EXPECT_FALSE(line({"abc", "def"}, {}, {0, 0})->valid());
  EXPECT_FALSE(line({"abc", "def"}, {}, {2, 0})->valid());

  const std::vector<std::string_view> many(256, "a");
  std::vector<uint8_t> order(256);
  for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<uint8_t>(255 - i);
  EXPECT_FALSE(line(many, {}, order)->valid());
  EXPECT_TRUE(line(many)->valid());  // no order: as many words as any line
}

// Every Latin and CJK line of every section file stays as it was: the region, and the word count's top bit, only
// where a word has a form or the line an order.
TEST(TextBlockBidi, ALineWithoutTheRegionKeepsItsBytes) {
  const std::string plain = bytesOf(*line({"abc", "def"}));
  const std::string same = bytesOf(*line({"abc", "def"}, {"abc", "def"}, {0, 1}));
  ASSERT_FALSE(plain.empty());
  EXPECT_EQ(plain, same);
  EXPECT_EQ(0, static_cast<uint8_t>(same[1]) & 0x80);
}

TEST(TextBlockBidi, TheRegionSurvivesTheCache) {
  const std::string bytes = bytesOf(*line({"abc", "שלום"}, {"abc", "םולש"}, {1, 0}));
  ASSERT_FALSE(bytes.empty());
  EXPECT_NE(0, static_cast<uint8_t>(bytes[1]) & 0x80);

  const auto restored = readBack(bytes);
  ASSERT_NE(nullptr, restored);
  EXPECT_EQ(2, restored->wordCount());
  EXPECT_FALSE(restored->hasDisplayForm(0));
  EXPECT_TRUE(restored->hasDisplayForm(1));
  EXPECT_STREQ("םולש", restored->wordDrawText(1));
  EXPECT_STREQ("שלום", restored->wordText(1));
  EXPECT_TRUE(restored->hasLogicalOrder());
  EXPECT_EQ(1, restored->logicalPosition(0));
  EXPECT_EQ(0, restored->logicalPosition(1));
  EXPECT_EQ(bytes, bytesOf(*restored));
}

// The region follows the annotation region, whose size is odd here (one ruby record and a 3-byte ruby): both read
// back where they were written.
TEST(TextBlockBidi, TheRegionFollowsTheAnnotations) {
  TextBlock::Annotation ruby;
  ruby.firstWord = 0;
  ruby.wordCount = 1;
  ruby.width = 30;
  ruby.text = "xyz";
  const auto block = line({"abc", "שלום"}, {"abc", "םולש"}, {1, 0}, {ruby});
  ASSERT_TRUE(block->valid());
  const auto restored = readBack(bytesOf(*block));
  ASSERT_NE(nullptr, restored);
  ASSERT_EQ(1, restored->annotationCount());
  EXPECT_EQ("xyz", restored->annotation(0).text);
  EXPECT_STREQ("םולש", restored->wordDrawText(1));
  EXPECT_EQ(1, restored->logicalPosition(0));
  EXPECT_EQ(0, restored->logicalPosition(1));
}

TEST(TextBlockBidi, ACorruptRegionFailsTheRead) {
  const std::string bytes = bytesOf(*line({"abc", "שלום"}, {"abc", "םולש"}, {1, 0}));
  ASSERT_FALSE(bytes.empty());
  ASSERT_NE(nullptr, readBack(bytes));
  const RegionAt region = regionOf(bytes);

  std::string repeated = bytes;  // the order, the region's last two bytes, rewritten to {0, 0}
  repeated[region.end - 2] = 0;
  repeated[region.end - 1] = 0;
  EXPECT_EQ(nullptr, readBack(repeated));

  std::string outside = bytes;  // word 1's offset, after the parts byte and word 0's, rewritten to 0x7000
  const uint16_t offset = 0x7000;
  memcpy(outside.data() + region.begin + 1 + 2, &offset, sizeof(offset));
  EXPECT_EQ(nullptr, readBack(outside));

  for (const uint8_t parts : {0x00, 0x04, 0x07}) {  // none, or a reserved bit
    std::string reserved = bytes;
    reserved[region.begin] = static_cast<char>(parts);
    EXPECT_EQ(nullptr, readBack(reserved)) << "parts " << static_cast<int>(parts);
  }

  std::string noForm = bytes;  // the display part with no form left
  const uint16_t none = 0xFFFF;
  memcpy(noForm.data() + region.begin + 1 + 2, &none, sizeof(none));
  EXPECT_EQ(nullptr, readBack(noForm));

  std::string unterminated = bytes;  // the forms' final NUL, before the two order bytes, rewritten to 'x'
  ASSERT_EQ('\0', unterminated[region.end - 3]);
  unterminated[region.end - 3] = 'x';
  EXPECT_EQ(nullptr, readBack(unterminated));
}

// Regions built by hand on a two-word line. Its parts say how long it is: one byte for the parts, 2 per word and the
// texts for the forms, one per word for the order.
TEST(TextBlockBidi, ARegionWhoseSizesDoNotAddUpFailsTheRead) {
  const std::string plain = bytesOf(*line({"abc", "def"}));
  ASSERT_FALSE(plain.empty());
  const auto exact = readBack(withRegion(plain, orderRegion({1, 0})));  // 1 + 2 bytes: what the helper builds reads
  ASSERT_NE(nullptr, exact);
  EXPECT_EQ(1, exact->logicalPosition(0));

  // One byte longer: an order-only region has no texts, so the spare byte is a size that does not add up, though the
  // last two bytes are still an order.
  EXPECT_EQ(nullptr, readBack(withRegion(plain, std::string("\x02\x00\x01\x00", 4))));

  // One byte shorter than forms and order need at the least (1 + 4 + 2 with no texts): parts, word 0 none, word 1 at
  // offset 0x0100, and the order {1, 0} sharing its last byte with that offset.
  EXPECT_EQ(nullptr, readBack(withRegion(plain, std::string("\x03\xFF\xFF\x00\x01\x00", 6))));
}

// A byte numbers at most 255 words: a 256-word line's order fails the read, though it is a permutation.
TEST(TextBlockBidi, AnOrderOnALineOf256WordsFailsTheRead) {
  for (const size_t words : {255u, 256u}) {
    const std::string plain = bytesOf(*line(std::vector<std::string_view>(words, "a")));
    ASSERT_FALSE(plain.empty());
    std::vector<uint8_t> reversed(words);
    for (size_t i = 0; i < words; i++) reversed[i] = static_cast<uint8_t>(words - 1 - i);
    const auto restored = readBack(withRegion(plain, orderRegion(reversed)));
    if (words == 255) {
      ASSERT_NE(nullptr, restored);
      EXPECT_EQ(254, restored->logicalPosition(0));
    } else {
      EXPECT_EQ(nullptr, restored);
    }
  }
}

// The word count's bit 15 promises a region: none (a size of 0), or one on a line of no words, fails the read.
TEST(TextBlockBidi, TheRegionBitWithoutARegionFailsTheRead) {
  EXPECT_EQ(nullptr, readBack(withRegion(bytesOf(*line({"abc", "def"})), "")));

  const TextBlock empty(std::vector<std::string>{}, std::vector<int16_t>{}, std::vector<EpdFontFamily::Style>{});
  ASSERT_TRUE(empty.valid());
  const std::string emptyBytes = bytesOf(empty);
  ASSERT_FALSE(emptyBytes.empty());
  ASSERT_NE(nullptr, readBack(emptyBytes));
  EXPECT_EQ(nullptr, readBack(withRegion(emptyBytes, orderRegion({0}))));
}

TEST(TextBlockBidi, RenderDrawsTheForm) {
  GfxRenderer r;
  const auto block = line({"abc", "שלום"}, {"abc", "םולש"});
  block->render(r, kFontId, 0, 0);
  ASSERT_EQ(2u, r.drawCalls.size());
  EXPECT_EQ("abc", r.drawCalls[0].text);
  EXPECT_EQ("םולש", r.drawCalls[1].text);
}

// The form drops what is not drawn (here a U+200E LEFT-TO-RIGHT MARK): the box is the width of what is.
TEST(TextBlockBidi, AWordBoxMeasuresTheForm) {
  GfxRenderer r;
  const auto block = line({"abc\xE2\x80\x8E"}, {"abc"});
  ASSERT_TRUE(block->hasDisplayForm(0));
  EXPECT_EQ(3 * GfxRenderer::GLYPH_W, block->wordBox(r, 0, kFontId, 0, 0).width);
}
