#include "QrUtils.h"

#include <Memory.h>
#include <Utf8.h>
#include <qrcodegen.h>

#include <algorithm>
#include <memory>

#include "Logging.h"
#include "components/UITheme.h"

namespace {

bool hasNonAscii(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (data[i] > 0x7F) return true;
  }
  return false;
}

// Byte-mode data capacity (bytes) at ECC LOW for QR versions 1-40
// (ISO/IEC 18004; matches qrcodegen's internal limits). Byte mode is the
// pessimistic bound — numeric/alphanumeric payloads fit more, never less.
constexpr uint16_t BYTE_CAPACITY_ECC_LOW[qrcodegen_VERSION_MAX] = {
    17,   32,   53,   78,   106,  134,  154,  192,  230,  271,  321,  367,  425,  458,
    520,  586,  644,  718,  792,  858,  929,  1003, 1091, 1171, 1273, 1367, 1465, 1528,
    1628, 1732, 1840, 1952, 2068, 2188, 2303, 2431, 2563, 2699, 2809, 2953};

int versionForByteLen(size_t len) {
  for (int v = qrcodegen_VERSION_MIN; v <= qrcodegen_VERSION_MAX; v++) {
    if (BYTE_CAPACITY_ECC_LOW[v - 1] >= len) return v;
  }
  return qrcodegen_VERSION_MAX;
}

}  // namespace

int QrUtils::compactSpacing() { return std::max(2, UITheme::getInstance().getMetrics().verticalSpacing / 4); }

bool QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  bool truncated = false;
  size_t len = textPayload.size();
  const char* text = textPayload.c_str();

  // Truncate at a UTF-8 safe boundary if needed
  std::string truncatedStr;
  if (len > MAX_QR_CAPACITY) {
    len = utf8SafeTruncateBuffer(text, static_cast<int>(MAX_QR_CAPACITY));
    truncatedStr = textPayload.substr(0, len);
    text = truncatedStr.c_str();
    truncated = true;
    LOG_DBG("QR", "Truncated payload from %u to %u bytes", textPayload.size(), len);
  }

  const auto* rawData = reinterpret_cast<const uint8_t*>(text);
  const bool nonAscii = hasNonAscii(rawData, len);

  // ASCII payloads size both work buffers for the smallest version that fits
  // (a ~40-byte URL needs ~2x200 bytes instead of 2x3918), so short QR paints
  // survive low-heap phases. The ECI path keeps VERSION_MAX buffers because
  // qrcodegen_encodeSegmentsAdvanced documents its buffer requirement against
  // qrcodegen_VERSION_MAX, not the maxVersion argument.
  const int maxVersion = nonAscii ? qrcodegen_VERSION_MAX : versionForByteLen(len);
  const size_t bufLen = qrcodegen_BUFFER_LEN_FOR_VERSION(maxVersion);
  auto qrcode = makeUniqueNoThrow<uint8_t[]>(bufLen);
  auto tempBuf = makeUniqueNoThrow<uint8_t[]>(bufLen);
  if (!qrcode || !tempBuf) {
    LOG_ERR("QR", "OOM allocating 2x%u byte QR buffers, skipping QR", bufLen);
    return truncated;
  }

  bool ok = false;

  if (nonAscii) {
    // Non-ASCII content: use ECI mode 26 (UTF-8) + byte segment via the low-level API
    // so scanners know the encoding rather than assuming ISO 8859-1.
    uint8_t eciBuf[4] = {};
    struct qrcodegen_Segment eciSeg = qrcodegen_makeEci(26, eciBuf);

    // Build byte segment — the segment data buffer can overlap with tempBuf
    const size_t segBufSize = qrcodegen_calcSegmentBufferSize(qrcodegen_Mode_BYTE, len);
    auto segBuf = makeUniqueNoThrow<uint8_t[]>(segBufSize);
    if (!segBuf) {
      LOG_ERR("QR", "OOM allocating %u byte QR segment buffer, skipping QR", segBufSize);
      return truncated;
    }
    struct qrcodegen_Segment byteSeg = qrcodegen_makeBytes(rawData, len, segBuf.get());

    struct qrcodegen_Segment segs[2] = {eciSeg, byteSeg};
    ok = qrcodegen_encodeSegmentsAdvanced(segs, 2, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                          qrcodegen_Mask_AUTO, false, tempBuf.get(), qrcode.get());
  } else {
    // ASCII-only: let the library auto-select the optimal mode (numeric/alphanumeric/byte)
    ok = qrcodegen_encodeText(text, tempBuf.get(), qrcode.get(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, maxVersion,
                              qrcodegen_Mask_AUTO, false);
  }

  if (ok) {
    const int size = qrcodegen_getSize(qrcode.get());
    const int maxDim = std::min(bounds.width, bounds.height);

    int px = maxDim / size;
    if (px < 1) px = 1;

    const int qrDisplaySize = size * px;
    const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2;
    const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2;

    for (int cy = 0; cy < size; cy++) {
      for (int cx = 0; cx < size; cx++) {
        if (qrcodegen_getModule(qrcode.get(), cx, cy)) {
          renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
        }
      }
    }
    // In Dark Mode the code keeps its polarity as a picture does: not every scanner reads white modules on black. Its
    // quiet zone (four modules, as far as the bounds reach) turns white with it.
    const int quiet = 4 * px;
    const int qx0 = std::max(bounds.x, xOff - quiet);
    const int qy0 = std::max(bounds.y, yOff - quiet);
    const int qx1 = std::min(bounds.x + bounds.width, xOff + qrDisplaySize + quiet);
    const int qy1 = std::min(bounds.y + bounds.height, yOff + qrDisplaySize + quiet);
    renderer.preserveImagePolarity(qx0, qy0, qx1 - qx0, qy1 - qy0);
  } else {
    LOG_ERR("QR", "Failed to encode QR code (%u bytes)", len);
  }

  return truncated;
}
