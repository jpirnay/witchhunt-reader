#include "ByteSize.h"

#include <cstdio>

namespace byte_size {

int formatTenths(char* out, const size_t outSize, const uint64_t bytes, const unsigned shift, const char* unit) {
  const uint64_t tenths = rounded(bytes, shift, 10);
  return snprintf(out, outSize, "%llu.%u%s%s", static_cast<unsigned long long>(tenths / 10),
                  static_cast<unsigned>(tenths % 10), unit ? " " : "", unit ? unit : "");
}

}  // namespace byte_size
