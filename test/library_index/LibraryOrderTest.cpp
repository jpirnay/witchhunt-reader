// An author's books in the order the Library lists them, sorted in the lent framebuffer.
#include <BuildArena.h>
#include <LibraryOrder.h>
#include <gtest/gtest.h>

#include <map>
#include <vector>

namespace {

using Catalog = std::map<uint16_t, LibraryOrder::BookKey>;

bool keyOf(void* user, const uint16_t record, LibraryOrder::BookKey& key) {
  const auto& catalog = *static_cast<const Catalog*>(user);
  const auto it = catalog.find(record);
  if (it == catalog.end()) return false;
  key = it->second;
  return true;
}

std::vector<uint16_t> sorted(std::vector<uint16_t> records, Catalog catalog, const size_t arenaBytes = 48000,
                             bool* ok = nullptr) {
  BuildArena arena(arenaBytes);
  const bool done = LibraryOrder::sortBySeries(records.data(), records.size(), arena, &keyOf, &catalog);
  if (ok != nullptr) *ok = done;
  return records;
}

}  // namespace

TEST(LibraryOrder, SeriesThenIndexThenTitleWithBooksInNoSeriesLast) {
  const Catalog catalog{{0, {"", "", "Small Gods"}},
                        {1, {"Discworld", "3", "Equal Rites"}},
                        {2, {"Discworld", "1", "The Colour of Magic"}},
                        {3, {"", "", "Good Omens"}},
                        {4, {"Discworld", "2", "The Light Fantastic"}}};
  EXPECT_EQ(sorted({0, 1, 2, 3, 4}, catalog), (std::vector<uint16_t>{2, 4, 1, 3, 0}));
}

TEST(LibraryOrder, SeriesIndexesCompareAsNumbers) {
  const Catalog catalog{{0, {"S", "10", "a"}}, {1, {"S", "9", "b"}}, {2, {"S", "2.5", "c"}}};
  EXPECT_EQ(sorted({0, 1, 2}, catalog), (std::vector<uint16_t>{2, 1, 0}));
}

TEST(LibraryOrder, TitlesCompareNaturallyAndIgnoreCase) {
  const Catalog catalog{{0, {"", "", "book 10"}}, {1, {"", "", "Book 9"}}, {2, {"", "", "apple"}}};
  EXPECT_EQ(sorted({0, 1, 2}, catalog), (std::vector<uint16_t>{2, 1, 0}));
}

// strtof gives NaN for "nan", and a NaN in the comparison breaks the sort's ordering contract.
TEST(LibraryOrder, AnIndexThatIsNotANumberSortsAsZero) {
  const Catalog catalog{{0, {"S", "1", "a"}}, {1, {"S", "nan", "b"}}, {2, {"S", "x", "c"}}};
  EXPECT_EQ(sorted({0, 1, 2}, catalog), (std::vector<uint16_t>{1, 2, 0}));
}

TEST(LibraryOrder, ABookWithoutDetailsStaysInTheList) {
  const Catalog catalog{{0, {"", "", "b"}}, {2, {"", "", "a"}}};
  const auto order = sorted({0, 1, 2}, catalog);
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order, (std::vector<uint16_t>{1, 2, 0})) << "no title sorts before any title";
}

TEST(LibraryOrder, AnArenaTooSmallLeavesTheOrderAlone) {
  const Catalog catalog{{0, {"", "", "b"}}, {1, {"", "", "a"}}};
  bool ok = true;
  EXPECT_EQ(sorted({0, 1}, catalog, 16, &ok), (std::vector<uint16_t>{0, 1}));
  EXPECT_FALSE(ok);
}
