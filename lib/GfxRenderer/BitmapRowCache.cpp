#include "BitmapRowCache.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "Bitmap.h"

namespace {

// 'B' 'R' 'C', then the layout's version; a u32 rendition fingerprint; u16 width, height and bytes per row; u16 0.
constexpr uint8_t MAGIC[4] = {'B', 'R', 'C', '1'};
constexpr size_t HEADER_BYTES = 16;
// Whole rows of it at a time: a 528 x 792 cover's 104 KB in 26 reads or writes instead of 792.
constexpr size_t BUFFER_BYTES = 4096;

void makeHeader(uint8_t (&out)[HEADER_BYTES], const uint32_t fingerprint, const int width, const int height,
                const int rowBytes) {
  memcpy(out, MAGIC, sizeof(MAGIC));
  for (int i = 0; i < 4; i++) out[4 + i] = static_cast<uint8_t>(fingerprint >> (8 * i));
  const uint16_t sizes[3] = {static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                             static_cast<uint16_t>(rowBytes)};
  for (int i = 0; i < 3; i++) {
    out[8 + 2 * i] = static_cast<uint8_t>(sizes[i]);
    out[9 + 2 * i] = static_cast<uint8_t>(sizes[i] >> 8);
  }
  out[14] = out[15] = 0;
}

std::string partPathFor(const std::string& path) { return path + ".part"; }

}  // namespace

BitmapRowCache::~BitmapRowCache() { finishOrDrop(false); }

bool BitmapRowCache::attach(Bitmap& bitmap, const std::string& path) {
  finishOrDrop(false);
  if (path.empty() || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) return false;
  uint32_t fingerprint = 0;
  if (!bitmap.renditionFingerprint(fingerprint)) {
    LOG_ERR("BRC", "No fingerprint for %s", path.c_str());
    return false;
  }
  rowBytes_ = (bitmap.getWidth() + 3) / 4;
  height_ = bitmap.getHeight();
  width_ = bitmap.getWidth();
  fingerprint_ = fingerprint;
  bufferRows_ = std::max(1, std::min(height_, static_cast<int>(BUFFER_BYTES) / rowBytes_));
  buffer_ = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(bufferRows_) * rowBytes_);
  if (!buffer_) {
    bufferRows_ = 1;
    buffer_ = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowBytes_));
  }
  if (!buffer_) {
    LOG_ERR("BRC", "No memory for a row cache buffer (%d bytes)", rowBytes_);
    return false;
  }
  path_ = path;

  if (openReplay()) {
    state_ = State::Replaying;
  } else {
    const std::string partPath = partPathFor(path_);
    uint8_t header[HEADER_BYTES];
    makeHeader(header, fingerprint_, width_, height_, rowBytes_);
    if (!Storage.openFileForWrite("BRC", partPath, file_) || file_.write(header, HEADER_BYTES) != HEADER_BYTES) {
      LOG_ERR("BRC", "Cannot write row cache: %s", partPath.c_str());
      if (file_.isOpen()) file_.close();
      Storage.remove(partPath.c_str());
      buffer_.reset();
      return false;
    }
    state_ = State::Recording;
    next_ = buffered_ = taken_ = 0;
  }
  // Row 0 next, with a fresh ditherer, whatever was read before; then the cache takes the rows.
  if (bitmap.rewindToData() != BmpReaderError::Ok) {
    LOG_ERR("BRC", "Cannot rewind %s", path_.c_str());
    stop();
    buffer_.reset();
    return false;
  }
  bitmap_ = &bitmap;
  bitmap.useRowCache(this);
  return true;
}

bool BitmapRowCache::openReplay() {
  if (!Storage.exists(path_.c_str())) return false;
  if (!Storage.openFileForRead("BRC", path_, file_)) return false;
  uint8_t expected[HEADER_BYTES];
  makeHeader(expected, fingerprint_, width_, height_, rowBytes_);
  uint8_t header[HEADER_BYTES];
  const bool matches = file_.size() == HEADER_BYTES + static_cast<size_t>(height_) * rowBytes_ &&
                       file_.read(header, HEADER_BYTES) == static_cast<int>(HEADER_BYTES) &&
                       memcmp(header, expected, HEADER_BYTES) == 0;
  if (!matches) {
    // Closed before it is removed: SdFat writes through a removed file's freed clusters.
    file_.close();
    LOG_INF("BRC", "Stale row cache, recording it again: %s", path_.c_str());
    Storage.remove(path_.c_str());
    return false;
  }
  next_ = buffered_ = taken_ = 0;
  return true;
}

bool BitmapRowCache::replayRow(uint8_t* row) {
  if (state_ != State::Replaying || row == nullptr) return false;
  if (taken_ == buffered_) {
    const int rows = std::min(bufferRows_, height_ - next_);
    const size_t bytes = static_cast<size_t>(rows > 0 ? rows : 0) * rowBytes_;
    if (rows <= 0 || file_.read(buffer_.get(), bytes) != static_cast<int>(bytes)) {
      LOG_ERR("BRC", "Row cache read failed at row %d, dropping it: %s", next_, path_.c_str());
      replayFailed_ = true;
      stop();
      Storage.remove(path_.c_str());  // after stop() closed it
      return false;
    }
    buffered_ = rows;
    taken_ = 0;
  }
  memcpy(row, buffer_.get() + static_cast<size_t>(taken_) * rowBytes_, rowBytes_);
  taken_++;
  next_++;
  return true;
}

void BitmapRowCache::recordRow(const uint8_t* row) {
  if (state_ != State::Recording || row == nullptr) return;
  if (next_ >= height_) {
    // A second pass that was never rewound: the file would no longer be the picture's rows in order.
    LOG_ERR("BRC", "More rows than the picture has, dropping the recording: %s", path_.c_str());
    stop();
    return;
  }
  memcpy(buffer_.get() + static_cast<size_t>(buffered_) * rowBytes_, row, rowBytes_);
  buffered_++;
  next_++;
  if (buffered_ == bufferRows_ && !flushRecorded()) stop();
}

bool BitmapRowCache::flushRecorded() {
  if (buffered_ == 0) return true;
  const size_t bytes = static_cast<size_t>(buffered_) * rowBytes_;
  if (file_.write(buffer_.get(), bytes) != bytes) {
    LOG_ERR("BRC", "Row cache write failed at row %d: %s", next_, path_.c_str());
    return false;
  }
  buffered_ = 0;
  return true;
}

bool BitmapRowCache::completeRecording() {
  // A pass reads rows only while a later one can still land on the screen (bitmap_rows::rowTarget()), so a picture cut
  // at its far edge leaves rows unread. They are decoded now, in order, which records them, so the file has them all.
  if (next_ < height_) {
    auto row = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowBytes_));
    auto raw = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(bitmap_->getRowBytes()));
    if (!row || !raw) {
      LOG_ERR("BRC", "No memory to finish the row cache: %s", path_.c_str());
      stop();
      return false;
    }
    while (state_ == State::Recording && next_ < height_) {
      if (bitmap_->readNextRow(row.get(), raw.get()) != BmpReaderError::Ok) {
        LOG_ERR("BRC", "Cannot read row %d to finish the row cache: %s", next_, path_.c_str());
        stop();
        return false;
      }
    }
    if (state_ != State::Recording) return false;  // a write failed on the way
  }
  if (!flushRecorded()) {
    stop();
    return false;
  }
  file_.close();
  state_ = State::Off;
  // FAT rename does not replace a file. attach() removed a stale cache of this name, but one it could not open is
  // still there: removed now, or every sleep would record a cache it can never publish.
  const std::string partPath = partPathFor(path_);
  if (!Storage.rename(partPath.c_str(), path_.c_str()) &&
      (!Storage.remove(path_.c_str()) || !Storage.rename(partPath.c_str(), path_.c_str()))) {
    LOG_ERR("BRC", "Cannot publish row cache: %s", path_.c_str());
    Storage.remove(partPath.c_str());
    return false;
  }
  LOG_DBG("BRC", "Row cache written: %s (%d rows of %d bytes)", path_.c_str(), height_, rowBytes_);
  return true;
}

void BitmapRowCache::rewind() {
  switch (state_) {
    case State::Recording:
      if (next_ == 0) return;  // nothing read yet: the pass to record is still to come
      if (completeRecording() && openReplay()) state_ = State::Replaying;
      return;
    case State::Replaying:
      if (!file_.seekSet(HEADER_BYTES)) {
        LOG_ERR("BRC", "Cannot rewind row cache, decoding instead: %s", path_.c_str());
        stop();
        return;
      }
      next_ = buffered_ = taken_ = 0;
      return;
    case State::Off:
      return;
  }
}

void BitmapRowCache::abandon() { stop(); }

bool BitmapRowCache::takeReplayFailure() {
  const bool failed = replayFailed_;
  replayFailed_ = false;
  return failed;
}

void BitmapRowCache::finish() { finishOrDrop(true); }

void BitmapRowCache::finishOrDrop(const bool complete) {
  if (complete && state_ == State::Recording && next_ > 0) completeRecording();
  stop();
  if (bitmap_) bitmap_->useRowCache(nullptr);
  bitmap_ = nullptr;
  buffer_.reset();
}

void BitmapRowCache::stop() {
  const State was = state_;
  state_ = State::Off;
  if (file_.isOpen()) file_.close();
  if (was == State::Recording) Storage.remove(partPathFor(path_).c_str());
}
