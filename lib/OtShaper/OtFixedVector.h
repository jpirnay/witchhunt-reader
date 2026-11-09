#pragma once

// witchhunt: deviation 1 - not upstream's. Upstream's buffer holds
// std::vectors and reserve()s them after asking the heap check; under
// -fno-exceptions an allocation that still fails aborts. This view takes
// their place: it works over memory its owner binds, so shaping allocates
// nothing, and a growth it cannot take fails the run as Buffer::canGrow does.

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

#include "OtHot.h"  // exp/shaper-iram

namespace ot {

// The calls lib/OtShaper makes of std::vector, over memory its owner binds;
// it never allocates. A growth past the capacity changes nothing, returns
// false (insert: nullptr) and clears the flag given to bind().
template <typename T>
class FixedVector {
  static_assert(std::is_trivially_copyable_v<T>, "elements are moved with memmove and never destroyed");

 public:
  FixedVector() = default;  // unbound: capacity 0
  FixedVector(const FixedVector&) = delete;
  FixedVector& operator=(const FixedVector&) = delete;
  FixedVector(FixedVector&& other) noexcept { take(other); }
  FixedVector& operator=(FixedVector&& other) noexcept {
    if (this != &other) take(other);
    return *this;
  }

  // Binds `capacity` elements at `data`, empty. A null `data` binds nothing.
  void bind(T* data, const size_t capacity, bool* fits = nullptr) {
    data_ = data;
    size_ = 0;
    capacity_ = data ? capacity : 0;
    fits_ = fits;
  }

  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool empty() const { return size_ == 0; }
  T* data() { return data_; }
  const T* data() const { return data_; }
  T* begin() { return data_; }
  T* end() { return data_ + size_; }
  const T* begin() const { return data_; }
  const T* end() const { return data_ + size_; }
  T& operator[](const size_t i) { return data_[i]; }
  const T& operator[](const size_t i) const { return data_[i]; }
  T& back() { return data_[size_ - 1]; }
  const T& back() const { return data_[size_ - 1]; }

  void clear() { size_ = 0; }

  OT_HOT bool push_back(const T& value) {
    if (size_ >= capacity_) return refuse();
    data_[size_++] = value;
    return true;
  }

  // New elements are value-initialised.
  bool resize(const size_t n) {
    if (n > capacity_) return refuse();
    for (size_t i = size_; i < n; i++) data_[i] = T{};
    size_ = n;
    return true;
  }

  bool assign(const size_t n, const T& value) {
    if (n > capacity_) return refuse();
    const T copy = value;  // `value` may be one of the elements
    for (size_t i = 0; i < n; i++) data_[i] = copy;
    size_ = n;
    return true;
  }

  T* insert(T* const at, const T& value) {
    if (size_ >= capacity_) {
      refuse();
      return nullptr;
    }
    const T copy = value;  // `value` may be one of the elements moved
    const size_t i = static_cast<size_t>(at - data_);
    std::memmove(data_ + i + 1, data_ + i, (size_ - i) * sizeof(T));
    data_[i] = copy;
    size_++;
    return data_ + i;
  }

  T* erase(T* const at) {
    const size_t i = static_cast<size_t>(at - data_);
    std::memmove(data_ + i, data_ + i + 1, (size_ - i - 1) * sizeof(T));
    size_--;
    return data_ + i;
  }

  // Exchanges the bindings: data, size, capacity and flag.
  void swap(FixedVector& other) {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(fits_, other.fits_);
  }

 private:
  bool refuse() {
    if (fits_) *fits_ = false;
    return false;
  }

  // Takes `other`'s binding and leaves it unbound.
  void take(FixedVector& other) {
    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    fits_ = other.fits_;
    other.data_ = nullptr;
    other.size_ = other.capacity_ = 0;
    other.fits_ = nullptr;
  }

  T* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
  bool* fits_ = nullptr;
};

}  // namespace ot
