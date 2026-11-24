#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstring>

// The firmware's one way to parse JSON text.
//
// ArduinoJson's parser is a template over the input type, and every file that calls deserializeJson()
// compiles its own: one set of parser functions per input type (std::string, String, const String,
// const char*), and at -Os a further private copy of the main parse loop in each calling file. On the
// X3 that came to 14 KB of flash for one parser. Every parse goes through parse(doc, text, length)
// below instead, defined in one file, so the firmware carries the parser once.
//
// Same results as deserializeJson() on the same text: the input is read up to `length` (a C string
// up to its NUL), never written, and the document copies what it keeps.
namespace JsonText {

DeserializationError parse(JsonDocument& doc, const char* text, size_t length);

inline DeserializationError parse(JsonDocument& doc, const char* text) {
  return parse(doc, text, text != nullptr ? std::strlen(text) : 0);
}

// Any string with c_str() and length(): std::string and Arduino's String, without including WString.h
// here (the host tests of std::string-only code have no Arduino shims).
template <typename Text>
auto parse(JsonDocument& doc, const Text& text) -> decltype(text.c_str(), text.length(), DeserializationError()) {
  return parse(doc, text.c_str(), text.length());
}

}  // namespace JsonText
