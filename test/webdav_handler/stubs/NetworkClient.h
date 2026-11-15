#pragma once

// What WebDAVHandler.cpp, UploadStallTolerance.h and HttpFileStreamer.h touch of the framework's NetworkClient.
class NetworkClient {
 public:
  void setTimeout(unsigned long) {}
  void clear() {}
};
