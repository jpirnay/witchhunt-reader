#pragma once

#include <string>

namespace StringUtils {

/**
 * Case-insensitive ASCII strcmp. Returns <0, 0, or >0 like strcmp, comparing
 * each byte by its lowercased value; bytes >= 0x80 are compared unchanged.
 *
 * This is the exact ordering glib's g_ascii_strcasecmp() imposes, which is what
 * StarDict index files are sorted by, so the dictionary's binary search must use
 * it too -- plain strcmp lands the descent on the wrong page for any word whose
 * alphabetic neighbourhood crosses a mixed-case boundary. Folding is written out
 * rather than deferred to std::tolower() so the ordering cannot drift with the
 * C locale.
 *
 * Inline because the descent calls it once per comparison step.
 */
inline int asciiCaseCmp(const char* a, const char* b) {
  const auto fold = [](const char c) {
    const auto u = static_cast<unsigned char>(c);
    return static_cast<int>(u >= 'A' && u <= 'Z' ? u + ('a' - 'A') : u);
  };
  while (*a && *b) {
    const int diff = fold(*a) - fold(*b);
    if (diff != 0) return diff;
    ++a;
    ++b;
  }
  return fold(*a) - fold(*b);
}

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = 100);

}  // namespace StringUtils
