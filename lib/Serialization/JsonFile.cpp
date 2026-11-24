#include "JsonFile.h"

#include <HalStorage.h>
#include <JsonText.h>
#include <Logging.h>

namespace JsonFile {

namespace {

// A String as a Print, so the document is written by the firmware's one Print serializer
// (JsonText::write) instead of ArduinoJson's String writer, a serializer of its own. Buffered as that
// writer is, because a String grows by one realloc per append. An append the heap refuses is dropped,
// and serializeWhole()'s length check reports it.
class StringPrint final : public Print {
 public:
  explicit StringPrint(String& out) : out_(out) {}
  ~StringPrint() override { flush(); }
  StringPrint(const StringPrint&) = delete;
  StringPrint& operator=(const StringPrint&) = delete;

  size_t write(const uint8_t c) override {
    if (used_ == sizeof(buffer_)) flush();
    buffer_[used_++] = static_cast<char>(c);
    return 1;
  }
  void flush() override {
    if (used_ > 0) out_.concat(buffer_, used_);
    used_ = 0;
  }

 private:
  String& out_;
  char buffer_[32] = {};
  unsigned used_ = 0;
};

}  // namespace

bool serializeWhole(const char* tag, const char* path, const JsonDocument& doc, String& out) {
  if (doc.overflowed()) {
    LOG_ERR(tag, "%s not saved: JSON document out of memory", path);
    return false;
  }
  const size_t expected = JsonText::measure(doc);
  {
    StringPrint sink(out);
    JsonText::write(doc, sink);
  }
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
