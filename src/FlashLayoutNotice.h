#pragma once

#include <cstdint>
#include <string>

/// The notice of emergency mode, shown once on the way to Home: the board kept another firmware's partition table, its
/// flash area is missing or too small (FlashArea::asExpected()), and the firmware runs on the fallbacks in
/// docs/design/flash-area.md. It urges a full install from the web flasher, which writes our table; a board that
/// cannot be flashed over USB keeps working as it is. Shown again only for another layout.
namespace FlashLayoutNotice {

/// The web flasher, whose Full install writes our partition table.
constexpr char kFlasherUrl[] = "https://jpirnay.github.io/witchhunt-reader/";
/// The flasher as it is typed: without the scheme.
constexpr const char* kFlasherAddress = kFlasherUrl + 8;
static_assert(kFlasherUrl[7] == '/' && kFlasherUrl[8] != '/', "the address follows https://");

/// What APP_STATE.flashLayoutNoticeFor keeps of a layout: the table's `spiffs` partition in KB plus 1 (1: none), so 0
/// stays "never shown".
constexpr uint32_t layoutMark(const uint32_t partitionBytes) { return partitionBytes / 1024 + 1; }

/// Whether a notice is due: in emergency mode, and not yet shown for this layout.
constexpr bool dueFor(const bool asExpected, const uint32_t partitionBytes, const uint32_t shownFor) {
  return !asExpected && shownFor != layoutMark(partitionBytes);
}

/// dueFor() of this board and APP_STATE.
bool due();

/// The notice was read: APP_STATE remembers the layout, saved to the card.
void markShown();

/// "3.4 MB", "80 KB": a partition's size as the notice names it.
std::string sizeText(uint32_t bytes);

/// The notice's first paragraph, in the interface language: what is wrong with this board's layout.
std::string whatIsWrong();

}  // namespace FlashLayoutNotice
