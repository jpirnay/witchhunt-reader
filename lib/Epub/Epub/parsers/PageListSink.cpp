#include "PageListSink.h"

#include <Logging.h>
#include <Serialization.h>

PageListSink::PageListSink(const std::string& cachePath) : path(cachePath + "/pagelist.bin") {
  if (!Storage.openFileForWrite("EBP", path, file)) {
    LOG_ERR("EBP", "PageListSink: could not open pagelist.bin for writing");
    return;
  }
  // Placeholder count; patched in finalize().
  serialization::writePod(file, static_cast<uint16_t>(0));
  writer.emplace(file, 512);
}

PageListSink::~PageListSink() {
  if (!finalized) {
    finalize();
  }
}

void PageListSink::addEntry(const std::string& href, const std::string& anchor, const std::string& label) {
  if (!file.isOpen() || finalized) return;
  writer->writeString(href);
  writer->writeString(anchor);
  writer->writeString(label);
  count++;
}

void PageListSink::discard() {
  finalized = true;
  if (writer) writer->discard();
  if (file.isOpen()) file.close();
  Storage.remove(path.c_str());
}

void PageListSink::finalize() {
  if (finalized) return;
  finalized = true;
  if (!file.isOpen()) return;

  if (count == 0) {
    if (writer) writer->discard();
    file.close();
    Storage.remove(path.c_str());
    return;
  }

  if (writer && !writer->flush()) {
    LOG_ERR("EBP", "PageListSink: writing the entries failed");
    file.close();
    Storage.remove(path.c_str());
    return;
  }
  file.flush();
  if (!file.seek(0)) {
    LOG_ERR("EBP", "PageListSink: could not seek to patch count");
    file.close();
    return;
  }
  serialization::writePod(file, count);
  file.flush();
  file.close();
  LOG_DBG("EBP", "Wrote pagelist.bin with %u entries", static_cast<unsigned>(count));
}
