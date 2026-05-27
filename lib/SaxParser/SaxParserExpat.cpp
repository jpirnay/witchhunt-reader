#include <expat.h>

#include "SaxParser.h"

// Internal state held in the opaque impl_ pointer.
struct SaxParserImpl {
  XML_Parser parser = nullptr;

  SaxStartCb startCb = nullptr;
  SaxEndCb endCb = nullptr;
  SaxCharCb charCb = nullptr;
  SaxDefaultCb defaultCb = nullptr;
  void* userData = nullptr;

  // Wrappers that adapt expat's XML_Char* signatures to the SaxParser char* signatures.
  // expat defines XML_Char as char (when XML_UNICODE is not set), so these are casts only.
  static void XMLCALL expatStart(void* ud, const XML_Char* name, const XML_Char** atts) {
    auto* impl = static_cast<SaxParserImpl*>(ud);
    impl->startCb(impl->userData, name, atts);
  }
  static void XMLCALL expatEnd(void* ud, const XML_Char* name) {
    auto* impl = static_cast<SaxParserImpl*>(ud);
    impl->endCb(impl->userData, name);
  }
  static void XMLCALL expatChar(void* ud, const XML_Char* s, int len) {
    auto* impl = static_cast<SaxParserImpl*>(ud);
    impl->charCb(impl->userData, s, len);
  }
  static void XMLCALL expatDefault(void* ud, const XML_Char* s, int len) {
    auto* impl = static_cast<SaxParserImpl*>(ud);
    impl->defaultCb(impl->userData, s, len);
  }
};

// ---------------------------------------------------------------------------

SaxParser::~SaxParser() {
  if (!impl_) return;
  auto* impl = static_cast<SaxParserImpl*>(impl_);
  if (impl->parser) {
    XML_StopParser(impl->parser, XML_FALSE);
    XML_SetElementHandler(impl->parser, nullptr, nullptr);
    XML_SetCharacterDataHandler(impl->parser, nullptr);
    XML_SetDefaultHandlerExpand(impl->parser, nullptr);
    XML_ParserFree(impl->parser);
    impl->parser = nullptr;
  }
  delete impl;
  impl_ = nullptr;
}

bool SaxParser::init(void* userData, SaxStartCb startCb, SaxEndCb endCb, SaxCharCb charCb, SaxDefaultCb defaultCb) {
  // Destroy any previous state.
  this->~SaxParser();

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) return false;

  auto* impl = new SaxParserImpl;
  impl->parser = parser;
  impl->userData = userData;
  impl->startCb = startCb;
  impl->endCb = endCb;
  impl->charCb = charCb;
  impl->defaultCb = defaultCb;
  impl_ = impl;

  XML_SetUserData(parser, impl);
  XML_SetElementHandler(parser, SaxParserImpl::expatStart, SaxParserImpl::expatEnd);
  if (charCb) XML_SetCharacterDataHandler(parser, SaxParserImpl::expatChar);
  if (defaultCb) XML_SetDefaultHandlerExpand(parser, SaxParserImpl::expatDefault);

  return true;
}

bool SaxParser::feed(const uint8_t* buf, size_t len) {
  if (!impl_) return false;
  auto* impl = static_cast<SaxParserImpl*>(impl_);
  if (!impl->parser) return false;

  constexpr size_t kChunk = 1024;
  const uint8_t* cursor = buf;
  size_t remaining = len;

  while (remaining > 0) {
    const size_t chunk = remaining < kChunk ? remaining : kChunk;
    void* const expatBuf = XML_GetBuffer(impl->parser, static_cast<int>(chunk));
    if (!expatBuf) {
      errorString_ = "XML_GetBuffer allocation failure";
      return false;
    }
    memcpy(expatBuf, cursor, chunk);
    if (XML_ParseBuffer(impl->parser, static_cast<int>(chunk), 0) == XML_STATUS_ERROR) {
      if (XML_GetErrorCode(impl->parser) == XML_ERROR_ABORTED) {
        return true;  // stop() was called — intentional early exit, not an error
      }
      errorLine_ = static_cast<int>(XML_GetCurrentLineNumber(impl->parser));
      errorString_ = XML_ErrorString(XML_GetErrorCode(impl->parser));
      return false;
    }
    cursor += chunk;
    remaining -= chunk;
  }
  return true;
}

bool SaxParser::finalize() {
  if (!impl_) return false;
  auto* impl = static_cast<SaxParserImpl*>(impl_);
  if (!impl->parser) return false;

  bool ok = true;
  if (XML_ParseBuffer(impl->parser, 0, 1) == XML_STATUS_ERROR) {
    if (XML_GetErrorCode(impl->parser) != XML_ERROR_ABORTED) {
      errorLine_ = static_cast<int>(XML_GetCurrentLineNumber(impl->parser));
      errorString_ = XML_ErrorString(XML_GetErrorCode(impl->parser));
      ok = false;
    }
  }

  XML_StopParser(impl->parser, XML_FALSE);
  XML_SetElementHandler(impl->parser, nullptr, nullptr);
  XML_SetCharacterDataHandler(impl->parser, nullptr);
  XML_SetDefaultHandlerExpand(impl->parser, nullptr);
  XML_ParserFree(impl->parser);
  impl->parser = nullptr;

  return ok;
}

void SaxParser::stop() {
  if (!impl_) return;
  auto* impl = static_cast<SaxParserImpl*>(impl_);
  if (impl->parser) {
    stopped_ = true;
    XML_StopParser(impl->parser, XML_FALSE);
  }
}

uint32_t SaxParser::byteOffset() const {
  if (!impl_) return 0;
  auto* impl = static_cast<SaxParserImpl*>(impl_);
  if (!impl->parser) return 0;
  return static_cast<uint32_t>(XML_GetCurrentByteIndex(impl->parser));
}
