#pragma once
// Host stand-in for the Arduino core's base64 encoder (RFC 4648, padded, no line breaks).

#include <cstddef>
#include <cstdint>
#include <string>

#include "WString.h"

class base64 {
 public:
  static String encode(const uint8_t* data, const size_t length) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < length; i += 3) {
      const uint32_t b0 = data[i];
      const uint32_t b1 = i + 1 < length ? data[i + 1] : 0;
      const uint32_t b2 = i + 2 < length ? data[i + 2] : 0;
      const uint32_t bits = (b0 << 16) | (b1 << 8) | b2;
      out += kAlphabet[(bits >> 18) & 63];
      out += kAlphabet[(bits >> 12) & 63];
      out += i + 1 < length ? kAlphabet[(bits >> 6) & 63] : '=';
      out += i + 2 < length ? kAlphabet[bits & 63] : '=';
    }
    return String(out);
  }
};
