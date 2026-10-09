#pragma once

#include <string>

// Whether a folder, or any folder below it, holds a file of the kind asked for: a search that stops
// at the first one. The Library's Books tab leaves out the folders that hold no book.
//
// Depth first, one open directory per level, like FileBrowserModel::countBooksBelow. Pure storage
// walking -- what counts as a book, as hidden, or as already known comes in through Rules -- so it
// runs on the host over a real directory tree.
namespace FolderSearch {

struct Rules {
  // The entries looked at; the others, files and folders alike, are passed over (hidden entries).
  bool (*listable)(const char* name) = nullptr;
  // The files searched for.
  bool (*wanted)(const char* name) = nullptr;
  // How many levels of folders below the one asked about are searched.
  int maxDepth = 8;
  // Optional. What is already known about a folder: -1 nothing, 0 none below it, more than 0 some.
  // A known folder is answered from that and not entered.
  int (*known)(void* user, const std::string& path) = nullptr;
  // Optional. Told of each folder searched to its end without a find, within maxDepth all the way.
  void (*foundNone)(void* user, const std::string& path) = nullptr;
  // Optional, asked before each entry. True gives up, and the answer is then true: a folder that may
  // hold what is looked for is kept.
  bool (*stop)(void* user) = nullptr;
  void* user = nullptr;
};

// True when `dirPath` or a folder below it holds a file `wanted` accepts, or the search gave up.
bool anyBelow(const std::string& dirPath, const Rules& rules);

}  // namespace FolderSearch
