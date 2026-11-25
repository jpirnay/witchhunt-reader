#pragma once

// Ported from crosspoint-reader (PR #2583 by Uri Tauber).

#include <cstddef>
#include <string>
#include <vector>

// One StarDict dictionary found under /dictionaries or /.dictionaries: a
// subfolder holding <stem>.idx plus <stem>.dict or <stem>.dict.dz.
struct DictionaryEntry {
  std::string name;  // subfolder name (shown to the user, stored in settings)
  std::string stem;  // index basename without .idx
};

namespace DictionaryRegistry {

// Bytes, with the terminator, of a folder or index file name the scan reads from the card. A longer name is not listed.
// SETTINGS.dictionaryName is this size, so every folder listed can be stored as the chosen dictionary whole: at 32
// bytes a longer name was stored cut short and opened nothing (crosspoint-reader PR #4507, kanavnet).
inline constexpr size_t MAX_NAME_BYTES = 128;

// Scan /dictionaries/*/ and /.dictionaries/*/ for dictionaries. Folders with
// multiple index stems are ambiguous and skipped. Result is sorted
// case-insensitively by name.
void discover(std::vector<DictionaryEntry>& out);

// Resolve a folder name to its extensionless base path
// ("/dictionaries/<folder>/<stem>" or "/.dictionaries/<folder>/<stem>").
// Returns false if the folder holds no usable dictionary in either root.
bool resolveBasePath(const char* folderName, std::string& basePathOut);

}  // namespace DictionaryRegistry
