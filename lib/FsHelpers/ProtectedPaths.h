#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Which card paths the web server and WebDAV refuse to touch.
//
// The web server used to test only the LAST component of a requested path, on the
// theory that a protected item is always the thing being named. It is not: a request
// for "/.crosspoint/wifi.json" names "wifi.json", which passed, and that file holds
// every saved WiFi password, obfuscated with a key any LAN peer can derive from the
// device's MAC address. opds.json and koreader.json were exposed the same way, and the
// plugin write endpoints could overwrite anything under /.crosspoint. The server has
// no authentication, so that was every client on the network while File Transfer ran.
//
// The rules, applied to every segment:
//  - The credential stores are never reachable, whatever the settings.
//  - The system folders the file manager never lists are never reachable.
//  - Dot-prefixed names are refused everywhere unless the user has turned on hidden
//    files; then a dot FOLDER may be walked through (to clear caches under
//    /.crosspoint, say), but an item that is itself dot-named still cannot be read,
//    written, renamed, moved or deleted. That last rule is what stops /.crosspoint
//    itself being renamed or moved, which would carry the credential stores out from
//    under the denylist.
//
// Two spellings reach an entry without naming it, and both are closed here:
//  - SdFat trims a name before opening it: leading spaces, trailing dots and spaces
//    (FatFile::parsePathName). " .crosspoint" and ".crosspoint." open ".crosspoint",
//    so every segment is checked as SdFat will see it.
//  - On FAT, a name that fails to match an entry's long name is compared with the
//    entry's generated 8.3 alias (the short-entry compare in FatFile::open), so
//    "CROSSP~1" opens ".crosspoint". No spelling rule maps an alias back to its long
//    name, so the caller resolves alias-shaped segments on the card and a request
//    that spells an entry by its alias is refused outright: nothing legitimate does.
//
// Upstream closed the same exposure for the three credential files (crosspoint-reader
// PR #3824, Justin Mitchell / @itsthisjustin). The SdFat trimming and 8.3-alias
// observations are theirs, as is the credential denylist; the per-segment rule for
// every protected folder, the hidden-files opt-in and the on-card alias resolution are
// ours, and WebDAV now shares the same checks.
namespace ProtectedPaths {

inline constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};

// The stores whose secrets are obfuscated with a device key: those files must never
// leave the device. Prefix-matched, so a sibling written next to one (a temp or backup
// copy) is covered too. Mirrors WifiCredentialStore, OpdsServerStore and
// KOReaderCredentialStore.
inline constexpr const char* CREDENTIAL_FILES[] = {"/.crosspoint/wifi.json", "/.crosspoint/opds.json",
                                                   "/.crosspoint/koreader.json"};

// What the request does with the path, which decides how its LAST segment is judged.
enum class Target : uint8_t {
  Directory,  // listed, or written into: every segment is a folder walked through
  Item,       // read, written, renamed, moved or deleted: its own name must be ordinary
};

inline char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool equalsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (asciiLower(a[i]) != asciiLower(b[i])) return false;
  }
  return true;
}

// FAT names are case-insensitive, so the comparisons below are too.
inline bool startsWithIgnoreCase(const std::string_view s, const std::string_view prefix) {
  return s.size() >= prefix.size() && equalsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

// The name SdFat opens for a requested path segment.
inline std::string_view effectiveName(const std::string_view segment) {
  size_t first = 0;
  while (first < segment.size() && segment[first] == ' ') first++;
  size_t last = segment.size();
  while (last > first && (segment[last - 1] == '.' || segment[last - 1] == ' ')) last--;
  return segment.substr(first, last - first);
}

inline bool isDotName(const std::string_view name) { return !name.empty() && name.front() == '.'; }

inline bool isSystemName(const std::string_view name) {
  for (const char* hidden : HIDDEN_ITEMS) {
    if (equalsIgnoreCase(name, hidden)) return true;
  }
  return false;
}

// An item the file manager treats as protected by name alone: dot-prefixed, or one of
// the system folders. What the listing hides and what the item rule above refuses.
inline bool isProtectedName(const std::string_view name) { return isDotName(name) || isSystemName(name); }

// Never where books are kept: the firmware's own folder (its caches, settings and the credential
// stores) and the system folders. The book listings and the library index leave them out even with
// hidden files shown, at any depth -- a cache's copies and leftovers are not the reader's books.
inline bool holdsNoBooks(const std::string_view name) {
  return isSystemName(name) || equalsIgnoreCase(name, ".crosspoint");
}

// Shaped like a generated 8.3 alias: a '~' followed by a digit, in a name that fits
// 8.3. Only a filter for which segments are worth resolving on the card -- a real file
// may be named like this, and resolution is what tells the two apart.
inline bool looksLikeShortAlias(const std::string_view name) {
  const size_t tilde = name.find('~');
  if (tilde == std::string_view::npos || tilde + 1 >= name.size() || name[tilde + 1] < '0' || name[tilde + 1] > '9') {
    return false;
  }
  const size_t dot = name.find('.');
  if (dot != std::string_view::npos && name.find('.', dot + 1) != std::string_view::npos) return false;
  const size_t base = dot == std::string_view::npos ? name.size() : dot;
  const size_t ext = dot == std::string_view::npos ? 0 : name.size() - dot - 1;
  return base <= 8 && ext <= 3;
}

inline bool isCredentialPath(const std::string_view canonical) {
  for (const char* file : CREDENTIAL_FILES) {
    if (startsWithIgnoreCase(canonical, file)) return true;
  }
  return false;
}

// True when `path` must be refused. `path` should already have "." and ".." resolved
// (normalizeWebPath / FsHelpers::normalisePath); this adds what SdFat does on top.
//
// `spelledByAlias(prefix, name)` is asked about every alias-shaped segment, with
// `prefix` the requested path up to and including that segment. It answers whether an
// entry exists there whose real name is NOT `name` -- i.e. whether the request reached
// it through its 8.3 alias. Host tests pass a fake; the firmware asks the card.
template <typename SpelledByAlias>
bool isProtectedPath(const std::string_view path, const Target target, const bool allowHidden,
                     SpelledByAlias&& spelledByAlias) {
  std::string canonical;
  canonical.reserve(path.size() + 1);
  std::string_view lastName;
  size_t pos = 0;
  while (pos < path.size()) {
    if (path[pos] == '/') {
      pos++;
      continue;
    }
    size_t end = path.find('/', pos);
    if (end == std::string_view::npos) end = path.size();
    const std::string_view name = effectiveName(path.substr(pos, end - pos));
    pos = end + 1;
    // A segment that trims to nothing is not a name to SdFat: a trailing one is
    // skipped like a separator ("/.crosspoint/ " opens "/.crosspoint"), any other
    // fails the open. Either way it names nothing, so the item rule below must
    // judge the last segment that DOES name something.
    if (name.empty()) continue;

    if (isSystemName(name)) return true;
    if (isDotName(name) && !allowHidden) return true;
    if (looksLikeShortAlias(name) && spelledByAlias(path.substr(0, end), name)) return true;

    canonical += '/';
    canonical.append(name.data(), name.size());
    lastName = name;
  }
  if (target == Target::Item && isDotName(lastName)) return true;
  return isCredentialPath(canonical);
}

}  // namespace ProtectedPaths
