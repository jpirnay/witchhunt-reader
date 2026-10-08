#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// A list of at most `Capacity` items, held inline: no heap, and trivially copyable when T is.
//
// For short lists that are built, read and dropped many times a second. ButtonNavigator resolves
// its button sets afresh on every loop pass (the buttons follow the screen orientation), and a
// std::vector there was a malloc/free pair per set per tick. The capacity is checked when the
// list is built: an initialiser longer than it does not compile.
template <typename T, std::size_t Capacity>
class InlineList {
  static_assert(Capacity > 0 && Capacity <= UINT8_MAX, "InlineList: the count is a uint8_t");

 public:
  // Implicit on purpose: call sites pass braced lists, and getters `return {a, b};`.
  template <typename... Items>
    requires(std::is_same_v<Items, T> && ...)
  constexpr InlineList(Items... items) : storage{items...}, count(sizeof...(Items)) {
    static_assert(sizeof...(Items) <= Capacity, "InlineList: more items than its capacity; raise the capacity");
  }

  [[nodiscard]] constexpr const T* begin() const { return storage; }
  [[nodiscard]] constexpr const T* end() const { return storage + count; }
  [[nodiscard]] constexpr std::size_t size() const { return count; }

 private:
  T storage[Capacity]{};
  uint8_t count = 0;
};
