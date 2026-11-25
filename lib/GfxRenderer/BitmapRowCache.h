#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class Bitmap;

// The 2-bit rows Bitmap::readNextRow() makes of a BMP, kept on the card, so that later draws of the picture read them
// instead of decoding and dithering it again.
//
// A sleep picture on a four-level panel is drawn once per plane (the B/W base, then the two gray planes), and each
// draw decoded and dithered the whole BMP again: on the X3 about 0.77 s a plane for a 528 x 792 cover, nearly all of
// it readNextRow(). The rows do not depend on the plane, the orientation or the placement (drawBitmap() applies
// those), only on the file and on how it is toned and dithered, so one recording serves every plane of this sleep and
// of later ones.
//
// The file holds a header naming the rendition (Bitmap::renditionFingerprint()), the picture's size, and then every
// row in file order. One whose header does not match is recorded again; so is a missing or truncated one.
//
// Attach before the bitmap's first row is read. While it is attached, rows are read 2-bit only: a draw that wants the
// 8-bit samples (a canvas or level planes) runs without one. The bitmap must outlive the cache, which lets go of it in
// finish() and in its destructor.
class BitmapRowCache {
 public:
  BitmapRowCache() = default;
  ~BitmapRowCache();
  BitmapRowCache(const BitmapRowCache&) = delete;
  BitmapRowCache& operator=(const BitmapRowCache&) = delete;

  // Replays `path` into `bitmap` (parsed) when it holds this rendition, else records the next pass into it. False
  // when it can do neither (no fingerprint, no memory, the card refuses the file); the bitmap then decodes as before.
  bool attach(Bitmap& bitmap, const std::string& path);
  // Completes and publishes a recording (the rows a pass left unread are decoded now) and lets go of the bitmap.
  void finish();

  bool replaying() const { return state_ == State::Replaying; }
  bool recording() const { return state_ == State::Recording; }
  // True once after a replay read failed: the pass it served stopped short, and the bitmap decodes from now on.
  bool takeReplayFailure();

  // Bitmap's side.
  bool replayRow(uint8_t* row);        // the next row; false on a short read
  void recordRow(const uint8_t* row);  // the row a decode just made
  void rewind();                       // Bitmap::rewindToData(): the next pass starts at row 0
  void abandon();                      // a read the cache cannot follow: drop the recording or the replay

 private:
  enum class State : uint8_t { Off, Recording, Replaying };

  bool openReplay();
  bool flushRecorded();
  bool completeRecording();
  void finishOrDrop(bool complete);
  void stop();  // closes the file; a recording's partial file is removed

  Bitmap* bitmap_ = nullptr;
  std::string path_;
  FsFile file_;
  State state_ = State::Off;
  uint32_t fingerprint_ = 0;
  int width_ = 0;
  int height_ = 0;
  int rowBytes_ = 0;
  int bufferRows_ = 0;
  std::unique_ptr<uint8_t[]> buffer_;
  int buffered_ = 0;  // recording: rows waiting to be written; replay: rows read ahead
  int next_ = 0;      // recording: rows recorded so far; replay: rows handed out
  int taken_ = 0;     // replay: rows of the buffer already handed out
  bool replayFailed_ = false;
};
