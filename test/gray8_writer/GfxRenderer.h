#pragma once
// Configurable host-test GfxRenderer stub for DirectGray8Writer.
//
// Unlike the pipeline harness stub, orientation and panel geometry are settable:
// the whole point of these tests is that the byte writer and the bit writer agree
// under EVERY orientation, and a stub fixed at Portrait could not show that.
#include <cstdint>
#include <cstring>
#include <vector>

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  GfxRenderer(const int panelWidth, const int panelHeight, const Orientation orientation)
      : width_(panelWidth), height_(panelHeight), orientation_(orientation), widthBytes_((panelWidth + 7) / 8) {
    frameBuffer_.assign(static_cast<size_t>(widthBytes_) * height_, 0xFF);
  }

  int getDisplayWidth() const { return width_; }
  int getDisplayHeight() const { return height_; }
  uint16_t getDisplayWidthBytes() const { return static_cast<uint16_t>(widthBytes_); }
  Orientation getOrientation() const { return orientation_; }
  RenderMode getRenderMode() const { return mode_; }
  void setRenderMode(const RenderMode mode) { mode_ = mode; }
  uint8_t* getWriteTarget() const { return const_cast<uint8_t*>(frameBuffer_.data()); }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return height_; }

  // Oriented logical surface, mirroring the device renderer's swap on portrait.
  int getScreenWidth() const {
    return (orientation_ == Portrait || orientation_ == PortraitInverted) ? height_ : width_;
  }
  int getScreenHeight() const {
    return (orientation_ == Portrait || orientation_ == PortraitInverted) ? width_ : height_;
  }

  // True where the 1-bpp framebuffer holds a black pixel (bit clear = black).
  bool isBlackAt(const int x, const int y) const {
    return (frameBuffer_[static_cast<size_t>(y) * widthBytes_ + (x >> 3)] & (0x80 >> (x & 7))) == 0;
  }

 private:
  int width_;
  int height_;
  Orientation orientation_;
  int widthBytes_;
  RenderMode mode_ = BW;
  std::vector<uint8_t> frameBuffer_;
};
