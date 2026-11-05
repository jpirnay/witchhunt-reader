#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "FontBytes.h"
#include "TtfFace.h"
#include "TtfFamily.h"
#include "TtfFont.h"
#include "TtfRasterizer.h"

namespace ttf_test {

/// A font file read through ttf::FileSource, as the SD card serves it, with an I/O error on demand. Every read's
/// offset is kept, so a test can see which way the reads went.
struct MemoryFile {
  std::vector<uint8_t> bytes;
  bool failReads = false;
  /// Reads that do not lie inside [readsFrom, readsTo) fail as well.
  uint32_t readsFrom = 0;
  uint32_t readsTo = UINT32_MAX;
  uint32_t reads = 0;
  uint32_t stamp = 0x57A3;  ///< what the card says of the file: FileSource::stamp
  std::vector<uint32_t> offsets;

  ttf::FileSource source() { return {static_cast<uint32_t>(bytes.size()), &MemoryFile::read, this, stamp}; }

  static bool read(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
    auto* file = static_cast<MemoryFile*>(ctx);
    ++file->reads;
    file->offsets.push_back(offset);
    if (file->failReads || offset > file->bytes.size() || len > file->bytes.size() - offset) return false;
    if (offset < file->readsFrom || uint64_t{offset} + len > file->readsTo) return false;
    std::memcpy(dst, file->bytes.data() + offset, len);
    return true;
  }
};

/// What the rasterizer draws for `cp` at `points`, straight from the file: what a stored glyph must equal.
struct Reference {
  EpdGlyph metrics{};
  std::vector<uint8_t> bitmap;
};

/// The same at a pixels-per-em (26.6).
inline Reference referenceAt(const std::vector<uint8_t>& file, const uint32_t cp, const uint32_t ppem) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer rasterizer;
  EXPECT_TRUE(rasterizer.allocate(
      ttf::planScratch(face, static_cast<uint32_t>(TtfFont::kMaxNativePx) << 6, ttf::kPoolBytesFlash)));
  Reference ref;
  EXPECT_EQ(ttf::Status::Ok, rasterizer.render(bytes, face, ttf::glyphIndex(bytes, face, cp), ppem, &ref.metrics));
  ref.bitmap.assign(rasterizer.output(), rasterizer.output() + ref.metrics.dataLength);
  return ref;
}

inline Reference reference(const std::vector<uint8_t>& file, const uint32_t cp, const uint8_t points) {
  return referenceAt(file, cp, ttf::ppemFromPoints(points));
}

/// What the rasterizer draws for `cp` at a pixels-per-em (26.6), emboldened at the engine's strength: what a glyph of
/// a synthetic bold style must equal.
inline Reference emboldenedAt(const std::vector<uint8_t>& file, const uint32_t cp, const uint32_t ppem) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer rasterizer;
  EXPECT_TRUE(rasterizer.allocate(
      ttf::planScratch(face, static_cast<uint32_t>(TtfFont::kMaxNativePx) << 6, ttf::kPoolBytesFlash, true)));
  Reference ref;
  EXPECT_EQ(ttf::Status::Ok, rasterizer.load(bytes, face, ttf::glyphIndex(bytes, face, cp), ppem, &ref.metrics));
  EXPECT_EQ(ttf::Status::Ok, rasterizer.embolden(ttf::emboldenStrength(ppem), &ref.metrics));
  EXPECT_EQ(ttf::Status::Ok, rasterizer.rasterize(rasterizer.output()));
  ref.bitmap.assign(rasterizer.output(), rasterizer.output() + ref.metrics.dataLength);
  return ref;
}

constexpr uint32_t px(const uint32_t pixels) { return pixels << 6; }

/// A glyph's bitmap, wherever TtfFont keeps it: the ring, or the store's log.
inline std::vector<uint8_t> bitmapOf(const TtfFont& font, const uint8_t style, const EpdGlyph* glyph) {
  const uint8_t* bitmap = nullptr;
  if (!font.ownsBitmap(glyph, bitmap)) bitmap = font.font(style)->data->bitmap + glyph->dataOffset;
  return bitmap ? std::vector<uint8_t>(bitmap, bitmap + glyph->dataLength) : std::vector<uint8_t>{};
}

inline void expectDrawnAs(const Reference& ref, const TtfFont& font, const uint8_t style, const EpdGlyph* glyph) {
  ASSERT_NE(nullptr, glyph);
  EXPECT_EQ(ref.metrics.width, glyph->width);
  EXPECT_EQ(ref.metrics.height, glyph->height);
  EXPECT_EQ(ref.metrics.left, glyph->left);
  EXPECT_EQ(ref.metrics.top, glyph->top);
  EXPECT_EQ(ref.metrics.advanceX, glyph->advanceX);
  EXPECT_EQ(ref.bitmap, bitmapOf(font, style, glyph));
}

}  // namespace ttf_test
