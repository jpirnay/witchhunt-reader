#pragma once

#include <cstddef>
#include <cstdint>

// Callback signatures matching the existing XMLCALL static-function pattern.
// atts is a null-terminated flat array of alternating key/value pairs (expat convention).
using SaxStartCb = void (*)(void* userData, const char* name, const char** atts);
using SaxEndCb = void (*)(void* userData, const char* name);
using SaxCharCb = void (*)(void* userData, const char* s, int len);
using SaxDefaultCb = void (*)(void* userData, const char* s, int len);  // entities / default

// XML_Char compatibility alias: expat defined XML_Char as char; we keep the
// same typedef here so XPath mapper code that uses XML_Char compiles unchanged.
using XML_Char = char;

class SaxParser {
 public:
  SaxParser() = default;
  ~SaxParser();

  SaxParser(const SaxParser&) = delete;
  SaxParser& operator=(const SaxParser&) = delete;

  // Initialise the parser and register callbacks. Returns false if the underlying
  // engine fails to allocate (same as XML_ParserCreate returning nullptr).
  bool init(void* userData, SaxStartCb startCb, SaxEndCb endCb, SaxCharCb charCb = nullptr,
            SaxDefaultCb defaultCb = nullptr);

  // Feed a chunk of bytes. Returns false on parse error; errorLine()/errorString()
  // are valid after a false return.
  bool feed(const uint8_t* buf, size_t len);

  // Signal end-of-input. Returns false on truncated document or parse error.
  // Returns true when parsing was stopped early via stop().
  bool finalize();

  // Abort parsing early (no further callbacks will fire after the current callback returns).
  void stop();

  // Running byte offset of the parse cursor — replacement for XML_GetCurrentByteIndex.
  // Valid inside any callback fired from feed().
  uint32_t byteOffset() const;

  // Diagnostic: bitmask of fixed-capacity limits that were hit (and therefore
  // silently truncated) during parsing. The yxml backend uses fixed buffers
  // sized from measured real-world maxima; if a document exceeds them the
  // parser degrades gracefully (drops the excess) rather than overflowing, but
  // the result is lossy. Callers with logging should surface a non-zero value
  // so field cases that exceed the bounds are diagnosable. Always 0 for expat.
  enum TruncationFlag : uint32_t {
    kTruncElemName  = 1u << 0,  // element name longer than kElemNameLen
    kTruncAttrName  = 1u << 1,  // attribute name longer than kElemNameLen
    kTruncAttrValue = 1u << 2,  // attribute value longer than kAttrValueLen
    kTruncMaxAttrs  = 1u << 3,  // more than kMaxAttrs attributes on one element
    kTruncMaxDepth  = 1u << 4,  // nesting deeper than kMaxDepth
  };
  uint32_t truncationFlags() const;

  // Error details — populated on feed()/finalize() returning false.
  int errorLine() const { return errorLine_; }
  const char* errorString() const { return errorString_ ? errorString_ : ""; }

  bool isActive() const { return impl_ != nullptr; }
  // True after stop() has been called — distinguishes intentional early exit from errors.
  bool isStopped() const { return stopped_; }

 private:
  void reset();  // releases impl_ and zeros all state; safe to call any number of times

  void* impl_ = nullptr;
  bool stopped_ = false;
  int errorLine_ = 0;
  const char* errorString_ = nullptr;
};
