#pragma once

#include <cstdint>
#include <string_view>

// Where an http(s) URL sends a request: the one reading of a URL's scheme, host and port, shared by
// SecureHttpClient, which connects there, and by the allowlists that judge a URL before the client
// gets it (src/network/PluginHosts.h). Judge and client read the URL with the same code, so the host
// that is judged is the host that is connected to.
//
// They used to read it apart. The plugin allowlist dropped everything up to the last '@' and judged
// the rest, while the client kept the user info and split the host from the port at the last ':'.
// For "https://evil.com:443@good.com/x" the allowlist judged good.com and the client connected to
// evil.com:443.

namespace crosspoint {

struct UrlTarget {
  std::string_view scheme;  // "http" or "https", in the URL's own case
  std::string_view host;    // as written; compare it without case
  uint16_t port = 0;        // the URL's, or the scheme's default
  std::string_view rest;    // from the first '/', '?' or '#' after the host on; empty when there is none
};

// False for a URL no request may go out for: one without "://", with a scheme other than http or
// https, or whose authority (from "://" to the first '/', '?' or '#') holds user info (an '@'), an
// empty host, a byte other than a letter, a digit, '-', '.' or '_' in the host (an IPv6 literal
// among them), or a port that is not 1 to 65535. A URL with user info is refused, not stripped, so
// no two readings of it can differ; the client never reached the server such a URL names anyway (it
// took "user:pw@host" for host "user" on port 0). `out` points into `url`.
bool splitUrl(std::string_view url, UrlTarget& out);

}  // namespace crosspoint
