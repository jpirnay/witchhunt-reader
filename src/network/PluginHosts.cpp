#include "PluginHosts.h"

#include <UrlTarget.h>

namespace PluginHosts {

namespace {

char lowerAscii(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

void appendLower(std::string& out, const std::string_view s) {
  for (const char c : s) out += lowerAscii(c);
}

bool equalsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
  }
  return true;
}

// Calls fn with each entry of a '\n'-terminated list until it answers true.
template <typename Fn>
bool anyEntry(const std::string& list, Fn&& fn) {
  size_t start = 0;
  for (size_t end = list.find('\n'); end != std::string::npos; end = list.find('\n', start)) {
    if (fn(std::string_view(list).substr(start, end - start))) return true;
    start = end + 1;
  }
  return false;
}

// A host as urlHost() gives one, short enough for the wanted list: lower-case letters, digits, '-', '.' and '_'.
bool isWantedHost(const std::string_view host) {
  if (host.empty() || host.size() > kMaxWantedHost) return false;
  for (const char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
    if (!ok) return false;
  }
  return true;
}

std::string_view wantedHostOf(const std::string_view entry) {
  return entry.substr(!entry.empty() && entry[0] == '-' ? 1 : 0);
}

// The well-formed entries of a wanted list, oldest first, the newest kMaxWanted of them. Anything else in the file
// (it sits in the plugin's own folder, which the plugin may write) is left out.
size_t wantedEntries(const std::string_view list, std::string_view (&out)[kMaxWanted]) {
  size_t count = 0;
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find('\n', start);
    if (end == std::string_view::npos) end = list.size();
    const std::string_view entry = list.substr(start, end - start);
    start = end + 1;
    if (!isWantedHost(wantedHostOf(entry))) continue;
    if (count == kMaxWanted) {
      for (size_t i = 1; i < kMaxWanted; i++) out[i - 1] = out[i];
      count--;
    }
    out[count++] = entry;
  }
  return count;
}

// The entries, each followed by '\n', the first `skip` left out.
std::string joinedEntries(const std::string_view* entries, const size_t count, const size_t skip) {
  std::string out;
  for (size_t i = skip; i < count; i++) {
    out.append(entries[i].data(), entries[i].size());
    out += '\n';
  }
  return out;
}

}  // namespace

std::string urlHost(const std::string_view url) {
  crosspoint::UrlTarget target;
  if (!crosspoint::splitUrl(url, target)) return {};
  std::string host;
  host.reserve(target.host.size());
  appendLower(host, target.host);
  return host;
}

bool hostMatches(const std::string_view host, const std::string_view pattern) {
  if (pattern.empty() || host.empty()) return false;
  if (pattern[0] == '.') {
    return host.size() > pattern.size() && equalsIgnoreCase(host.substr(host.size() - pattern.size()), pattern);
  }
  return equalsIgnoreCase(host, pattern);
}

void Reach::allow(const std::string_view pattern) {
  if (pattern.empty() || pattern.find('\n') != std::string_view::npos) return;
  appendLower(patterns_, pattern);
  patterns_ += '\n';
}

void Reach::approve(const std::string_view host) {
  if (host.empty() || host.find('\n') != std::string_view::npos) return;
  appendLower(approved_, host);
  approved_ += '\n';
}

void Reach::declare(const std::string_view host) {
  if (host.empty() || host.find('\n') != std::string_view::npos) return;
  appendLower(declared_, host);
  declared_ += '\n';
}

std::string wantedWith(const std::string_view list, const std::string_view host) {
  if (!isWantedHost(host)) return std::string(list);
  std::string_view entries[kMaxWanted];
  const size_t count = wantedEntries(list, entries);
  for (size_t i = 0; i < count; i++) {
    if (wantedHostOf(entries[i]) == host) return std::string(list);
  }
  std::string out = joinedEntries(entries, count, count == kMaxWanted ? 1 : 0);
  out.append(host.data(), host.size());
  out += '\n';
  return out;
}

std::string wantedWithout(const std::string_view list, const std::string_view host, const bool dismiss) {
  std::string_view entries[kMaxWanted];
  size_t count = wantedEntries(list, entries);
  size_t kept = 0;
  for (size_t i = 0; i < count; i++) {
    if (wantedHostOf(entries[i]) != host) entries[kept++] = entries[i];
  }
  count = kept;
  if (!dismiss || !isWantedHost(host)) return joinedEntries(entries, count, 0);
  std::string out = joinedEntries(entries, count, count == kMaxWanted ? 1 : 0);
  out += '-';
  out.append(host.data(), host.size());
  out += '\n';
  return out;
}

size_t wantedHosts(const std::string_view list, std::string_view (&out)[kMaxWanted]) {
  std::string_view entries[kMaxWanted];
  const size_t count = wantedEntries(list, entries);
  size_t asked = 0;
  for (size_t i = 0; i < count; i++) {
    if (entries[i][0] != '-') out[asked++] = entries[i];
  }
  return asked;
}

bool Reach::allowsHost(const std::string_view host) const {
  if (host.empty()) return false;
  const auto exact = [host](const std::string_view entry) { return equalsIgnoreCase(host, entry); };
  return anyEntry(patterns_, [host](const std::string_view p) { return hostMatches(host, p); }) ||
         anyEntry(approved_, exact) || anyEntry(declared_, exact);
}

}  // namespace PluginHosts
