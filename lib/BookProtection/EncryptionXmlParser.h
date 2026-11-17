#pragma once

#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <cstddef>
#include <cstdint>

// META-INF/encryption.xml (OCF), streamed: hands each entry encrypted with AES-CBC to a visitor as
// its EncryptedData element closes, and keeps nothing per entry.
//
//   <EncryptedData>
//     <EncryptionMethod Algorithm="http://www.w3.org/2001/04/xmlenc#aes128-cbc"/>
//     <KeyInfo>...</KeyInfo>
//     <CipherData><CipherReference URI="OEBPS/chapter1.xhtml"/></CipherData>
//     <EncryptionProperties><EncryptionProperty>
//       <Compression Method="8" OriginalLength="40213"/>    (Readium LCP; Adobe gives none)
//     </EncryptionProperty></EncryptionProperties>
//   </EncryptedData>
//
// Elements are matched by local name, whatever prefix the document binds. Other algorithms (font
// obfuscation, an EncryptedKey's RSA) are not content encryption and are passed over. Without a
// Compression element the plaintext is deflated, as Adobe's is but for its aes128-cbc-uncompressed.
class EncryptionXmlParser final : public Print {
 public:
  struct Item {
    const char* uri;  // percent-decoded, relative to the container's root, NUL-terminated
    size_t uriLen;
    uint16_t cipherBits;  // 128 or 256
    bool deflated;
    bool sizeKnown;
    uint32_t plainSize;
  };
  // False stops the parse.
  using Visit = bool (*)(void* ctx, const Item& item);

  EncryptionXmlParser(Visit visit, void* ctx) : visit_(visit), ctx_(ctx) {}
  bool setup();
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buffer, size_t size) override;
  // Ends the document: false when it was malformed, cut short, a URI did not fit the parser, or the
  // visitor stopped it.
  bool finish();

 private:
  static void startElement(void* userData, const char* name, const char** atts);
  static void endElement(void* userData, const char* name);

  static constexpr size_t URI_CAPACITY = 512;

  SaxParser sax_;
  Visit visit_;
  void* ctx_;
  // The EncryptedData element being read.
  int depth_ = 0;
  int dataDepth_ = -1;  // its depth, -1 outside one
  uint16_t cipherBits_ = 0;
  bool deflated_ = true;
  bool sizeKnown_ = false;
  uint32_t plainSize_ = 0;
  char uri_[URI_CAPACITY] = {};
  size_t uriLen_ = 0;
  bool failed_ = false;
};
