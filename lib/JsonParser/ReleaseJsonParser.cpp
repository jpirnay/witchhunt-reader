#include "ReleaseJsonParser.h"

#include <cstdlib>
#include <cstring>

#include "ReleaseNotesExcerpt.h"

namespace {

// The notes dialog shows a few lines; more would only be cut by its own line cap.
constexpr size_t NOTES_MAX_LINES = 4;

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// safeCopy for text that is drawn: a cut never lands inside a multi-byte UTF-8 sequence.
void safeCopyUtf8(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  if (n < srcLen) {
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "sha256:" and 64 hex digits into 32 bytes. False, with out untouched, for anything else.
bool parseSha256Digest(const char* value, size_t len, uint8_t* out) {
  static constexpr char PREFIX[] = "sha256:";
  static constexpr size_t PREFIX_LEN = sizeof(PREFIX) - 1;
  if (len != PREFIX_LEN + 64 || memcmp(value, PREFIX, PREFIX_LEN) != 0) return false;
  uint8_t bytes[32];
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexValue(value[PREFIX_LEN + 2 * i]);
    const int lo = hexValue(value[PREFIX_LEN + 2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  memcpy(out, bytes, sizeof(bytes));
  return true;
}

}  // namespace

ReleaseJsonParser::ReleaseJsonParser()
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd, sOnStringOverflow}) {
  safeCopy(firmwareAssetName, sizeof(firmwareAssetName), "firmware.bin", sizeof("firmware.bin") - 1);
  reset();
}

void ReleaseJsonParser::setFirmwareAssetName(const char* name) {
  safeCopy(firmwareAssetName, sizeof(firmwareAssetName), name, strlen(name));
}

void ReleaseJsonParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  depth = 0;
  assetDepth = 0;
  tagName[0] = '\0';
  firmwareUrl[0] = '\0';
  firmwareSize = 0;
  memset(firmwareSha256, 0, sizeof(firmwareSha256));
  firmwareHasSha256 = false;
  tagFound = false;
  firmwareFound = false;
  releaseName[0] = '\0';
  releaseNotes[0] = '\0';
  notesFound = false;
  topLevelArray = false;
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSize = 0;
  memset(currentAssetSha256, 0, sizeof(currentAssetSha256));
  currentAssetHasSha256 = false;
}

bool ReleaseJsonParser::inReleaseObject() const {
  if (!topLevelArray) {
    return depth == 1;
  }
  return depth == 2;
}

void ReleaseJsonParser::feed(const char* data, size_t len) { parser.feed(data, len); }

bool ReleaseJsonParser::foundTag() const { return tagFound; }
bool ReleaseJsonParser::foundFirmware() const { return firmwareFound; }
const char* ReleaseJsonParser::getTagName() const { return tagName; }
const char* ReleaseJsonParser::getFirmwareUrl() const { return firmwareUrl; }
size_t ReleaseJsonParser::getFirmwareSize() const { return firmwareSize; }
bool ReleaseJsonParser::hasFirmwareSha256() const { return firmwareHasSha256; }
const uint8_t* ReleaseJsonParser::getFirmwareSha256() const { return firmwareSha256; }
const char* ReleaseJsonParser::getReleaseName() const { return releaseName; }
bool ReleaseJsonParser::foundNotes() const { return notesFound; }
const char* ReleaseJsonParser::getReleaseNotes() const { return releaseNotes; }

// A value of the release object itself, not of an asset, the author or the reactions.
bool ReleaseJsonParser::atReleaseKey(LastKey key) const {
  return lastKey == key && position == Position::TOP_LEVEL && inReleaseObject();
}

void ReleaseJsonParser::takeNotes(const char* body, size_t len, bool complete) {
  release_notes::excerpt(body, len, complete, releaseNotes, sizeof(releaseNotes), NOTES_MAX_LINES);
  notesFound = true;
}

void ReleaseJsonParser::commitAsset() {
  if (strcmp(currentAssetName, firmwareAssetName) == 0) {
    memcpy(firmwareUrl, currentAssetUrl, sizeof(firmwareUrl));
    firmwareSize = currentAssetSize;
    memcpy(firmwareSha256, currentAssetSha256, sizeof(firmwareSha256));
    firmwareHasSha256 = currentAssetHasSha256;
    firmwareFound = true;
  }
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSize = 0;
  memset(currentAssetSha256, 0, sizeof(currentAssetSha256));
  currentAssetHasSha256 = false;
}

void ReleaseJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->inReleaseObject()) {
        if (len == 8 && memcmp(key, "tag_name", 8) == 0)
          self->lastKey = LastKey::TAG_NAME;
        else if (len == 4 && memcmp(key, "name", 4) == 0)
          self->lastKey = LastKey::RELEASE_NAME;
        else if (len == 4 && memcmp(key, "body", 4) == 0)
          self->lastKey = LastKey::BODY;
        else if (len == 6 && memcmp(key, "assets", 6) == 0)
          self->lastKey = LastKey::ASSETS;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    case Position::IN_ASSET_OBJECT:
      if (self->assetDepth == 1) {
        if (len == 4 && memcmp(key, "name", 4) == 0)
          self->lastKey = LastKey::ASSET_NAME;
        else if (len == 20 && memcmp(key, "browser_download_url", 20) == 0)
          self->lastKey = LastKey::ASSET_URL;
        else if (len == 4 && memcmp(key, "size", 4) == 0)
          self->lastKey = LastKey::ASSET_SIZE;
        else if (len == 6 && memcmp(key, "digest", 6) == 0)
          self->lastKey = LastKey::ASSET_DIGEST;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->lastKey) {
    case LastKey::TAG_NAME:
      if (self->position == Position::TOP_LEVEL && self->inReleaseObject()) {
        safeCopy(self->tagName, sizeof(self->tagName), value, len);
        self->tagFound = true;
      }
      break;
    case LastKey::RELEASE_NAME:
      if (self->atReleaseKey(LastKey::RELEASE_NAME))
        safeCopyUtf8(self->releaseName, sizeof(self->releaseName), value, len);
      break;
    case LastKey::BODY:
      if (self->atReleaseKey(LastKey::BODY)) self->takeNotes(value, len, true);
      break;
    case LastKey::ASSET_NAME:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetName, sizeof(self->currentAssetName), value, len);
      break;
    case LastKey::ASSET_URL:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetUrl, sizeof(self->currentAssetUrl), value, len);
      break;
    case LastKey::ASSET_DIGEST:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        self->currentAssetHasSha256 = parseSha256Digest(value, len, self->currentAssetSha256);
      break;
    default:
      break;
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  if (self->lastKey == LastKey::ASSET_SIZE && self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1) {
    self->currentAssetSize = static_cast<size_t>(strtoul(value, nullptr, 10));
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNull(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);
  if (self->atReleaseKey(LastKey::BODY)) self->notesFound = true;  // no notes, and nothing more to wait for
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnStringOverflow(void* ctx, const char* prefix, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);
  if (self->atReleaseKey(LastKey::BODY)) self->takeNotes(prefix, len, false);
  // The rest of the value is skipped and no onString follows to clear the key.
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      self->depth++;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::IN_ASSET_OBJECT;
      self->assetDepth = 1;
      self->currentAssetName[0] = '\0';
      self->currentAssetUrl[0] = '\0';
      self->currentAssetSize = 0;
      memset(self->currentAssetSha256, 0, sizeof(self->currentAssetSha256));
      self->currentAssetHasSha256 = false;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
  }
}

void ReleaseJsonParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      if (self->assetDepth == 0) {
        self->commitAsset();
        self->position = Position::IN_ASSETS_ARRAY;
      }
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth == 0) {
        self->topLevelArray = true;
        self->depth++;
      } else if (self->lastKey == LastKey::ASSETS && self->inReleaseObject()) {
        self->position = Position::IN_ASSETS_ARRAY;
      } else {
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) {
        self->depth--;
      }
      if (self->topLevelArray && self->depth == 0) {
        self->topLevelArray = false;
      }
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::TOP_LEVEL;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      self->lastKey = LastKey::NONE;
      break;
  }
}
