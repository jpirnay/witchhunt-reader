#pragma once

// Whether the chapter build (Background-C) may run its slices while the refresh of the page it just drew is pending.
//
// displayBuildPage() starts that refresh, releases the render lock and waits for the waveform, so that the loop task
// can go on building: 1.7 s for the HALF a new chapter opens with on the X4. Two things stood in the way on a board
// whose card is on the panel's SPI bus. The wait held the bus lock (HalDisplay::completeDisplay), and the build reads
// the chapter from the card; and stepCurrentSectionBuild() therefore sat out every pending refresh. X4, 2026-11-16:
// 2,506 ms of layout took 4,282 ms, none of it between the trigger and the waveform's end.
//
// Where the panel leaves the bus alone between a refresh's start and its finish
// (HalDisplay::refreshLeavesBusToCard), the wait is made without the lock (waitRefreshOffBus) and the build runs on.
// Only for that one refresh: any other pending refresh is waited out with the bus held, and a slice started then
// would block on its first card read with the render lock in hand.
namespace BuildDuringRefresh {

// displayBuildPage(): wait for the waveform without the bus lock. Nothing is pending where the driver finished the
// refresh in the call that started it (a blocking driver, Dark Mode's blocking path): then there is nothing to wait
// for.
constexpr bool waitsOffBus(const bool refreshPending, const bool refreshLeavesBusToCard) {
  return refreshPending && refreshLeavesBusToCard;
}

struct Slice {
  bool refreshPending = false;
  // HalCapabilities::decodesThroughRefresh(): the card has its own bus and the build's arena is PSRAM.
  bool cardOffPanelBus = false;
  // The pending refresh is the build page's, and its wait holds no bus lock (waitsOffBus()).
  bool buildPageWaitOffBus = false;
};

// stepCurrentSectionBuild(): this slice waits for the refresh to finish.
constexpr bool sliceWaits(const Slice& s) { return s.refreshPending && !s.cardOffPanelBus && !s.buildPageWaitOffBus; }

}  // namespace BuildDuringRefresh
