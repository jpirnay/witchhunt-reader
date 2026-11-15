#pragma once

// The slice of the framework's WebServer that WebDAVHandler.cpp and ChunkedResponse.h touch, so the handler runs on
// the host. One request at a time: set it up, hand the server to the handler, read the answer back. The types follow
// WebServer.h and RequestHandler.h from framework-arduinoespressif32 3.3.x.

#include <NetworkClient.h>
#include <WString.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

inline void yield() {}

enum HTTPMethod {
  HTTP_ANY,
  HTTP_GET,
  HTTP_HEAD,
  HTTP_POST,
  HTTP_PUT,
  HTTP_DELETE,
  HTTP_OPTIONS,
  HTTP_PROPFIND,
  HTTP_MKCOL,
  HTTP_MOVE,
  HTTP_COPY,
  HTTP_LOCK,
  HTTP_UNLOCK,
};

enum HTTPRawStatus { RAW_START, RAW_WRITE, RAW_END, RAW_ABORTED };

struct HTTPRaw {
  HTTPRawStatus status = RAW_START;
  size_t totalSize = 0;
  size_t currentSize = 0;
  uint8_t buf[1436] = {};
};

constexpr size_t CONTENT_LENGTH_UNKNOWN = static_cast<size_t>(-1);

class WebServer {
 public:
  // The request.
  void setRequest(const HTTPMethod method, const char* uri) {
    method_ = method;
    uri_ = uri;
    headers_.clear();
    code = 0;
    body.clear();
  }
  void setHeader(const char* name, const char* value) { headers_[name] = value; }

  HTTPMethod method() const { return method_; }
  String uri() const { return String(uri_.c_str()); }
  String header(const char* name) const {
    const auto it = headers_.find(name);
    return it == headers_.end() ? String() : String(it->second.c_str());
  }
  size_t clientContentLength() const { return 0; }
  NetworkClient& client() { return client_; }

  // As the framework's: %XX is a byte, '+' a space.
  static String urlDecode(const String& text) {
    std::string out;
    const char* s = text.c_str();
    for (size_t i = 0; s[i] != '\0'; i++) {
      if (s[i] == '%' && s[i + 1] != '\0' && s[i + 2] != '\0') {
        const char hex[3] = {s[i + 1], s[i + 2], '\0'};
        out += static_cast<char>(std::stoi(hex, nullptr, 16));
        i += 2;
      } else {
        out += s[i] == '+' ? ' ' : s[i];
      }
    }
    return String(out);
  }

  // The answer.
  void send(const int status, const char* = nullptr, const String& content = String()) {
    code = status;
    body += content.c_str();
  }
  void sendHeader(const char*, const char*) {}
  void setContentLength(size_t) {}
  void sendContent(const char* data, const size_t len) { body.append(data, len); }
  void sendContent(const char* data) { body += data; }

  int code = 0;
  std::string body;

 private:
  HTTPMethod method_ = HTTP_GET;
  std::string uri_;
  std::map<std::string, std::string> headers_;
  NetworkClient client_;
};

class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  virtual bool canHandle(WebServer&, HTTPMethod, const String&) { return false; }
  virtual bool canRaw(WebServer&, const String&) { return false; }
  virtual void raw(WebServer&, const String&, HTTPRaw&) {}
  virtual bool handle(WebServer&, HTTPMethod, const String&) { return false; }
};
