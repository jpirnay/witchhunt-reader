#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstring>
#include <string>

// The firmware's one way to parse JSON text and to write it.
//
// ArduinoJson's parser and serializer are templates over the input and the output type, and every file
// that calls deserializeJson() or serializeJson() compiles its own: one set of functions per type
// (std::string, String, const char*, and each class derived from Print separately), and at -Os a further
// private copy of the main loop in each calling file. On the X3 the parser alone came to 14 KB of flash.
// Every parse and every write goes through the functions below instead, defined in one file, so the
// firmware carries each once. (Arduino's String is written in one place too: JsonFile::serializeWhole.)
namespace JsonText {

// ---- Parsing: the same results as deserializeJson() on the same text. The input is read up to
// `length` (a C string up to its NUL), never written, and the document copies what it keeps. ----

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

// ---- Writing: what serializeJson() and measureJson() give for the same value. ----

// The bytes write() gives for `value`, without writing them.
size_t measure(JsonVariantConst value);
// Appended to `out`; the bytes written.
size_t write(JsonVariantConst value, std::string& out);
// Into `buffer`, NUL-terminated and cut at `size` - 1 bytes; the bytes written.
size_t write(JsonVariantConst value, char* buffer, size_t size);
#if ARDUINOJSON_ENABLE_ARDUINO_PRINT
// To any Print (a file, a chunked HTTP response), through the one Print writer rather than one per
// class derived from it.
size_t write(JsonVariantConst value, Print& out);
#endif

}  // namespace JsonText
