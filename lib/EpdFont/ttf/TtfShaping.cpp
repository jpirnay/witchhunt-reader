// Ours (writing modes step 2).
#include "TtfShaping.h"

#include "GlyphStoreFormat.h"

namespace ttf {

bool shapingTables(const uint8_t* block, const uint32_t blockBytes, const uint32_t fileBytes, ot::FaceTables* out) {
  if (fileBytes > blockBytes) return false;
  if (!ot::tablesFromSfnt(block, fileBytes > 0 ? fileBytes : blockBytes, out)) return false;
  out->tables[ot::FaceTables::CPPL] = ot::Table();
  if (fileBytes == 0) return true;  // SD mode: the directory's own CPac record is the block's
  // Flash mode: the file's CPac, if it had one, is not ours; the block's is the room after the file.
  const glyphstore::CpacRoom room = glyphstore::cpacRoomOf(fileBytes, blockBytes);
  out->tables[ot::FaceTables::CPAC] = room.bytes > 0 ? ot::Table(block + room.at, room.bytes) : ot::Table();
  return true;
}

}  // namespace ttf
