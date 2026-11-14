#include "TlsPolicy.h"

namespace crosspoint {

TlsPlan planTls(const TlsPolicy policy, const bool skipValidation, const bool sendsCredentials) {
  switch (policy) {
    case TlsPolicy::Strict:
      return {true, false};
    case TlsPolicy::Unverified:
      return {false, false};
    case TlsPolicy::Verified:
      return {!skipValidation, false};
    case TlsPolicy::VerifiedThenUnverified:
      // The user's skip setting reaches every non-firmware request alike.
      if (skipValidation) return {false, false};
      // A password never goes over an unverified connection on the policy's say-so alone.
      if (sendsCredentials) return {true, false};
      return {true, true};
  }
  return {true, false};
}

TlsPlan planHop(const TlsPlan& request, const bool hopSendsCredentials, const bool peerUnverifiable) {
  if (!request.fallbackOnVerifyError) return request;
  if (hopSendsCredentials) return {request.verify, false};
  // Known to fail this session: straight to the connection the fallback would make.
  if (peerUnverifiable) return {false, false};
  return request;
}

std::string tlsPeer(const std::string_view host, const uint16_t port) {
  std::string peer{host};
  peer += ':';
  peer += std::to_string(port);
  return peer;
}

std::string urlPeer(std::string_view url) {
  uint16_t port = 443;
  const size_t scheme = url.find("://");
  if (scheme != std::string_view::npos) {
    if (url.substr(0, scheme) == "http") port = 80;
    url.remove_prefix(scheme + 3);
  }
  std::string_view authority = url.substr(0, url.find_first_of("/?#"));
  const size_t at = authority.rfind('@');
  if (at != std::string_view::npos) authority.remove_prefix(at + 1);
  std::string_view host = authority;
  std::string_view portText;
  if (!authority.empty() && authority.front() == '[') {
    // An IPv6 literal: its colons are not the port's.
    const size_t close = authority.find(']');
    host = authority.substr(0, close == std::string_view::npos ? authority.size() : close + 1);
    if (close != std::string_view::npos && close + 1 < authority.size() && authority[close + 1] == ':') {
      portText = authority.substr(close + 2);
    }
  } else if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
    host = authority.substr(0, colon);
    portText = authority.substr(colon + 1);
  }
  if (!portText.empty() && portText.size() <= 5) {
    uint32_t value = 0;
    bool digits = true;
    for (const char c : portText) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (digits && value > 0 && value <= 65535) port = static_cast<uint16_t>(value);
  }
  return tlsPeer(host, port);
}

uint32_t UnverifiableHosts::hashOf(const std::string_view host) {
  uint32_t hash = 2166136261u;
  for (const char c : host) {
    hash ^= static_cast<uint8_t>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    hash *= 16777619u;
  }
  return hash;
}

bool UnverifiableHosts::contains(const std::string_view host) const {
  if (host.empty()) return false;
  const uint32_t hash = hashOf(host);
  for (uint8_t i = 0; i < count_; i++) {
    if (hashes_[i] == hash) return true;
  }
  return false;
}

void UnverifiableHosts::add(const std::string_view host) {
  if (host.empty() || contains(host)) return;
  if (count_ < kCapacity) {
    hashes_[count_++] = hashOf(host);
    return;
  }
  hashes_[next_] = hashOf(host);
  next_ = static_cast<uint8_t>((next_ + 1) % kCapacity);
}

}  // namespace crosspoint
