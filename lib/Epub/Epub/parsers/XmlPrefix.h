#pragma once

#include <cstdint>
#include <cstring>

// The prefix a document binds one XML namespace to.
//
// A prefix is only a document's local name for a namespace: <ns0:package xmlns:ns0="http://www.idpf.org/2007/opf"> and
// <package xmlns="http://www.idpf.org/2007/opf"> are the same element. Python's ElementTree writes the first form, so a
// tool that rewrites a book may. SaxParser has no namespace mode, and the parsers compare literal names, so the
// declaration is read here and the name handed on without the prefix. CrossInk 8bd0001a (Julia Nguyen) fixed the same
// book in the OPF parser with Expat's namespace mode.
class XmlPrefix {
 public:
  explicit XmlPrefix(const char* uri) : uri_(uri) {}

  // Looks through an element's attributes for xmlns:<prefix>="<uri>". The last one found stands.
  void note(const char** atts) {
    constexpr char kDeclaration[] = "xmlns:";
    constexpr size_t kDeclarationBytes = sizeof(kDeclaration) - 1;
    for (int i = 0; atts[i]; i += 2) {
      if (strncmp(atts[i], kDeclaration, kDeclarationBytes) != 0 || strcmp(atts[i + 1], uri_) != 0) continue;
      const char* prefix = atts[i] + kDeclarationBytes;
      const size_t bytes = strlen(prefix);
      if (bytes == 0 || bytes > kMaxPrefixBytes) continue;  // no document's prefix is this long: not worth a buffer
      memcpy(prefix_, prefix, bytes);
      prefix_[bytes] = ':';
      length_ = static_cast<uint8_t>(bytes + 1);
    }
  }

  // The local name when `name` carries this prefix, else null.
  const char* localName(const char* name) const {
    return length_ != 0 && strncmp(name, prefix_, length_) == 0 ? name + length_ : nullptr;
  }

  // `name` without this prefix: a name with another prefix, or none, stays as it is.
  const char* strip(const char* name) const {
    const char* local = localName(name);
    return local != nullptr ? local : name;
  }

 private:
  static constexpr size_t kMaxPrefixBytes = 15;

  const char* uri_;
  char prefix_[kMaxPrefixBytes + 1] = {};  // with its ':'
  uint8_t length_ = 0;
};
