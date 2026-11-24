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
