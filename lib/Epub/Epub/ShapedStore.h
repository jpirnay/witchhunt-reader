#pragma once
// Ours (writing modes step 2, "How shaping runs" 5), not upstream's. Nothrow: every allocation is makeUniqueNoThrow or
// a vector growth behind a heap check; a refusal is a false return, never an abort.

#include <GlyphRun.h>
#include <IndicShaper.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The paragraph's shaped and reordered tokens (writing modes step 2, "How shaping runs" 5): per token, its pieces and
// their forms, in step with ParsedText's word vectors, valid for the text and key they were made from. The forms
// sit in chunks of kChunkBytes, from makeUniqueNoThrow (a piece larger than a chunk gets one of its own size); a
// piece's bytes never straddle two. Reserved from the paragraph's estimate; its chunks survive the paragraphs
// (rule 1) until release().
//
// Pieces are listed in the order they were written, which is the order their bytes lie in the chunks; an entry names
// its run of that list. The bytes of an erased or replaced entry stay as dead space until dropFront() or reset().
class ShapedStore final : public IndicShaper::PieceSink {
 public:
  struct Piece {
    uint16_t begin = 0, end = 0;  // bytes of the token's text
    IndicShaper::PieceKind kind = IndicShaper::PieceKind::Text;
    uint16_t width = 0;  // px, set by setWidth()
    uint16_t count = 0;  // glyph records (Glyphs)
    uint16_t bytes = 0;  // its form's bytes
    uint8_t chunk = 0;
    uint16_t at = 0;
  };
  static constexpr size_t kChunkBytes = 1024;
  // Past them a piece is refused (logged once at ERR) and the token is laid out as code points. Records take about 2 B
  // a byte of Indic text and about 3 B a byte of Arabic (readyShapedStore's estimates; an Arabic letter or mark is 2
  // bytes and one record of 4 B, 6 with an offset). A 96-token pass holds about 3-6 KB of Indic; of Arabic about 3 KB
  // in prose (10 B a token), up to about 6 KB fully vocalised (a mark on each letter, about 20 B a token). The cap
  // sits five times above it.
  static constexpr size_t kMaxChunks = 32;
  // Opens token `token`'s entry for (text, key); shapeToken then writes through the sink; commit() keeps the pieces
  // (an entry with none for a Plain token), abandon() drops them. `token` may be one past the last: a scratch entry,
  // for a candidate the breaker measures, which abandon() always ends. False when another entry is open or `token`
  // is further out.
  bool open(size_t token, std::string_view text, uint32_t key);
  void commit();
  void abandon();
  // Token `token`'s pieces when its entry was made for this text and key; nullopt otherwise (none, or stale). The span
  // and the views of glyphs() and form() are valid until the next call that changes the store.
  std::optional<std::span<const Piece>> pieces(size_t token, std::string_view text, uint32_t key) const;
  // The open entry's pieces so far (none when no entry is open): a scratch entry's are measured here before abandon().
  // Valid as pieces() is.
  std::span<const Piece> openPieces() const;
  // Token `token`'s pieces as committed, whatever text and key they were made for: the word cache copies an entry just
  // committed (WordShapeCache::insert). None when the entry is stale or holds none. Valid as pieces() is.
  std::span<const Piece> committedPieces(size_t token) const;
  // The entries, in step with the word vectors (the scratch entry is not one).
  size_t size() const { return entries_.size(); }
  void setWidth(size_t token, size_t piece, uint16_t width);
  GlyphRun glyphs(const Piece& piece) const;
  std::string_view form(const Piece& piece) const;
  // In step with the word vectors. insert() and reserve() grow nothrow behind a heap check, as
  // ParsedText::reserveWords does; false when refused.
  bool insert(size_t token);
  void erase(size_t token);
  void dropFront(size_t count);  // the consumed tokens' bytes are reclaimed; the kept tokens keep their pieces
  // Room for `tokens` more entries and for `bytes` more of forms (whole chunks, at most kMaxChunks held).
  bool reserve(size_t tokens, size_t bytes);
  void reset();               // empty, chunks kept
  void release();             // everything freed
  size_t chunkBytes() const;  // held, for tests

  // PieceSink: the open entry's pieces. False when it has no room (a chunk refused, kMaxChunks held, a form past
  // 64 KB, a record outside its fields); the caller abandons the entry.
  bool beginPiece(IndicShaper::PieceKind kind, uint16_t begin, uint16_t end) override;
  bool addGlyph(const GlyphRecord& record) override;
  bool addBytes(const char* bytes, size_t length) override;

 private:
  struct Entry {
    uint64_t hash = 0;
    uint32_t key = 0;
    uint32_t length = 0;  // the text's bytes
    uint32_t begin = 0;   // its run of pieces_
    uint32_t count = 0;
    bool valid = false;
  };
  struct Chunk {
    std::unique_ptr<uint8_t[]> data;
    uint16_t size = 0;
    uint16_t used = 0;
  };
  static constexpr uint8_t kDead = 0xFF;  // a piece being dropped by dropFront()

  bool makeRoom(size_t extra);
  size_t acquire(size_t size);
  bool allocChunk(Chunk& into, size_t size);
  void eraseRange(Entry& entry);
  void removeEmptyOversize();
  void recomputeCursor();
  bool oversize(size_t chunk) const { return chunks_[chunk].size > kChunkBytes; }
  template <typename T>
  static bool grow(std::vector<T>& v, size_t extra);
  static bool heapAllows(size_t bytes);

  std::vector<Piece> pieces_;
  std::vector<Entry> entries_;
  std::array<Chunk, kMaxChunks> chunks_;
  size_t nChunks_ = 0;
  size_t cur_ = 0;  // the chunk being filled: every chunk after it is empty

  bool pending_ = false;
  bool placed_ = false;  // the open piece has a place in a chunk
  size_t pendToken_ = 0;
  size_t pendStart_ = 0;  // where the open entry's pieces begin in pieces_
  uint64_t pendHash_ = 0;
  uint32_t pendKey_ = 0;
  uint32_t pendLength_ = 0;
  size_t savedCur_ = 0;
  uint16_t savedUsed_ = 0;
  bool refusalLogged_ = false;
};
