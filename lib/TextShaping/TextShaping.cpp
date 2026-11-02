#include "TextShaping.h"

#include <cstring>

#include "HebrewShaper.h"

namespace TextShaping {

bool shape(const char* text, size_t len, Unit* units, size_t capacity, size_t& count) {
  // Hebrew letters and points are U+0590-05FF; the lead byte 0xD7 covers U+05C0-05FF, which holds every letter.
  if (text == nullptr || memchr(text, 0xD7, len) == nullptr) return false;
  return HebrewShaper::shape(text, len, units, capacity, count);
}

}  // namespace TextShaping
