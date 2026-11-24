#include "JsonFile.h"

#include <HalStorage.h>
#include <JsonText.h>
#include <Logging.h>

namespace JsonFile {

bool serializeWhole(const char* tag, const char* path, const JsonDocument& doc, String& out) {
  if (doc.overflowed()) {
    LOG_ERR(tag, "%s not saved: JSON document out of memory", path);
    return false;
  }
  const size_t expected = JsonText::measure(doc);
  serializeJson(doc, out);
  if (out.length() != expected) {
    LOG_ERR(tag, "%s not saved: %u of %u bytes of JSON fitted", path, static_cast<unsigned>(out.length()),
            static_cast<unsigned>(expected));
    return false;
  }
  return true;
}

bool save(const char* tag, const char* path, const JsonDocument& doc) {
  String json;
  return serializeWhole(tag, path, doc, json) && Storage.writeFile(path, json);
}

}  // namespace JsonFile
