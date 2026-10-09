#pragma once

#include <cstdint>
#include <string>

// How the book index tells books and authors apart and orders authors. Pure functions.
namespace LibraryKeys {

// Text folded for comparison: ASCII lowercased, Latin-1 and Latin Extended-A letters reduced to their
// base letter ("Ö" -> "o", "ß" -> "ss", "Ł" -> "l"), combining marks dropped (so a decomposed name
// from macOS folds like its composed form), whitespace runs collapsed to one space and trimmed.
// Other characters pass through unchanged.
std::string fold(const std::string& text);

// The key an author files under, surname first: the book's file-as when it has one; a name that
// already holds a comma as written; otherwise the last word moved to the front ("Terry Pratchett" ->
// "pratchett terry"). Folded, with commas and full stops dropped, so "Le Guin, Ursula K." and a
// file-as of "Le Guin Ursula K" are one key.
std::string authorSortKey(const std::string& name, const std::string& fileAs);

// One author across books: FNV-1a of the folded name, so the same name in another case, accent form
// or spacing still groups. An empty name is library::AUTHOR_UNKNOWN; no name hashes to that or to
// library::AUTHOR_PENDING.
uint32_t authorHash(const std::string& name);

// One book across folders: FNV-1a of the filename, ASCII-lowercased, then its size. A book moved to
// another folder, or renamed only in case, stays the same book; so are two copies of it.
uint32_t bookIdentity(const char* fileName, uint32_t size);

}  // namespace LibraryKeys
