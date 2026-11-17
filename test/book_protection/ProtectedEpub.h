#pragma once

// Protected EPUBs for host tests, as tools/make_protected_epub.py makes them for the device: every
// entry but the mimetype, META-INF/* and the OPF becomes a stored entry of IV and AES-CBC ciphertext
// of its raw DEFLATE (PKCS#7, Adobe's shape), listed in META-INF/encryption.xml. AES is OpenSSL's.
// An entry the source stores is wrapped in stored DEFLATE blocks, so no deflater is needed.

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <string>
#include <vector>

#include "TestZip.h"
#include "ZipFile.h"

namespace protected_epub {

// IV, then AES-CBC (128 or 256 by the key's length) of `plain`: PKCS#7 padding, or xmlenc's.
inline std::string encryptAes(const std::string& plain, const std::vector<uint8_t>& key, const bool xmlenc = false) {
  std::string padded = plain;
  const size_t pad = 16 - padded.size() % 16;
  for (size_t i = 0; i + 1 < pad; ++i) padded.push_back(static_cast<char>(xmlenc ? 0x5A ^ (i * 31) : pad));
  padded.push_back(static_cast<char>(pad));
  unsigned char iv[16];
  for (int i = 0; i < 16; ++i) iv[i] = static_cast<unsigned char>(0xC0 + i * 3);
  std::string out(reinterpret_cast<const char*>(iv), 16);
  out.resize(16 + padded.size());
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int produced = 0;
  EXPECT_EQ(EVP_EncryptInit_ex(c, key.size() == 16 ? EVP_aes_128_cbc() : EVP_aes_256_cbc(), nullptr, key.data(), iv),
            1);
  EVP_CIPHER_CTX_set_padding(c, 0);
  EXPECT_EQ(EVP_EncryptUpdate(c, reinterpret_cast<unsigned char*>(out.data()) + 16, &produced,
                              reinterpret_cast<const unsigned char*>(padded.data()), static_cast<int>(padded.size())),
            1);
  EVP_CIPHER_CTX_free(c);
  EXPECT_EQ(produced, static_cast<int>(padded.size()));
  return out;
}

// `plain` as a raw DEFLATE stream of stored blocks.
inline std::string storedDeflate(const std::string& plain) {
  std::string out;
  size_t at = 0;
  do {
    const size_t n = std::min<size_t>(65535, plain.size() - at);
    const bool last = at + n == plain.size();
    out.push_back(last ? 1 : 0);
    out.push_back(static_cast<char>(n));
    out.push_back(static_cast<char>(n >> 8));
    out.push_back(static_cast<char>(~n));
    out.push_back(static_cast<char>(~n >> 8));
    out.append(plain, at, n);
    at += n;
  } while (at < plain.size());
  return out;
}

// Writes `target`, the Adobe-style protected copy of the EPUB `source`, with `key` (16 bytes).
// Returns the number of entries encrypted.
inline int protectEpub(const std::string& source, const std::string& target, const std::vector<uint8_t>& key) {
  ZipFile zip(source);
  std::vector<std::string> names;
  zip.streamCentralDirectoryNames([&names](const std::string_view name) {
    if (!name.empty() && name.back() != '/') names.emplace_back(name);
  });
  size_t containerSize = 0;
  uint8_t* container = zip.readFileToMemory("META-INF/container.xml", &containerSize, true);
  EXPECT_NE(container, nullptr);
  const std::string containerXml = container ? reinterpret_cast<const char*>(container) : "";
  free(container);
  const size_t at = containerXml.find("full-path=\"") + 11;
  const std::string opf = containerXml.substr(at, containerXml.find('"', at) - at);

  std::vector<test_zip::EntrySpec> entries;
  std::string xml = R"(<?xml version="1.0"?><encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container")"
                    R"( xmlns:enc="http://www.w3.org/2001/04/xmlenc#">)";
  int encrypted = 0;
  for (const auto& name : names) {
    test_zip::StringSink plain;
    EXPECT_TRUE(zip.readFileToStream(name.c_str(), plain, 1024)) << name;
    if (name == "mimetype" || name.rfind("META-INF/", 0) == 0 || name == opf) {
      entries.push_back({name, plain.data});
      continue;
    }
    ZipFile::FileStatSlim stat{};
    EXPECT_TRUE(zip.loadFileStatSlim(name.c_str(), &stat));
    const std::string stream =
        stat.method == 8 ? test_zip::deflatedEntry(source, name.c_str()).stream : storedDeflate(plain.data);
    entries.push_back({name, encryptAes(stream, key)});
    xml += R"(<enc:EncryptedData><enc:EncryptionMethod Algorithm="http://www.w3.org/2001/04/xmlenc#aes128-cbc"/>)";
    xml += R"(<enc:CipherData><enc:CipherReference URI=")" + name + R"("/></enc:CipherData></enc:EncryptedData>)";
    ++encrypted;
  }
  entries.push_back({"META-INF/encryption.xml", xml + "</encryption>"});
  // The mimetype first, as OCF has it.
  std::stable_partition(entries.begin(), entries.end(),
                        [](const test_zip::EntrySpec& e) { return e.name == "mimetype"; });
  test_zip::writeZip(target, entries);
  return encrypted;
}

}  // namespace protected_epub
