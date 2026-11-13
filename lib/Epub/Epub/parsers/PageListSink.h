#pragma once

#include <BufferedFileIO.h>
#include <HalStorage.h>

#include <optional>
#include <string>

// Streams printed-page entries (href, anchor, label) directly to pagelist.bin
// instead of buffering them in a std::vector. Buffering blows the X3 heap on
// books with long EPUB 3 <nav epub:type="page-list"> sections (a few hundred
// entries × 3 std::string each is enough to throw std::bad_alloc).
//
// File format matches the legacy writePageListBin() in Epub.cpp so the reader
// side (Epub::streamPrintedPageEntries) is unchanged: u16 count, then per entry
// writeString(href), writeString(anchor), writeString(label).
//
// Lifecycle: construct (opens the file and writes a placeholder count of 0),
// addEntry(...) for each parsed entry, then finalize() to seek back and patch
// the real count. If no entries were added, finalize() removes the file —
// matching the legacy writer's "remove on empty" behaviour so subsequent
// fallback parsers (NCX after nav, page-map after both) see no stale file.
//
// Not copyable; not thread-safe (each book parse is single-threaded).
class PageListSink {
 public:
  explicit PageListSink(const std::string& cachePath);
  ~PageListSink();

  PageListSink(const PageListSink&) = delete;
  PageListSink& operator=(const PageListSink&) = delete;

  // True if the file was opened successfully. When false, addEntry() and
  // finalize() are no-ops; callers don't need to check before each push.
  bool isOpen() const { return file.isOpen(); }

  // Number of entries successfully streamed so far. Callers (e.g. Epub.cpp
  // orchestrator deciding whether the NCX fallback should run) read this
  // instead of inspecting the file.
  uint16_t entryCount() const { return count; }

  void addEntry(const std::string& href, const std::string& anchor, const std::string& label);

  // Patches the placeholder count at offset 0 with the real entry count and
  // closes the file. If no entries were added, the file is removed instead.
  // Safe to call multiple times — subsequent calls are no-ops.
  void finalize();

  // Closes the file and removes it, whatever was streamed: for a parse that failed, so no partial
  // list outlives it and a fallback parser starts from no file. Removing pagelist.bin from outside
  // instead deleted it under the open handle, which the destructor then patched and closed.
  void discard();

 private:
  std::string path;
  FsFile file;
  // An entry is three strings, six writes: 4,058 SdFat calls for a 676-entry page list at a book's first open (X3,
  // 2026-11-13). They go through a 512-byte buffer from the heap (unbuffered if the block cannot be had).
  std::optional<serialization::BufferedFileWriter> writer;
  uint16_t count = 0;
  bool finalized = false;
};
