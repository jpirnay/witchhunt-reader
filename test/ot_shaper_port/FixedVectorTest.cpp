#include <OtFixedVector.h>
#include <gtest/gtest.h>

#include <utility>
#include <vector>

// ot::FixedVector, the view deviation 1 puts in place of the engine's
// std::vectors: it works over memory its owner binds and never allocates.

namespace {

std::vector<int> contents(const ot::FixedVector<int>& v) { return std::vector<int>(v.begin(), v.end()); }

// The element just past a 4-element binding: a refused growth must leave it.
constexpr int SENTINEL = -12345;

}  // namespace

TEST(FixedVector, BindsEmpty) {
  int storage[4];
  bool ok = true;
  ot::FixedVector<int> v;
  v.bind(storage, 4, &ok);
  EXPECT_EQ(v.size(), 0u);
  EXPECT_EQ(v.capacity(), 4u);
  EXPECT_TRUE(v.empty());
  EXPECT_EQ(v.data(), storage);
  EXPECT_TRUE(ok);
}

TEST(FixedVector, PushBackFillsThenRefuses) {
  int storage[5] = {0, 0, 0, 0, SENTINEL};
  bool ok = true;
  ot::FixedVector<int> v;
  v.bind(storage, 4, &ok);
  for (int i = 1; i <= 4; i++) EXPECT_TRUE(v.push_back(i));
  EXPECT_TRUE(ok);
  EXPECT_FALSE(v.push_back(5));
  EXPECT_EQ(v.size(), 4u);
  EXPECT_EQ(contents(v), (std::vector<int>{1, 2, 3, 4}));
  EXPECT_EQ(storage[4], SENTINEL);
  EXPECT_FALSE(ok);
}

TEST(FixedVector, InsertShiftsTheTail) {
  int storage[5] = {0, 0, 0, 0, SENTINEL};
  bool ok = true;
  ot::FixedVector<int> v;
  v.bind(storage, 4, &ok);
  for (int i = 1; i <= 3; i++) v.push_back(i);
  EXPECT_EQ(v.insert(v.begin() + 1, 9), v.begin() + 1);
  EXPECT_EQ(contents(v), (std::vector<int>{1, 9, 2, 3}));
  EXPECT_TRUE(ok);

  EXPECT_EQ(v.insert(v.begin(), 8), nullptr);
  EXPECT_EQ(contents(v), (std::vector<int>{1, 9, 2, 3}));
  EXPECT_EQ(storage[4], SENTINEL);
  EXPECT_FALSE(ok);
}

TEST(FixedVector, EraseShiftsTheTailDown) {
  int storage[4];
  bool ok = true;
  ot::FixedVector<int> v;
  v.bind(storage, 4, &ok);
  for (int i = 1; i <= 4; i++) v.push_back(i);
  EXPECT_EQ(v.erase(v.begin() + 1), v.begin() + 1);
  EXPECT_EQ(contents(v), (std::vector<int>{1, 3, 4}));
  EXPECT_TRUE(ok);
}

TEST(FixedVector, ResizeValueInitialises) {
  int storage[5] = {-1, -1, -1, -1, SENTINEL};
  bool ok = true;
  ot::FixedVector<int> v;
  v.bind(storage, 4, &ok);
  v.push_back(7);
  EXPECT_TRUE(v.resize(3));
  EXPECT_EQ(contents(v), (std::vector<int>{7, 0, 0}));
  EXPECT_TRUE(v.resize(1));
  EXPECT_EQ(contents(v), (std::vector<int>{7}));
  EXPECT_TRUE(ok);

  EXPECT_FALSE(v.resize(5));
  EXPECT_EQ(v.size(), 1u);
  EXPECT_EQ(storage[4], SENTINEL);
  EXPECT_FALSE(ok);
}

TEST(FixedVector, AssignFills) {
  int storage[5] = {0, 0, 0, 0, SENTINEL};
  bool ok = true;
  ot::FixedVector<int> v;
  v.bind(storage, 4, &ok);
  EXPECT_TRUE(v.assign(3, 5));
  EXPECT_EQ(contents(v), (std::vector<int>{5, 5, 5}));
  EXPECT_TRUE(ok);

  EXPECT_FALSE(v.assign(5, 1));
  EXPECT_EQ(contents(v), (std::vector<int>{5, 5, 5}));
  EXPECT_EQ(storage[4], SENTINEL);
  EXPECT_FALSE(ok);
}

TEST(FixedVector, SwapExchangesBindings) {
  int storageA[4], storageB[3];
  bool okA = true, okB = true;
  ot::FixedVector<int> a, b;
  a.bind(storageA, 4, &okA);
  b.bind(storageB, 3, &okB);
  a.push_back(1);
  a.push_back(2);
  b.push_back(3);
  a.swap(b);
  EXPECT_EQ(a.data(), storageB);
  EXPECT_EQ(a.size(), 1u);
  EXPECT_EQ(a.capacity(), 3u);
  EXPECT_EQ(b.data(), storageA);
  EXPECT_EQ(b.size(), 2u);
  EXPECT_EQ(b.capacity(), 4u);
  EXPECT_EQ(contents(a), (std::vector<int>{3}));
  EXPECT_EQ(contents(b), (std::vector<int>{1, 2}));

  // The flags went with the bindings: a growth past `a` clears B's flag.
  EXPECT_TRUE(a.resize(3));
  EXPECT_FALSE(a.push_back(4));
  EXPECT_FALSE(okB);
  EXPECT_TRUE(okA);
  EXPECT_FALSE(b.resize(5));
  EXPECT_FALSE(okA);
}

TEST(FixedVector, MoveLeavesTheSourceUnbound) {
  int storage[4];
  bool ok = true;
  ot::FixedVector<int> a;
  a.bind(storage, 4, &ok);
  a.push_back(1);
  a.push_back(2);
  ot::FixedVector<int> c;
  c = std::move(a);
  EXPECT_EQ(a.capacity(), 0u);  // NOLINT(bugprone-use-after-move): the moved-from state is the point
  EXPECT_EQ(a.size(), 0u);      // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(c.data(), storage);
  EXPECT_EQ(c.capacity(), 4u);
  EXPECT_EQ(contents(c), (std::vector<int>{1, 2}));
  EXPECT_TRUE(ok);
}

TEST(FixedVector, AnUnboundVectorRefusesEverything) {
  ot::FixedVector<int> never;
  EXPECT_EQ(never.capacity(), 0u);
  EXPECT_FALSE(never.push_back(1));

  bool ok = true;
  ot::FixedVector<int> v;
  v.bind(nullptr, 0, &ok);
  EXPECT_EQ(v.capacity(), 0u);
  EXPECT_FALSE(v.push_back(1));
  EXPECT_FALSE(v.resize(1));
  EXPECT_FALSE(v.assign(1, 1));
  EXPECT_EQ(v.insert(v.begin(), 1), nullptr);
  EXPECT_TRUE(v.empty());
  EXPECT_FALSE(ok);
}
