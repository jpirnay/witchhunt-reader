#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Sidecar files: companions sitting beside a book, sharing its name with a
// different extension. The firmware prefers them over the equivalent data
// embedded in the book, so "Some Book.jpg" overrides its cover and
// "Some Book.opf" overrides its metadata (see docs/sidecar-files.md).
//
// This is the single definition of what counts as a sidecar. It used to be
// spread across three places - the cover resolver, the metadata resolver and
// the move-to-/COMPLETED extension list - which is precisely how .opf came to
// be readable by the reader but left behind when a finished book moved.
//
// ADDING A NEW KIND OF SIDECAR MEANS ADDING IT HERE AND NOWHERE ELSE.
namespace SidecarFiles {

// Cover images, in resolution order: the first one that exists wins.
inline constexpr const char* kCoverExtensions[] = {".jpg", ".jpeg", ".png", ".bmp", ".JPG", ".JPEG", ".PNG", ".BMP"};
// Calibre-style metadata OPF.
inline constexpr const char* kMetadataExtensions[] = {".opf", ".OPF"};
// A protected EPUB's content key, wrapped to the device (lib/BookProtection/BookKey.h). Its name is
// the book's whole name and ".key" ("Some Book.epub.key"), the same as an extension of the base.
inline constexpr const char* kKeyExtensions[] = {".epub.key", ".EPUB.key"};
// The licence a plugin wrote beside a protected EPUB it fetched (the Plugin Hub's protected-content:
// "Some Book.epub.rights", Adobe's rights.xml), which it derives the key from again after a flash
// erase. Upstream moves it with the book too (EpubReaderActivity's SIDECARS).
inline constexpr const char* kLicenseExtensions[] = {".epub.rights", ".EPUB.rights"};
// metadataStamp() reads no further than this. It must cover every byte a reader
// of the sidecar takes in, or an edit past the cap would go unnoticed.
inline constexpr size_t kMetadataStampBytes = 16384;

// "/Books/Some Book.epub" -> "/Books/Some Book". Empty when the path carries no
// extension of its own - a bare name, or one whose only dot belongs to a parent
// directory ("/My.Books/untitled"), which must not be mistaken for one.
std::string basePath(const std::string& bookPath);

// Full path of the first existing sidecar of that kind, or "" when there is
// none. Both hit the filesystem once per candidate extension.
std::string coverPath(const std::string& bookPath);
std::string metadataPath(const std::string& bookPath);

// Fingerprint of the metadata sidecar's contents, for callers that keep a copy of
// what it produced and must notice when it changes. The recent-books list stores
// title and author when a book is opened, so a sidecar written afterwards never
// reached the home screen. 0 = no sidecar; an existing one, even an empty one,
// is never 0. A hash of the contents rather than the modification time: a device
// without a set clock stamps every write 1980-01-01, and an edit that keeps the
// length would keep the size too.
uint32_t metadataStamp(const std::string& bookPath);

// Extensions of every sidecar that actually exists beside this book, covers and
// metadata alike. For callers that must treat them as a set rather than resolve
// one - moving a book has to carry all of them, or it silently strands the
// cover and the metadata corrections behind.
std::vector<const char*> existingExtensions(const std::string& bookPath);

// True when moving the book to targetBookPath would land one of its sidecars on a
// file that is already there. Asked before the book moves, so a book is never
// moved without its sidecars, and no file at the destination is overwritten.
bool anyTargetTaken(const std::string& bookPath, const std::string& targetBookPath);

// Rename every sidecar of bookPath to sit beside targetBookPath, keeping its
// extension. Call it for the move a reader asked for, where anyTargetTaken() has
// already ruled out a clash; false when any one of them failed to move.
bool moveAll(const std::string& bookPath, const std::string& targetBookPath);

// Rename a protected book's key and licence (kKeyExtensions, kLicenseExtensions) to sit beside
// targetBookPath, for a move that did not take the sidecars along: the web File Manager, WebDAV and
// the serial transfer move the book alone, and without its key it no longer opens
// (BookRelocation::bookMoved() calls this for every move). One already at the target belonged to a
// book no longer at that path and is replaced (FileReplace, which never leaves neither copy). One
// not at the old path (moveAll() took it, or a moved folder held it) is no failure. Covers and
// metadata stay where they are. False when any one of them failed to move.
bool moveProtection(const std::string& bookPath, const std::string& targetBookPath);

// Delete every sidecar of bookPath. For a book being deleted from a view that
// hides its sidecars: left behind, they would be invisible there and linger on
// the card for good. False when any one of them failed.
bool removeAll(const std::string& bookPath);

}  // namespace SidecarFiles
