#include "JsonText.h"

// The only deserializeJson() call in the firmware (see JsonText.h).
DeserializationError JsonText::parse(JsonDocument& doc, const char* text, const size_t length) {
  return deserializeJson(doc, text, length);
}
