#pragma once
#include <WString.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace FsHelpers {

std::string decodeUriEscapes(const std::string& path);

std::string normalisePath(const std::string& path);
// Out-parameter overload that reuses `out`'s capacity and performs the
// normalisation in-place without allocating a temporary components vector.
// Use inside hot loops to keep heap fragmentation bounded.
void normalisePath(const std::string& path, std::string& out);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(std::string_view fileName, const char* extension);
inline bool checkFileExtension(const String& fileName, const char* extension) {
  return checkFileExtension(std::string_view{fileName.c_str(), fileName.length()}, extension);
}

// Check for either .jpg or .jpeg extension (case-insensitive)
bool hasJpgExtension(std::string_view fileName);
inline bool hasJpgExtension(const String& fileName) {
  return hasJpgExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .png extension (case-insensitive)
bool hasPngExtension(std::string_view fileName);
inline bool hasPngExtension(const String& fileName) {
  return hasPngExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .bmp extension (case-insensitive)
bool hasBmpExtension(std::string_view fileName);

// Check for .gif extension (case-insensitive)
bool hasGifExtension(std::string_view fileName);
inline bool hasGifExtension(const String& fileName) {
  return hasGifExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .epub extension (case-insensitive)
bool hasEpubExtension(std::string_view fileName);
inline bool hasEpubExtension(const String& fileName) {
  return hasEpubExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for either .xtc or .xtch extension (case-insensitive)
bool hasXtcExtension(std::string_view fileName);
inline bool hasXtcExtension(const String& fileName) {
  return hasXtcExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .txt extension (case-insensitive)
bool hasTxtExtension(std::string_view fileName);
inline bool hasTxtExtension(const String& fileName) {
  return hasTxtExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .md extension (case-insensitive)
bool hasMarkdownExtension(std::string_view fileName);
inline bool hasMarkdownExtension(const String& fileName) {
  return hasMarkdownExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .css extension (case-insensitive)
bool hasCssExtension(std::string_view fileName);
inline bool hasCssExtension(const String& fileName) {
  return hasCssExtension(std::string_view{fileName.c_str(), fileName.length()});
}

std::string extractFolderPath(const std::string& filePath);

/**
 * Sanitize a filename/path component for FAT32 in a caller-provided buffer.
 * Replaces invalid path characters, spaces, and control characters with '-'.
 */
void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen);

/**
 * Natural case-insensitive string compare: digit runs compare by numeric value
 * (leading zeros ignored), everything else byte-wise after tolower.
 * Returns <0, 0 or >0 like strcmp.
 */
int naturalCompare(const char* s1, const char* s2);

/**
 * Write an order-preserving byte encoding of `name` into `out` (up to `cap`
 * bytes, no terminator); returns the number of bytes written. Bytewise
 * comparison of two full keys matches naturalCompare on the original names;
 * truncated keys are a consistent coarsening (equal prefixes need a
 * naturalCompare fallback). Never emits 0x00, so fixed-size keys can be
 * zero-padded.
 */
size_t naturalSortKey(const char* name, uint8_t* out, size_t cap);

}  // namespace FsHelpers
