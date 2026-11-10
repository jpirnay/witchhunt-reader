#pragma once

#include <cstring>
#include <string>

// The script a book's reading font is offered and declined by (BookPackOffer): the family without its Noto stem,
// "Devanagari" of NotoSerifDevanagari, "JP" of NotoSansJP, "Arabic" of NotoNaskhArabic. Both styles of a script are
// one offer: no to one is no to the other.
namespace ReadingFontScript {

inline std::string of(const char* family) {
  for (const char* stem : {"NotoSans", "NotoSerif", "NotoNaskh"}) {
    const size_t length = std::strlen(stem);
    if (std::strncmp(family, stem, length) == 0) return family + length;
  }
  return family;
}

// Whether `script` is in `list`, comma-separated ("Devanagari,JP"): a whole entry, never a part of one.
inline bool listed(const std::string& list, const std::string& script) {
  if (script.empty()) return false;
  for (size_t start = 0; start <= list.size();) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    if (list.compare(start, end - start, script) == 0) return true;
    start = end + 1;
  }
  return false;
}

// `list` with `script` added, when it is not there already.
inline void add(std::string& list, const std::string& script) {
  if (script.empty() || listed(list, script)) return;
  if (!list.empty()) list += ',';
  list += script;
}

}  // namespace ReadingFontScript
