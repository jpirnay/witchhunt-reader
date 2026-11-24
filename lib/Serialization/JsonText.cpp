#include "JsonText.h"

// The firmware's deserializeJson() call and its serializeJson() calls but those into a String (see JsonText.h).

DeserializationError JsonText::parse(JsonDocument& doc, const char* text, const size_t length) {
  return deserializeJson(doc, text, length);
}

size_t JsonText::measure(const JsonVariantConst value) { return measureJson(value); }

// Measured, then written into the string's own buffer by the char-buffer writer below, rather than by
// ArduinoJson's std::string writer: that one stays in the image for as<std::string>(), but its
// serializer loop is not compiled a second time here (X3: -458 B).
size_t JsonText::write(const JsonVariantConst value, std::string& out) {
  const size_t at = out.size();
  const size_t length = measureJson(value);
  out.resize(at + length);
  // The NUL the writer ends with lands on out[size()], which holds one already.
  return serializeJson(value, out.data() + at, length + 1);
}

size_t JsonText::write(const JsonVariantConst value, char* buffer, const size_t size) {
  return serializeJson(value, buffer, size);
}

#if ARDUINOJSON_ENABLE_ARDUINO_PRINT
size_t JsonText::write(const JsonVariantConst value, Print& out) { return serializeJson(value, out); }
#endif
