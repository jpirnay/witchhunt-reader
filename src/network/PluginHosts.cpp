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

bool Reach::allowsHost(const std::string_view host) const {
  if (host.empty()) return false;
  const auto exact = [host](const std::string_view entry) { return equalsIgnoreCase(host, entry); };
  return anyEntry(patterns_, [host](const std::string_view p) { return hostMatches(host, p); }) ||
         anyEntry(approved_, exact) || anyEntry(declared_, exact);
}

}  // namespace PluginHosts
