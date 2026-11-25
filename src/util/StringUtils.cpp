#include "StringUtils.h"

#include <Utf8.h>

namespace StringUtils {

std::string sanitizeFilename(const std::string& name, size_t maxBytes) {
  std::string result;
  result.reserve(std::min(name.size(), maxBytes));

  const auto* text = reinterpret_cast<const unsigned char*>(name.c_str());

  // Skip leading spaces and dots so they don't consume the byte budget
  while (*text == ' ' || *text == '.') {
    text++;
  }

  // Process full UTF-8 codepoints to avoid trimming in the middle of a multibyte sequence
  while (*text != 0) {
    const auto* cpStart = text;
    uint32_t cp = utf8NextCodepoint(&text);
    // A byte that is not part of a character (a title in Latin-1, as an XTC header can carry): utf8NextCodepoint()
    // reads it as U+FFFD and steps over it. Copied in, it made a long name that is not UTF-8, which SdFat refuses: a
    // screenshot named after such a book was lost. A U+FFFD written as one (EF BF BD) stays.
    const bool notUtf8 = cp == REPLACEMENT_GLYPH &&
                         !(text - cpStart == 3 && cpStart[0] == 0xEF && cpStart[1] == 0xBF && cpStart[2] == 0xBD);

    if (notUtf8 || cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' ||
        cp == '>' || cp == '|') {
      // Replace illegal and control characters with '_'
      if (result.length() + 1 > maxBytes) break;
      result += '_';
    } else if (cp >= 128 || (cp >= 32 && cp < 127)) {
      const size_t cpBytes = text - cpStart;
      if (result.length() + cpBytes > maxBytes) break;
      result.append(reinterpret_cast<const char*>(cpStart), cpBytes);
    }
  }

  // Trim trailing spaces and dots
  size_t end = result.find_last_not_of(" .");
  if (end != std::string::npos) {
    result.resize(end + 1);
  } else {
    result.clear();
  }

  return result.empty() ? "book" : result;
}

}  // namespace StringUtils
