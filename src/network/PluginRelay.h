#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

// The rules of the plugin relay's second form (POST /api/relay, CrossPointWebServer::handleRelayPost): which methods
// and request headers a plugin may send, and the response headers it gets back. Upstream crosspoint-reader's plugin
// API (#3114, Justin Mitchell, and later PRs) defines the wire contract: a plugin calls api.relay(method, url,
// headers, body) and receives {status, headers: [[name, value], ...], body}. Unlike upstream, the host must still be
// one the plugin may reach (its manifest's allowedHosts, or one the user approved), and a request carrying headers or a
// body, or one that is not a GET or HEAD, is a credentialed one, so TLS is verified.
//
// Pure, so the host tests check the rules without a network.
namespace PluginRelay {

// The methods a plugin may send: HTTP's own and the WebDAV ones self-hosted services use. Anything else, CONNECT
// and TRACE among them, is refused.
inline bool methodAllowed(const std::string& method) {
  static constexpr const char* kMethods[] = {"GET",      "HEAD",  "POST", "PUT",  "PATCH", "DELETE", "OPTIONS",
                                             "PROPFIND", "MKCOL", "MOVE", "COPY", "LOCK",  "UNLOCK"};
  for (const char* m : kMethods) {
    if (method == m) return true;
  }
  return false;
}

// A request header a plugin may set. The client writes the framing and connection headers itself, and a second
// Host or Content-Length would let a plugin's request say something other than what is sent. The name must be an
// RFC 9110 token and the value free of control bytes, so neither can end the header line and start another.
inline bool requestHeaderAllowed(const std::string& name, const std::string& value) {
  if (name.empty() || name.size() > 64 || value.size() > 4096) return false;
  for (const char c : name) {
    const bool token = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos;
    if (!token) return false;
  }
  for (const char c : value) {
    const auto u = static_cast<unsigned char>(c);
    if ((u < 0x20 && u != '\t') || u == 0x7F) return false;
  }
  std::string lower(name);
  for (char& c : lower) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  static constexpr const char* kReserved[] = {"host",       "content-length",  "connection", "transfer-encoding",
                                              "keep-alive", "upgrade",         "te",         "trailer",
                                              "expect",     "proxy-connection"};
  for (const char* r : kReserved) {
    if (lower == r) return false;
  }
  return true;
}

// Appends s to out as a JSON string, quotes included. Control bytes are escaped, so the result can stand in a
// response header without ending it.
inline void appendJsonString(std::string& out, const std::string& s) {
  out += '"';
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20 || u == 0x7F) {
      char esc[8];
      snprintf(esc, sizeof(esc), "\\u%04x", u);
      out += esc;
    } else {
      out += c;
    }
  }
  out += '"';
}

// The response headers as upstream's X-Relay-Headers carries them: a JSON list of [name, value] pairs in the order
// they came, duplicates kept (every Set-Cookie). Bounded, because the list rides in one header of our own response:
// a pair that would take the list past maxBytes is left out and dropped() counts it.
class HeaderList {
 public:
  explicit HeaderList(const size_t maxBytes) : maxBytes_(maxBytes) {}

  void add(const std::string& name, const std::string& value) {
    std::string pair = "[";
    appendJsonString(pair, name);
    pair += ',';
    appendJsonString(pair, value);
    pair += ']';
    // "[" + pairs joined by "," + "]"
    const size_t needed = json_.size() + (json_.empty() ? 1 : 0) + (count_ ? 1 : 0) + pair.size() + 1;
    if (needed > maxBytes_) {
      ++dropped_;
      return;
    }
    json_ += json_.empty() ? "[" : ",";
    json_ += pair;
    ++count_;
  }
  std::string json() const { return json_.empty() ? "[]" : json_ + "]"; }
  size_t dropped() const { return dropped_; }

 private:
  size_t maxBytes_;
  std::string json_;
  size_t count_ = 0;
  size_t dropped_ = 0;
};

// What POST /api/fetch says about a download that failed (CrossPointWebServer::fetchToSd), from the status of the
// server's answer: its own, or 0 or less when none came (a transport or TLS error). The answer's "status" carries the
// same number, as upstream's does, so a plugin can tell a missing file from a blocked one from a network failure. A
// 401 or 403 is marked apart: a catalog with a password grant then mints a new token and tries again.
struct FetchFailure {
  std::string error;
  bool unauthorized = false;
};

inline FetchFailure describeFetchFailure(const int status) {
  if (status == 401 || status == 403) return {"the server refused the request (" + std::to_string(status) + ")", true};
  // No answer, or a 2xx whose file could not be written: the status names nothing.
  if (status < 300) return {"download failed", false};
  return {"the server answered " + std::to_string(status), false};
}

}  // namespace PluginRelay
