#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

// Minimal host-side stand-in for the Arduino framework's WString/String.
// Provides the surface used by repo code compiled in host tests: c_str/length
// plus the startsWith/endsWith used on HalStorage::listFiles() results and the
// parse/build calls of KOReaderDocumentId's hash-cache file.
// Owns its bytes (std::string) so shims can return Strings for generated names.
class String {
 public:
  String() = default;
  String(const char* s) : _buf(s ? s : "") {}
  // explicit: an implicit std::string->String conversion makes calls ambiguous
  // where repo code overloads on both types (e.g. hasGifExtension).
  explicit String(std::string s) : _buf(std::move(s)) {}

  const char* c_str() const { return _buf.c_str(); }
  size_t length() const { return _buf.length(); }
  bool isEmpty() const { return _buf.empty(); }
  char operator[](size_t i) const { return i < _buf.size() ? _buf[i] : '\0'; }
  // Arduino semantics: -1 when absent, substring clamps, toInt() is 0 for no leading digits.
  int indexOf(char c) const {
    const size_t pos = _buf.find(c);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  String substring(size_t from) const { return substring(from, _buf.size()); }
  String substring(size_t from, size_t to) const {
    if (to > _buf.size()) to = _buf.size();
    return from >= to ? String() : String(_buf.substr(from, to - from));
  }
  long toInt() const { return std::strtol(_buf.c_str(), nullptr, 10); }
  String& operator+=(char c) {
    _buf += c;
    return *this;
  }
  String& operator+=(const char* s) {
    _buf += s ? s : "";
    return *this;
  }
  // Arduino's concat(): false, and the String as it was, when it cannot grow. ArduinoJson's String
  // writer appends through it. growLimit() stands in for a heap with no block big enough.
  bool concat(const char* s) {
    const size_t n = s ? std::strlen(s) : 0;
    if (_buf.size() + n > growLimit()) return false;
    _buf.append(s ? s : "", n);
    return true;
  }
  bool concat(const char* s, const unsigned int n) {
    if (_buf.size() + n > growLimit()) return false;
    _buf.append(s, n);
    return true;
  }
  // The longest a String may grow to through concat(); no limit unless a test sets one.
  static size_t& growLimit() {
    static size_t limit = static_cast<size_t>(-1);
    return limit;
  }
  bool operator==(const String& o) const { return _buf == o._buf; }
  bool operator!=(const String& o) const { return _buf != o._buf; }
  bool startsWith(const char* prefix) const { return _buf.rfind(prefix, 0) == 0; }
  bool endsWith(const char* suffix) const {
    const size_t n = std::strlen(suffix);
    return _buf.size() >= n && _buf.compare(_buf.size() - n, n, suffix) == 0;
  }

 private:
  std::string _buf;
};
