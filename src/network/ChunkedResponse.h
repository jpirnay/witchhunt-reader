#pragma once

#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>
#include <esp_task_wdt.h>

#include <cstring>
#include <memory>

// Coalesces a chunked (CONTENT_LENGTH_UNKNOWN) response body into TCP-sized
// writes. Written naively, a streamed listing calls sendContent() once per
// entry, and every call is its own HTTP chunk — chunk header, TCP write, and
// the WebServer library's internal malloc for the chunk-size line. On a folder
// with a few hundred files that dominates the response time.
//
// Ported from CrossInk commit 1d6b82f ("perf: batch web file list responses",
// Julia Nguyen and Justin Mitchell), which back-ported crosspoint-reader
// 302c0771. Generalised here from the JSON file listing to every streamed list
// response, including WebDAV PROPFIND.
class ChunkedResponse {
 public:
  explicit ChunkedResponse(WebServer* server) : server(server) {
    // ~1 TCP segment. Heap rather than stack: handleClient() runs on the shared
    // Arduino loop task, below the whole activity call chain and alongside a 1KB
    // serialization buffer in the settings handler, so 1.4KB is not free there.
    // The allocation is fallible and a failure degrades to the old per-entry
    // sends rather than failing the request.
    buffer = makeUniqueNoThrow<char[]>(CAPACITY);
    if (!buffer) {
      LOG_ERR("WEB", "OOM: chunked response buffer; falling back to per-entry sends");
    }
  }

  void append(const char* data, size_t len) {
    if (!buffer) {
      server->sendContent(data, len);
      return;
    }
    if (used + len > CAPACITY) flush();
    if (len > CAPACITY) {
      // Longer than the whole buffer — send it on its own rather than truncating.
      server->sendContent(data, len);
      return;
    }
    memcpy(buffer.get() + used, data, len);
    used += len;
  }

  void append(const char* str) { append(str, strlen(str)); }

  // Flushes the tail and terminates the chunked response with the empty chunk.
  void finish() {
    flush();
    server->sendContent("");
  }

 private:
  static constexpr size_t CAPACITY = 1400;

  void flush() {
    if (used == 0) return;
    esp_task_wdt_reset();
    server->sendContent(buffer.get(), used);
    used = 0;
    esp_task_wdt_reset();
  }

  WebServer* server;
  std::unique_ptr<char[]> buffer;
  size_t used = 0;
};

// JSON-array framing on top of ChunkedResponse: owns the brackets and the
// separating commas so callers cannot emit a leading or doubled comma when an
// entry is skipped.
class ChunkedJsonArray {
 public:
  explicit ChunkedJsonArray(WebServer* server) : out(server) { out.append("[", 1); }

  void addEntry(const char* json, size_t len) {
    if (seenFirst) out.append(",", 1);
    seenFirst = true;
    out.append(json, len);
  }

  void finish() {
    out.append("]", 1);
    out.finish();
  }

 private:
  ChunkedResponse out;
  bool seenFirst = false;
};
