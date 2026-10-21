#pragma once
// Host stub: a scripted server behind a fake TCP connection.
//
// Ported from Free-Ink/freeink-sdk f80a99c (libs/network/SecureNet/test/host/stubs/WiFiClient.h,
// Justin Mitchell). Changed here:
// - the connection is kept alive across requests: each complete request takes the next reply, as
//   SecureHttpClient's keep-alive and the chunked download need;
// - besides fully scripted replies, the server can serve one resource and answer Range requests
//   itself (206 with Content-Range, 416 past the end, 200 when told to ignore Range);
// - a reply can be cut short and end in a peer close or an out-of-memory read, which the stub
//   SecureClient in the test reports as wolfSSL's MEMORY_E;
// - a server's certificate can fail to verify, for the stub SecureClient's handshake.
#include <Client.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

struct FakeReply {
  enum class End {
    KeepOpen,     // the connection stays up for the next request
    Close,        // the peer closes after these bytes (a short body is a mid-body drop)
    OutOfMemory,  // after these bytes, the next read cannot allocate its TLS record
  };
  std::string bytes;
  End end = End::KeepOpen;
};

// A fault for one request of the served resource, by request index (0-based, counted across
// connections): keep the headers, send only bodyBytes of the body, then end as given.
struct FakeFault {
  size_t bodyBytes = 0;
  FakeReply::End end = FakeReply::End::Close;
};

struct FakeNet {
  static std::deque<FakeReply>& replies() {  // scripted replies, used before the resource
    static std::deque<FakeReply> r;
    return r;
  }
  static std::vector<std::string>& requests() {
    static std::vector<std::string> r;
    return r;
  }
  static int& connects() {
    static int n = 0;
    return n;
  }
  static std::string& resource() {
    static std::string s;
    return s;
  }
  static bool& serving() {  // answer requests from resource()
    static bool b = false;
    return b;
  }
  static bool& totalUnknown() {  // Content-Range ".../*"
    static bool b = false;
    return b;
  }
  static size_t& ignoreRangeFrom() {  // from this request index on, answer 200 with the whole resource
    static size_t i = static_cast<size_t>(-1);
    return i;
  }
  static std::map<size_t, FakeFault>& faults() {
    static std::map<size_t, FakeFault> f;
    return f;
  }
  // For the stub SecureClient: the servers (host:port) whose certificate does not verify, and each
  // https handshake in order, as "host:port verified", "... fallback", "... unverified" or
  // "... refused".
  static std::set<std::string>& unverifiable() {
    static std::set<std::string> s;
    return s;
  }
  static std::vector<std::string>& handshakes() {
    static std::vector<std::string> h;
    return h;
  }
  static void reset() {
    unverifiable().clear();
    handshakes().clear();
    replies().clear();
    requests().clear();
    connects() = 0;
    resource().clear();
    serving() = false;
    totalUnknown() = false;
    ignoreRangeFrom() = static_cast<size_t>(-1);
    faults().clear();
  }

  // The reply the served resource gives to a request.
  static FakeReply serve(const std::string& request, size_t index) {
    const std::string& r = resource();
    std::string head;
    std::string body;
    const size_t rangeAt = request.find("Range: bytes=");
    if (rangeAt == std::string::npos || index >= ignoreRangeFrom()) {
      head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(r.size()) + "\r\n\r\n";
      body = r;
    } else {
      const char* p = request.c_str() + rangeAt + 13;
      char* end = nullptr;
      const size_t first = std::strtoul(p, &end, 10);
      size_t last = std::strtoul(end + 1, nullptr, 10);
      const std::string total = totalUnknown() ? "*" : std::to_string(r.size());
      if (first >= r.size()) {
        head = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + std::to_string(r.size()) +
               "\r\nContent-Length: 0\r\n\r\n";
      } else {
        last = std::min(last, r.size() - 1);
        body = r.substr(first, last - first + 1);
        head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(first) + "-" +
               std::to_string(last) + "/" + total + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
      }
    }
    FakeReply reply{head + body, FakeReply::End::KeepOpen};
    const auto fault = faults().find(index);
    if (fault != faults().end()) {
      reply.bytes = head + body.substr(0, std::min(body.size(), fault->second.bodyBytes));
      reply.end = fault->second.end;
    }
    return reply;
  }
};

// Scripted replies the redirect tests share: a redirect to location, and a short 200.
inline FakeReply fakeRedirectTo(const std::string& location, int status = 302) {
  return {"HTTP/1.1 " + std::to_string(status) + " Moved\r\nLocation: " + location + "\r\nContent-Length: 0\r\n\r\n",
          FakeReply::End::KeepOpen};
}
inline FakeReply fakeOk() { return {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", FakeReply::End::KeepOpen}; }

class WiFiClient : public Client {
 public:
  int connect(IPAddress, uint16_t) override { return 0; }
  int connect(const char*, uint16_t) override {
    ++FakeNet::connects();
    open_ = true;
    reply_ = FakeReply{};
    pos_ = 0;
    request_.clear();
    return 1;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t size) override {
    if (!open_ || closedByPeer()) return 0;
    request_.append(reinterpret_cast<const char*>(buf), size);
    if (request_.find("\r\n\r\n") != std::string::npos) {
      const size_t index = FakeNet::requests().size();
      FakeNet::requests().push_back(request_);
      if (!FakeNet::replies().empty()) {
        reply_ = FakeNet::replies().front();
        FakeNet::replies().pop_front();
      } else if (FakeNet::serving()) {
        reply_ = FakeNet::serve(request_, index);
      } else {
        reply_ = FakeReply{"", FakeReply::End::Close};  // the server has nothing more to say
      }
      request_.clear();
      pos_ = 0;
    }
    return size;
  }
  int available() override { return open_ ? static_cast<int>(reply_.bytes.size() - pos_) : 0; }
  int read() override { return available() > 0 ? static_cast<uint8_t>(reply_.bytes[pos_++]) : -1; }
  int read(uint8_t* buf, size_t size) override {
    const size_t n = std::min(size, static_cast<size_t>(available()));
    if (n) memcpy(buf, reply_.bytes.data() + pos_, n);
    pos_ += n;
    return n ? static_cast<int>(n) : -1;
  }
  int peek() override { return available() > 0 ? static_cast<uint8_t>(reply_.bytes[pos_]) : -1; }
  void flush() override {}
  void stop() override { open_ = false; }
  uint8_t connected() override { return open_ && !closedByPeer(); }
  operator bool() override { return connected(); }

  // For the stub SecureClient: the reply is spent and its next read runs out of memory.
  bool outOfMemoryNow() const {
    return open_ && pos_ >= reply_.bytes.size() && reply_.end == FakeReply::End::OutOfMemory;
  }

 private:
  bool closedByPeer() const { return pos_ >= reply_.bytes.size() && reply_.end == FakeReply::End::Close; }

  FakeReply reply_;
  size_t pos_ = 0;
  bool open_ = false;
  std::string request_;
};
