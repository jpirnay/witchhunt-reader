#pragma once
// In-memory HalStorage for JsonFile and the stores that write through it: a map stands in for the
// card's files, and every writeFile() is counted.

#include <map>
#include <string>

#include "WString.h"

namespace json_file_fixture {

struct State {
  std::map<std::string, std::string> files;
  int writes = 0;
};

inline State state;

}  // namespace json_file_fixture

class HalStorage {
 public:
  bool exists(const char* path) const { return json_file_fixture::state.files.count(path) != 0; }
  String readFile(const char* path) const {
    const auto it = json_file_fixture::state.files.find(path);
    return it == json_file_fixture::state.files.end() ? String() : String(it->second.c_str());
  }
  // As on the card (SDCardManager::writeFile): the whole content replaces the file.
  bool writeFile(const char* path, const String& content) {
    ++json_file_fixture::state.writes;
    json_file_fixture::state.files[path] = std::string(content.c_str(), content.length());
    return true;
  }
  bool mkdir(const char*, bool = true) { return true; }

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
};

#define Storage HalStorage::getInstance()
