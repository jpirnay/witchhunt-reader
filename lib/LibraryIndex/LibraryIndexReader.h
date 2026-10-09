#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

// Reads a published book index. Holds one open file and its header; every record, name and path is
// read when asked for, so it costs the same whatever the card holds.
class LibraryIndexReader {
 public:
  LibraryIndexReader() = default;
  ~LibraryIndexReader() { close(); }
  LibraryIndexReader(const LibraryIndexReader&) = delete;
  LibraryIndexReader& operator=(const LibraryIndexReader&) = delete;

  // False, with nothing open, when the file is missing, from another version, or its sections do not
  // fit it. The caller treats all of those as "no index".
  bool open(const std::string& path);
  void close();
  bool isOpen() const { return opened; }

  const library::Header& header() const { return hdr; }
  bool partial() const { return (hdr.flags & library::FLAG_PARTIAL) != 0; }

  bool book(uint16_t index, library::BookRecord& out);
  // The record index of the rank-th newest book, 0 being the newest.
  bool newBook(uint16_t rank, uint16_t& recordIndex);
  bool author(uint16_t index, library::AuthorRecord& out);
  // The record index at one slot of the author-books table.
  bool authorBook(uint32_t slot, uint16_t& recordIndex);
  bool blobString(uint32_t offset, std::string& out);
  // An author's name as the books spell it, and its filing name ("Pratchett, Terry") when `filing` is
  // given.
  bool authorName(const library::AuthorRecord& author, std::string& name, std::string* filing = nullptr);

 private:
  bool readAt(uint32_t offset, void* out, size_t len);

  HalFile file;
  library::Header hdr{};
  bool attempted = false;  // an open was tried on `file`, so close() is legal on it
  bool opened = false;
};
