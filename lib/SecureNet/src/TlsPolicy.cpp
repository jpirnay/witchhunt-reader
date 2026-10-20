#include "TlsPolicy.h"

namespace crosspoint {

TlsPlan planTls(const TlsPolicy policy, const bool skipValidation, const bool sendsCredentials,
                const bool hostUnverifiable) {
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
      // Known to fail this session: straight to the connection the fallback would make.
      if (hostUnverifiable) return {false, false};
      return {true, true};
  }
  return {true, false};
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
