// A series index read as a number in millionths (lib/LibraryIndex/SeriesIndex.h), against strtof, which read it
// before: the Library's series order, the end-of-book screen's sequel and the KOReader upload's series_index.
#include <SeriesIndex.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

namespace {

// What strtof made of `text`, in millionths, and where it stopped.
int64_t viaStrtof(const char* text, const char** end) {
  char* stop = nullptr;
  const float value = std::strtof(text, &stop);
  *end = stop;
  return std::llround(static_cast<double>(value) * 1e6);
}

}  // namespace

// Every decimal a book writes, and the prefixes strtof stops at: the same number and the same end.
TEST(SeriesIndex, ReadsADecimalAsStrtofDid) {
  for (const char* text : {"0",  "1",   "3",    "2.5",  "3.0",    "02",     "10",       "12.5",    "1.25",
                           ".5", "5.",  "-1",   "+4",   " 7",     "\t8",    "1e2",      "2.5e-1",  "25E-1",
                           "1e", "1e+", "3abc", "2.5 ", "100.75", "1234.5", "999999.5", "0.000005"}) {
    const char* ours = nullptr;
    const auto parsed = SeriesIndex::parse(text, &ours);
    const char* theirs = nullptr;
    const int64_t expected = viaStrtof(text, &theirs);
    ASSERT_TRUE(parsed.has_value()) << text;
    EXPECT_EQ(parsed->millionths, expected) << text;
    EXPECT_EQ(ours, theirs) << text;
  }
}

TEST(SeriesIndex, NoDigitsIsNoNumber) {
  for (const char* text : {"", " ", "x", "-", ".", "e5", "nan", "inf"}) {
    const char* end = nullptr;
    EXPECT_FALSE(SeriesIndex::parse(text, &end).has_value()) << text;
    EXPECT_EQ(end, text) << text;  // as strtof leaves it when it converts nothing
  }
}

TEST(SeriesIndex, OrdersAsNumbers) {
  EXPECT_LT(*SeriesIndex::parse("2.5"), *SeriesIndex::parse("9"));
  EXPECT_LT(*SeriesIndex::parse("9"), *SeriesIndex::parse("10"));
  EXPECT_LT(*SeriesIndex::parse("1.9"), *SeriesIndex::parse("1.10e1"));
  EXPECT_EQ(*SeriesIndex::parse("3"), *SeriesIndex::parse("3.000"));
  EXPECT_LT(*SeriesIndex::parse("-1"), *SeriesIndex::parse("0"));
}

// Digits past the sixth decimal are dropped; an index of ten million or more is no index.
TEST(SeriesIndex, LimitsOfTheMillionths) {
  EXPECT_EQ(SeriesIndex::parse("1.0000009")->millionths, 1000000);
  EXPECT_EQ(SeriesIndex::parse("9999999.999999")->millionths, 9999999999999);
  EXPECT_FALSE(SeriesIndex::parse("10000000").has_value());
  EXPECT_FALSE(SeriesIndex::parse("1e7").has_value());
  EXPECT_FALSE(SeriesIndex::parse("123456789012345678901234567890").has_value());
  EXPECT_EQ(SeriesIndex::parse("1e-7")->millionths, 0);
}
