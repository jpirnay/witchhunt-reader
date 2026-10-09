#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <string>

// Exact reads and writes on the index's files, and its strings: a uint16_t length, then the bytes.
namespace library {

bool readExact(HalFile& file, void* out, size_t len);
bool writeExact(HalFile& file, const void* data, size_t len);
// False when the string exceeds MAX_STRING or the file came up short.
bool readBlobString(HalFile& file, std::string& out);
bool writeBlobString(HalFile& file, const std::string& s);

// One entry of a build's names file: the author's hash, whether its key came from a file-as, its
// display name and sort key. The one place that format is written.
bool appendNameEntry(HalFile& names, uint32_t hash, bool fromFileAs, const std::string& name, const std::string& key);

}  // namespace library
