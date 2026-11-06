#pragma once

// The slice of the framework's WebServer that MultipartRoute.h touches, so its logic runs on the host.
// The handler interface follows RequestHandler.h from framework-arduinoespressif32 3.3.11.

#include <string>

using String = std::string;

enum HTTPMethod { HTTP_GET, HTTP_POST };
struct HTTPUpload {};
struct HTTPRaw {};

class WebServer {
 public:
  void send(const int code, const char*, const char* body) {
    sentCode = code;
    sentBody = body;
  }

  int sentCode = 0;
  std::string sentBody;
};

class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  virtual bool canHandle(WebServer&, HTTPMethod, const String&) { return false; }
  virtual bool canUpload(WebServer&, const String&) { return false; }
  virtual bool canRaw(WebServer&, const String&) { return false; }
  virtual bool handle(WebServer&, HTTPMethod, const String&) { return false; }
  virtual void upload(WebServer&, const String&, HTTPUpload&) {}
  virtual void raw(WebServer&, const String&, HTTPRaw&) {}
};
