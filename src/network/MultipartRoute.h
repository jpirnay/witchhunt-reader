#pragma once

#include <WebServer.h>

#include <functional>
#include <utility>

// A POST route that takes a file as multipart/form-data, and nothing else.
//
// WebServer::on(path, method, fn, ufn) calls ufn for both kinds of body: the parts of a form through
// upload() and any other body through raw(), with no way to tell them apart. A function that reads
// server.upload() then dereferences null on a body of the wrong kind, so one POST with another
// Content-Type crashed the reader. Here a body that is not a form is read and dropped: dropped rather
// than refused by canRaw(), which would send it down the framework's plain path and hold all of it on
// the heap. A form with no file in it gets the same 415, so `finished` only ever runs for a file that
// arrived in this request, never on what an earlier upload left behind.
//
// Multipart is the one body kind that can carry a file and its target in the query string: the
// framework parses the query ahead of a form, and never for a raw body.
//
// The crash was found by Arthur Tazhitdinov in crosspoint-reader #4285, whose route table refuses the
// same bodies through canRaw(); reworked here as one handler per upload route.
class MultipartRoute final : public RequestHandler {
 public:
  using Action = std::function<void()>;

  // `path` must outlive the route: a string literal. `part` runs for each piece of a file, with
  // server.upload() valid; `finished` runs once the whole request is in, to send the reply.
  MultipartRoute(const char* path, Action finished, Action part)
      : path(path), finished(std::move(finished)), part(std::move(part)) {}

  bool canHandle(WebServer&, const HTTPMethod method, const String& uri) override {
    if (method != HTTP_POST || uri != path) return false;
    sawFile = false;  // a request cut off mid-form never reached handle()
    return true;
  }

  // Asked only of the handler canHandle() chose, so only of a request for this route.
  bool canUpload(WebServer&, const String&) override { return true; }
  bool canRaw(WebServer&, const String&) override { return true; }

  void upload(WebServer&, const String&, HTTPUpload&) override {
    sawFile = true;
    part();
  }

  void raw(WebServer&, const String&, HTTPRaw&) override {}

  bool handle(WebServer& server, HTTPMethod, const String&) override {
    if (sawFile) {
      finished();
    } else {
      server.send(415, "application/json", "{\"error\":\"send the file as multipart/form-data\"}");
    }
    return true;
  }

 private:
  const char* path;
  Action finished;
  Action part;
  bool sawFile = false;
};
