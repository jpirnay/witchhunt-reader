#include "UrlTarget.h"

namespace crosspoint {

namespace {

bool equalsIgnoreCaseAscii(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// What a DNS name or an IPv4 address is written with. Everything else is refused rather than read: a byte
// another parser takes for a delimiter ('\\', which browsers read as '/', '%', '[') could otherwise make
// two readings of one host.
bool isHostByte(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

}  // namespace

bool splitUrl(const std::string_view url, UrlTarget& out) {
  out = UrlTarget{};
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return false;
  const std::string_view scheme = url.substr(0, schemeEnd);
  const bool https = equalsIgnoreCaseAscii(scheme, "https");
  if (!https && !equalsIgnoreCaseAscii(scheme, "http")) return false;

  const size_t authorityStart = schemeEnd + 3;
  const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
  const std::string_view authority = authorityEnd == std::string_view::npos
                                         ? url.substr(authorityStart)
                                         : url.substr(authorityStart, authorityEnd - authorityStart);
  // A host has no ':', so the first one starts the port; there is no other reading to choose from.
  const size_t colon = authority.find(':');
  const std::string_view host = authority.substr(0, colon);
  if (host.empty()) return false;
  for (const char c : host) {
    if (!isHostByte(c)) return false;  // '@' among them: user info is refused here
  }

  uint32_t port = https ? 443 : 80;
  if (colon != std::string_view::npos) {
    const std::string_view digits = authority.substr(colon + 1);
    if (digits.empty() || digits.size() > 5) return false;
    port = 0;
    for (const char c : digits) {
      if (c < '0' || c > '9') return false;  // a second ':', an '@' in user info, anything else
      port = port * 10 + static_cast<uint32_t>(c - '0');
    }
    if (port == 0 || port > 65535) return false;
  }

  out.scheme = scheme;
  out.host = host;
  out.port = static_cast<uint16_t>(port);
  out.rest = authorityEnd == std::string_view::npos ? std::string_view() : url.substr(authorityEnd);
  return true;
}

}  // namespace crosspoint
