#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

// Ported from crosspoint-reader/crosspoint-reader (MIT),
// initially authored in PR #1810 by znelson and contributors.
// The configurable firmware asset name comes from PR #2983 by Justin Mitchell.
class ReleaseJsonParser {
 public:
  ReleaseJsonParser();

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  // Release asset this parser accepts as the firmware image, matched exactly.
  // Defaults to "firmware.bin" (the X3/X4 asset); other boards set their own
  // "firmware-<board>.bin". Survives reset() so a retry keeps the choice.
  void setFirmwareAssetName(const char* name);

  void reset();
  void feed(const char* data, size_t len);

  bool foundTag() const;
  bool foundFirmware() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  size_t getFirmwareSize() const;

 private:
  bool inReleaseObject() const;

  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitAsset();

  StreamingJsonParser parser;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;

  char tagName[32];
  char firmwareUrl[512];
  size_t firmwareSize;
  bool tagFound;
  bool firmwareFound;

  char firmwareAssetName[32];
  char currentAssetName[32];
  char currentAssetUrl[512];
  bool topLevelArray;
  size_t currentAssetSize;
};
