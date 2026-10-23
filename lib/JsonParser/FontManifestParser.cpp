#include "FontManifestParser.h"

#include <cstring>
#include <limits>

namespace {

template <size_t N>
bool keyIs(const char* key, const size_t len, const char (&name)[N]) {
  return len == N - 1 && memcmp(key, name, N - 1) == 0;
}

// The number token as a uint32_t: digits only -- no sign, fraction or exponent -- and at most
// 4294967295, the values ArduinoJson's is<uint32_t>() accepted.
bool parseUint32(const char* text, const size_t len, uint32_t& out) {
  if (len == 0) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < len; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    if (value > std::numeric_limits<uint32_t>::max()) return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

// Copies what fits, NUL-terminated, and returns how much that was.
size_t copyBounded(char* dst, const size_t dstSize, const char* src, const size_t srcLen) {
  const size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
  return n;
}

}  // namespace

FontManifestParser::FontManifestParser(const FontManifestCallbacks& callbacks)
    : json(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                         sOnArrayStart, sOnArrayEnd}),
      cb(callbacks) {
  reset();
}

void FontManifestParser::reset() {
  json.reset();
  where = Where::BEFORE_ROOT;
  pendingKey = Key::NONE;
  skipDepth = 0;
  structureError = false;
  manifestVersion = 0;
  baseUrlBuf[0] = '\0';
  baseUrlOverflowed = false;
  beginFile();
}

void FontManifestParser::feed(const char* data, const size_t len) { json.feed(data, len); }

bool FontManifestParser::hasError() const {
  // StreamingJsonParser scans on past the root, and stops at its first error. Once the root has
  // closed, any error it finds is in content that is ignored.
  return structureError || (json.hasError() && !complete());
}

void FontManifestParser::sOnKey(void* ctx, const char* key, const size_t len) {
  static_cast<FontManifestParser*>(ctx)->onKey(key, len);
}

void FontManifestParser::sOnString(void* ctx, const char* value, const size_t len) {
  static_cast<FontManifestParser*>(ctx)->onString(value, len);
}

void FontManifestParser::sOnNumber(void* ctx, const char* value, const size_t len) {
  static_cast<FontManifestParser*>(ctx)->onNumber(value, len);
}

void FontManifestParser::sOnBool(void* ctx, const bool value) { static_cast<FontManifestParser*>(ctx)->onBool(value); }

void FontManifestParser::sOnNull(void* ctx) { static_cast<FontManifestParser*>(ctx)->onOtherValue(); }

void FontManifestParser::sOnObjectStart(void* ctx) { static_cast<FontManifestParser*>(ctx)->onOpen(false); }

void FontManifestParser::sOnObjectEnd(void* ctx) { static_cast<FontManifestParser*>(ctx)->onClose(false); }

void FontManifestParser::sOnArrayStart(void* ctx) { static_cast<FontManifestParser*>(ctx)->onOpen(true); }

void FontManifestParser::sOnArrayEnd(void* ctx) { static_cast<FontManifestParser*>(ctx)->onClose(true); }

FontManifestParser::Key FontManifestParser::takeKey() {
  const Key key = pendingKey;
  pendingKey = Key::NONE;
  return key;
}

void FontManifestParser::onKey(const char* key, const size_t len) {
  if (ignoring()) return;
  // A key straight after a key: StreamingJsonParser dropped the value between them.
  if (pendingKey != Key::NONE) valueDropped(takeKey());

  switch (where) {
    case Where::IN_ROOT:
      if (keyIs(key, len, "version")) {
        pendingKey = Key::VERSION;
      } else if (keyIs(key, len, "baseUrl")) {
        pendingKey = Key::BASE_URL;
      } else if (keyIs(key, len, "families")) {
        pendingKey = Key::FAMILIES;
      } else {
        pendingKey = Key::OTHER;
      }
      break;
    case Where::IN_FAMILY:
      if (keyIs(key, len, "name")) {
        pendingKey = Key::NAME;
      } else if (keyIs(key, len, "description")) {
        pendingKey = Key::DESCRIPTION;
      } else if (keyIs(key, len, "format")) {
        pendingKey = Key::FORMAT;
      } else if (keyIs(key, len, "inDownloadAll")) {
        pendingKey = Key::IN_DOWNLOAD_ALL;
      } else if (keyIs(key, len, "files")) {
        pendingKey = Key::FILES;
      } else {
        pendingKey = Key::OTHER;
      }
      break;
    case Where::IN_FILE:
      if (keyIs(key, len, "name")) {
        pendingKey = Key::NAME;
      } else if (keyIs(key, len, "size")) {
        pendingKey = Key::SIZE;
      } else if (keyIs(key, len, "crc32")) {
        pendingKey = Key::CRC32;
      } else {
        pendingKey = Key::OTHER;
      }
      break;
    default:
      pendingKey = Key::OTHER;
      break;
  }
}

// StreamingJsonParser drops a string or number too long for its token buffer, and says nothing. The
// only trace is a key with no value before the next key or the closing brace.
void FontManifestParser::valueDropped(const Key key) {
  if (where == Where::IN_ROOT && key == Key::BASE_URL) {
    baseUrlBuf[0] = '\0';
    baseUrlOverflowed = true;
  } else if (where == Where::IN_FILE && key == Key::NAME) {
    fileName[0] = '\0';
    fileNameLen = 0;
    fileNameOverflow = true;
  }
}

void FontManifestParser::onString(const char* value, const size_t len) {
  if (ignoring()) return;
  const Key key = takeKey();
  switch (where) {
    case Where::IN_ROOT:
      if (key == Key::BASE_URL) baseUrlOverflowed = copyBounded(baseUrlBuf, sizeof(baseUrlBuf), value, len) < len;
      break;
    case Where::IN_FAMILIES:
      emitEmptyFamily();
      break;
    case Where::IN_FAMILY:
      if (key == Key::NAME && cb.onFamilyName) cb.onFamilyName(cb.ctx, value, len);
      if (key == Key::DESCRIPTION && cb.onFamilyDescription) cb.onFamilyDescription(cb.ctx, value, len);
      if (key == Key::FORMAT && cb.onFamilyFormat) cb.onFamilyFormat(cb.ctx, value, len);
      break;
    case Where::IN_FILES:
      emitEmptyFile();
      break;
    case Where::IN_FILE:
      if (key == Key::NAME) {
        fileNameLen = copyBounded(fileName, sizeof(fileName), value, len);
        fileNameOverflow = fileNameLen < len;
      }
      break;
    default:
      break;
  }
}

void FontManifestParser::onNumber(const char* value, const size_t len) {
  if (ignoring()) return;
  const Key key = takeKey();
  uint32_t number = 0;
  const bool isUint32 = parseUint32(value, len, number);
  switch (where) {
    case Where::IN_ROOT:
      if (key == Key::VERSION) {
        manifestVersion =
            isUint32 && number <= static_cast<uint32_t>(std::numeric_limits<int>::max()) ? static_cast<int>(number) : 0;
      }
      break;
    case Where::IN_FAMILIES:
      emitEmptyFamily();
      break;
    case Where::IN_FILES:
      emitEmptyFile();
      break;
    case Where::IN_FILE:
      if (key == Key::SIZE) {
        fileSize = isUint32 ? number : 0;
      } else if (key == Key::CRC32) {
        fileHasCrc32 = isUint32;
        fileCrc32 = isUint32 ? number : 0;
      }
      break;
    default:
      break;
  }
}

void FontManifestParser::onBool(const bool value) {
  if (ignoring()) return;
  if (where == Where::IN_FAMILY && pendingKey == Key::IN_DOWNLOAD_ALL) {
    takeKey();
    if (cb.onFamilyInDownloadAll) cb.onFamilyInDownloadAll(cb.ctx, value);
    return;
  }
  onOtherValue();
}

// true, false or null where the manifest reads none, which is still an element of an array.
void FontManifestParser::onOtherValue() {
  if (ignoring()) return;
  takeKey();
  if (where == Where::IN_FAMILIES) {
    emitEmptyFamily();
  } else if (where == Where::IN_FILES) {
    emitEmptyFile();
  }
}

void FontManifestParser::onOpen(const bool isArray) {
  if (where == Where::DONE) return;
  if (skipDepth > 0) {
    ++skipDepth;
    return;
  }

  const Key key = takeKey();
  switch (where) {
    case Where::BEFORE_ROOT:
      // A root that is not an object holds no manifest: skipped, and the document ends with it.
      if (isArray) {
        skipDepth = 1;
      } else {
        where = Where::IN_ROOT;
      }
      break;
    case Where::IN_ROOT:
      if (isArray && key == Key::FAMILIES) {
        where = Where::IN_FAMILIES;
      } else {
        skipDepth = 1;
      }
      break;
    case Where::IN_FAMILIES:
      if (isArray) {
        emitEmptyFamily();
        skipDepth = 1;
      } else {
        if (cb.onFamilyBegin) cb.onFamilyBegin(cb.ctx);
        where = Where::IN_FAMILY;
      }
      break;
    case Where::IN_FAMILY:
      if (isArray && key == Key::FILES) {
        where = Where::IN_FILES;
      } else {
        skipDepth = 1;
      }
      break;
    case Where::IN_FILES:
      if (isArray) {
        emitEmptyFile();
        skipDepth = 1;
      } else {
        beginFile();
        where = Where::IN_FILE;
      }
      break;
    case Where::IN_FILE:
      skipDepth = 1;
      break;
    case Where::DONE:
      break;
  }
}

void FontManifestParser::onClose(const bool isArray) {
  if (where == Where::DONE) return;
  if (skipDepth > 0) {
    --skipDepth;
    if (skipDepth == 0 && where == Where::BEFORE_ROOT) where = Where::DONE;
    return;
  }
  // A key with nothing after it before the closing brace: StreamingJsonParser dropped its value.
  if (pendingKey != Key::NONE) valueDropped(takeKey());

  // The closer must match the container: StreamingJsonParser pops either kind with either bracket.
  const auto expect = [this, isArray](const bool array) {
    if (isArray != array) structureError = true;
  };
  switch (where) {
    case Where::BEFORE_ROOT:
      structureError = true;  // closes what never opened
      break;
    case Where::IN_ROOT:
      expect(false);
      where = Where::DONE;
      break;
    case Where::IN_FAMILIES:
      expect(true);
      where = Where::IN_ROOT;
      break;
    case Where::IN_FAMILY:
      expect(false);
      if (cb.onFamilyEnd) cb.onFamilyEnd(cb.ctx);
      where = Where::IN_FAMILIES;
      break;
    case Where::IN_FILES:
      expect(true);
      where = Where::IN_FAMILY;
      break;
    case Where::IN_FILE:
      expect(false);
      emitFile();
      where = Where::IN_FILES;
      break;
    case Where::DONE:
      break;
  }
}

void FontManifestParser::beginFile() {
  fileName[0] = '\0';
  fileNameLen = 0;
  fileNameOverflow = false;
  fileSize = 0;
  fileCrc32 = 0;
  fileHasCrc32 = false;
}

void FontManifestParser::emitFile() {
  if (!cb.onFile) return;
  const FontManifestFile file{fileName, fileNameLen, fileNameOverflow, fileSize, fileCrc32, fileHasCrc32};
  cb.onFile(cb.ctx, file);
}

void FontManifestParser::emitEmptyFamily() {
  if (cb.onFamilyBegin) cb.onFamilyBegin(cb.ctx);
  if (cb.onFamilyEnd) cb.onFamilyEnd(cb.ctx);
}

void FontManifestParser::emitEmptyFile() {
  beginFile();
  emitFile();
}
