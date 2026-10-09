#include "LibraryBlob.h"

#include "LibraryFormat.h"

namespace library {

bool readExact(HalFile& file, void* out, const size_t len) {
  return len == 0 || file.read(out, len) == static_cast<int>(len);
}

bool writeExact(HalFile& file, const void* data, const size_t len) { return len == 0 || file.write(data, len) == len; }

bool readBlobString(HalFile& file, std::string& out) {
  uint16_t len = 0;
  if (!readExact(file, &len, sizeof(len)) || len > MAX_STRING) return false;
  out.resize(len);
  return readExact(file, len > 0 ? &out[0] : nullptr, len);
}

bool writeBlobString(HalFile& file, const std::string& s) {
  if (s.size() > MAX_STRING) return false;
  const auto len = static_cast<uint16_t>(s.size());
  return writeExact(file, &len, sizeof(len)) && writeExact(file, s.data(), s.size());
}

bool appendNameEntry(HalFile& names, const uint32_t hash, const bool fromFileAs, const std::string& name,
                     const std::string& key) {
  const uint8_t flags = fromFileAs ? 1 : 0;
  return writeExact(names, &hash, sizeof(hash)) && writeExact(names, &flags, sizeof(flags)) &&
         writeBlobString(names, name) && writeBlobString(names, key);
}

}  // namespace library
