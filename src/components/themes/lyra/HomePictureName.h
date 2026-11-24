#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

// The name of a picture Home made for a widget, in its cache folder: pic_<path>_<bytes>_<w>x<h>.bmp, with
// the source's path as an FNV-1a hash and its size, both in eight hex digits. The size in the name makes a
// replaced picture be made again; the path apart from it finds what an earlier copy of the source left.
inline std::string homePictureName(const std::string_view source, const uint32_t sourceBytes, const int width,
                                   const int height) {
  uint32_t hash = 2166136261u;
  for (const char c : source) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  char name[64];
  snprintf(name, sizeof(name), "pic_%08x_%08x_%dx%d.bmp", static_cast<unsigned>(hash),
           static_cast<unsigned>(sourceBytes), width, height);
  return name;
}

// True when `other` was made from an earlier copy of `made`'s source at the same widget size: a picture a
// plugin fetches again on every sleep has a new size each time, and each such file would otherwise stay.
// A file of the same source at another size is kept: two widgets may show one picture.
inline bool replacesHomePicture(const std::string_view made, const std::string_view other) {
  constexpr size_t kPath = 13;  // "pic_" and the path's hash and "_"
  constexpr size_t kSize = 8;   // the size's hex digits
  const size_t n = made.size();
  if (n <= kPath + kSize || other.size() != n || memcmp(made.data(), "pic_", 4) != 0) return false;
  // The lengths are checked, so plain compares: string_view::substr would add a bounds check a call.
  return memcmp(other.data(), made.data(), kPath) == 0 &&
         memcmp(other.data() + kPath + kSize, made.data() + kPath + kSize, n - kPath - kSize) == 0 &&
         memcmp(other.data() + kPath, made.data() + kPath, kSize) != 0;
}

// A made picture's file as a number (FNV-1a), for the small tables that remember one by it.
inline uint32_t homePictureKey(const std::string_view target) {
  uint32_t h = 2166136261u;
  for (const char c : target) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  return h;
}

// The pictures Home could not make since the start, by the file each was to become (the target names
// the source's path and size, so a replaced source is a new picture). A corrupt file or a PNG past the
// decoder's cap used to say Loading for the whole visit and be decoded again on every return to Home,
// up to ~10 s on the C3. One that failed is drawn as such; it is tried again on a later visit until it
// has failed kTries times, as a failure may be the heap's rather than the file's. Bounded: past kMax
// pictures the oldest is forgotten, and at worst tried again.
class HomePictureFailures {
 public:
  static constexpr int kMax = 16;
  static constexpr uint8_t kTries = 2;

  void add(const std::string_view target) {
    const uint32_t h = homePictureKey(target);
    const int at = find(h);
    if (at >= 0) {
      if (counts_[at] < kTries) ++counts_[at];
      return;
    }
    hashes_[next_] = h;
    counts_[next_] = 1;
    next_ = static_cast<uint8_t>((next_ + 1) % kMax);
    if (used_ < kMax) ++used_;
  }
  // Failed at least once since the start.
  bool failed(const std::string_view target) const { return find(homePictureKey(target)) >= 0; }
  // Failed kTries times: not tried again until the next start.
  bool givenUp(const std::string_view target) const {
    const int at = find(homePictureKey(target));
    return at >= 0 && counts_[at] >= kTries;
  }

 private:
  int find(const uint32_t h) const {
    for (int i = 0; i < used_; ++i) {
      if (hashes_[i] == h) return i;
    }
    return -1;
  }

  uint32_t hashes_[kMax]{};
  uint8_t counts_[kMax]{};
  uint8_t used_ = 0;
  uint8_t next_ = 0;
};
