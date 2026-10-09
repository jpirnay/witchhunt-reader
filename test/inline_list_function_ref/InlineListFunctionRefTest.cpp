// The two pieces that keep ButtonNavigator off the heap: InlineList holds the button sets it
// resolves on every loop pass, FunctionRef the callbacks it runs before returning. ButtonNavigator
// itself needs MappedInputManager and HalGPIO and cannot build on host, so these pin what it relies
// on: the same buttons in the same order as the std::vector they replace, the caller's callable
// reached by reference, and no allocation on either.

#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>
#include <new>
#include <vector>

#include "util/FunctionRef.h"
#include "util/InlineList.h"

namespace {
std::size_t allocations = 0;
}  // namespace

// Counts every plain operator new in the binary, so a test can read the delta across the code it
// runs. Each "stays off the heap" test checks its counter against the type it replaced first: a
// hook that saw nothing would make a zero meaningless.
void* operator new(const std::size_t size) {
  ++allocations;
  if (void* block = std::malloc(size == 0 ? 1 : size)) return block;
  throw std::bad_alloc();
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }

namespace {

// Stands in for MappedInputManager::Button: a scoped enum, which is what keeps a stray int out.
enum class Key { Back, Confirm, Left, Right, Up, Down };
using Keys = InlineList<Key, 2>;

// The getters' shape: `return {buttonFor(...), buttonFor(...)};`.
Keys stepAndPage(const Key step, const Key page) { return {step, page}; }
Keys stepOnly(const Key step) { return {step}; }

// The call sites' shape: a braced list straight into a `const Buttons&` parameter.
std::vector<Key> asVector(const Keys& keys) { return {keys.begin(), keys.end()}; }

// Kept out of line so the optimiser cannot drop the control allocations below as unused.
[[gnu::noinline]] Key lastOf(const std::vector<Key>& keys) { return keys.back(); }
[[gnu::noinline]] void invoke(const std::function<void()>& callback) { callback(); }

void runTwice(const FunctionRef<void()> callback) {
  callback();
  callback();
}
// ButtonNavigator::onNext hands its callback on by value to onNextPress and onNextContinuous.
void handOn(const FunctionRef<void()> callback) {
  runTwice(callback);
  callback();
}

TEST(InlineList, KeepsTheItemsAndOrderOfTheVectorItReplaced) {
  EXPECT_EQ((std::vector<Key>{Key::Down, Key::Right}), asVector(stepAndPage(Key::Down, Key::Right)));
  EXPECT_EQ((std::vector<Key>{Key::Right, Key::Down}), asVector(stepAndPage(Key::Right, Key::Down)));
  EXPECT_EQ((std::vector<Key>{Key::Up}), asVector(stepOnly(Key::Up)));
  EXPECT_EQ((std::vector<Key>{Key::Left}), asVector({Key::Left}));
  EXPECT_EQ(2u, stepAndPage(Key::Up, Key::Left).size());
  EXPECT_EQ(1u, stepOnly(Key::Up).size());
}

TEST(InlineList, StaysOffTheHeap) {
  std::size_t before = allocations;
  EXPECT_EQ(Key::Right, lastOf(std::vector<Key>{Key::Down, Key::Right}));
  ASSERT_GT(allocations, before) << "the counting hook missed a std::vector";

  before = allocations;
  std::size_t seen = 0;
  for (int pass = 0; pass < 100; ++pass) {
    for (const Key key : stepAndPage(Key::Down, Key::Right)) seen += static_cast<std::size_t>(key);
  }
  EXPECT_EQ(before, allocations);
  EXPECT_EQ(100u * (static_cast<std::size_t>(Key::Down) + static_cast<std::size_t>(Key::Right)), seen);
}

TEST(FunctionRef, ReachesTheCallersCallableNotACopy) {
  int last = 0;
  auto counter = [calls = 0, &last]() mutable { last = ++calls; };
  const FunctionRef<void()> ref(counter);
  ref();
  ref();
  EXPECT_EQ(2, last);
  counter();  // a copy inside the ref would have left the original at 0
  EXPECT_EQ(3, last);
}

TEST(FunctionRef, ForwardsArgumentsAndResult) {
  const auto twice = [](const int value) { return value * 2; };
  const FunctionRef<int(int)> ref(twice);
  EXPECT_EQ(42, ref(21));
}

TEST(FunctionRef, SurvivesBeingHandedOnByValue) {
  int calls = 0;
  handOn([&calls] { ++calls; });
  EXPECT_EQ(3, calls);
}

TEST(FunctionRef, StaysOffTheHeap) {
  // The page jump's shape: a lambda capturing several references, which outgrows std::function's
  // inline buffer both here (16 bytes) and on the C3 (8).
  int selected = 0;
  int total = 50;
  int page = 10;
  bool forward = true;
  int changes = 0;
  const auto jump = [&] {
    selected = forward ? (selected + page) % total : selected;
    ++changes;
  };

  std::size_t before = allocations;
  invoke(jump);
  ASSERT_GT(allocations, before) << "the counting hook missed a std::function";

  before = allocations;
  for (int pass = 0; pass < 100; ++pass) runTwice(jump);
  EXPECT_EQ(before, allocations);
  EXPECT_EQ(201, changes);
  EXPECT_EQ(10, selected);  // 201 jumps of 10 rows through 50
}

}  // namespace
