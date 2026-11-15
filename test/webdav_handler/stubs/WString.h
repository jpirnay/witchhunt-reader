#pragma once

// The part of the Arduino String that WebDAVHandler.cpp and WebPathGuard.cpp use, for WebDAVHandlerTest. Shadows
// test/shims/WString.h, which has too little of it. Arduino semantics: indexOf() is -1 when absent, substring()
// clamps.

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

class String {
 public:
  String() = default;
  String(const char* s) : buf_(s ? s : "") {}  // implicit, as Arduino's is
  explicit String(std::string s) : buf_(std::move(s)) {}

  const char* c_str() const { return buf_.c_str(); }
  unsigned int length() const { return static_cast<unsigned int>(buf_.length()); }
  bool isEmpty() const { return buf_.empty(); }
  char charAt(const unsigned int i) const { return i < buf_.size() ? buf_[i] : '\0'; }
  char operator[](const unsigned int i) const { return charAt(i); }

  int indexOf(const char c, const unsigned int from = 0) const { return position(buf_.find(c, from)); }
  int indexOf(const char* s, const unsigned int from = 0) const { return position(buf_.find(s, from)); }
  int lastIndexOf(const char c) const { return position(buf_.rfind(c)); }
  String substring(const unsigned int from) const { return substring(from, length()); }
  String substring(const unsigned int from, unsigned int to) const {
    if (to > buf_.size()) to = length();
    return from >= to ? String() : String(buf_.substr(from, to - from));
  }
  bool startsWith(const char* prefix) const { return buf_.rfind(prefix, 0) == 0; }
  bool endsWith(const char* suffix) const {
    const size_t n = std::strlen(suffix);
    return buf_.size() >= n && buf_.compare(buf_.size() - n, n, suffix) == 0;
  }
  void toLowerCase() {
    for (char& c : buf_) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
  }

  String& operator+=(const String& s) {
    buf_ += s.buf_;
    return *this;
  }
  String& operator+=(const char* s) {
    buf_ += s ? s : "";
    return *this;
  }
  String& operator+=(const char c) {
    buf_ += c;
    return *this;
  }
  friend String operator+(String a, const String& b) { return a += b; }
  friend String operator+(String a, const char* b) { return a += b; }
  friend String operator+(const char* a, const String& b) { return String(a) += b; }

  bool operator==(const String& o) const { return buf_ == o.buf_; }
  bool operator==(const char* s) const { return buf_ == (s ? s : ""); }
  bool operator!=(const String& o) const { return !(*this == o); }
  bool operator!=(const char* s) const { return !(*this == s); }

 private:
  static int position(const size_t p) { return p == std::string::npos ? -1 : static_cast<int>(p); }

  std::string buf_;
};
