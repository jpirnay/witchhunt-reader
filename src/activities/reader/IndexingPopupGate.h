#pragma once

#include <cstdint>

#include "BuildDuringRefresh.h"

// How an "Indexing" popup ships. The popup is feedback; the work it announces (the first open's indexing, a chapter
// build, a TXT or Markdown page index) is what produces the page, and it used to wait for the popup's refresh before it
// started: 0.63-0.66 s on a C3 first open (X3, X4, 2026-11-16), a FAST refresh on every chapter crossing elsewhere.
// Where the panel and the board allow it, the popup ships async (PopupShip::Async) and the work runs through its
// waveform. What each panel's refresh needs from the host, and for how long, is in
// docs/future_work/reader-priority-scheme.md, section 4.
//
// Two things decide it. The work reads the card: that must be possible while the refresh is pending
// (cardReadsDuringRefresh). And the first open and a chapter build lend the secondary buffer to the work; a lend of
// the framebuffer waits out a refresh in flight (FreeInkDisplay::borrowSecondaryBuffer), so where it would, the lend is
// taken without that wait (Lend).
//
// The decisions alone, apart from the reader, so the host can test them. The values come from the HAL
// (ReaderUtils::indexingPopupPanel).
namespace IndexingPopupGate {

struct Panel {
  // GfxRenderer::supportsAsyncRefresh(): the driver returns while the waveform runs, and the output is not inverted
  // (Dark Mode takes the blocking path in the SDK).
  bool asyncRefresh = false;
  // !HalCapabilities::sdUsesSpi(): the card has a bus of its own (X4 Pro, Picco, Murphy).
  bool cardOffPanelBus = false;
  // HalCapabilities::decodesThroughRefresh(): the loop task's chapter build runs through any pending refresh.
  bool decodesThroughRefresh = false;
  // GfxRenderer::refreshLeavesBusToCard(): the card shares the panel's SPI bus, which the panel needs only to start and
  // to finish a refresh (the X4).
  bool refreshLeavesBusToCard = false;
  // GfxRenderer::asyncRefreshHoldsFrameBuffer(): the X3, whose refresh's finish reads the painted frame back.
  bool refreshHoldsFrame = false;
  // GfxRenderer::lendsDuringRefresh(): the framebuffer lend (the C3s), which during a refresh takes the frame not being
  // painted.
  bool lendsDuringRefresh = false;
  // GfxRenderer::lendsPsramArena(): the lend is a block of PSRAM, which waits for nothing (the S3 boards).
  bool lendsPsramArena = false;
  // GfxRenderer::hasSecondaryBuffer(): two frame buffers, so a refresh's swap leaves the write buffer free (false
  // while the PSRAM arena is lent elsewhere).
  bool hasSecondaryBuffer = false;
};

// The card can be read between a refresh's start and its finish, by the flow that finishes it.
// - A card on a bus of its own.
// - The X4: its SSD1677 needs the shared bus only to start and to finish (HalDisplay::refreshLeavesBusToCard).
// - The X3: its controllers need the bus only to start and to finish as well. They are left out of
//   refreshLeavesBusToCard for the finish, which reads the frame back; the chapter build has read the card through its
//   Indexing popup's waveform since 193c1560c (X3, 2026-11-09: the chapter built 0.30-0.38 s sooner).
// Not the T5S3: a host task clocks its waveform out, and card traffic beside it has not been tried for a build
// (HalDisplay::refreshLeavesBusToCard).
constexpr bool cardReadsDuringRefresh(const Panel& p) {
  return p.cardOffPanelBus || p.refreshLeavesBusToCard || p.refreshHoldsFrame;
}

// When the secondary buffer is lent to the work behind the popup, and how the popup ships.
enum class Lend : uint8_t {
  // The popup's refresh is waited out, then the buffer is lent (RED seeded from the popup first).
  Blocking,
  // The popup ships async; the lend is the PSRAM arena, which touches no frame and waits for nothing.
  PsramArena,
  // The popup ships async; the lend takes the frame not being painted
  // (FreeInkDisplay::borrowSecondaryBufferDuringRefresh),
  // and the refresh's finish reads the painted one, which nothing draws over before it (the C3s). Where a FAST diffs
  // against a host frame (the X4), RED is seeded from the popup once it is finished, as the blocking order seeded it
  // before its lend: the popup's start took the frame before it into RED.
  DuringRefresh,
  // The popup ships async; nothing is lent: the build was handed over by the look-ahead (Background-B) with the block
  // it
  // holds already, and lays out in it. RED is seeded from the popup once it is finished, as after a lend.
  Held,
};

// The chapter build that starts with its popup (EpubReaderActivity::buildSection, a released build: a crossing into a
// chapter not laid out, a jump, a relayout). The kickoff slices on the render task until the waveform ends.
constexpr Lend chapterLend(const Panel& p) {
  if (!p.asyncRefresh || !cardReadsDuringRefresh(p)) return Lend::Blocking;
  if (!p.hasSecondaryBuffer) return Lend::Blocking;  // nothing to lend, or the PSRAM arena is out
  if (p.lendsPsramArena) return Lend::PsramArena;
  if (p.lendsDuringRefresh) return Lend::DuringRefresh;
  return Lend::Blocking;
}

// A chapter build adopted from the look-ahead with its block (a turn into the chapter Background-B was laying out):
// nothing to lend, so only the card decides.
constexpr Lend adoptedLend(const Panel& p) {
  return p.asyncRefresh && cardReadsDuringRefresh(p) ? Lend::Held : Lend::Blocking;
}

// The first open's indexing (ReaderActivity::onEnter). As the chapter, except that every PSRAM board indexes while the
// popup paints, the T5S3 included, as it has since #792 (by code; no T5S3 run).
constexpr Lend firstOpenLend(const Panel& p) {
  if (!p.asyncRefresh || !p.hasSecondaryBuffer) return Lend::Blocking;
  if (p.lendsPsramArena) return Lend::PsramArena;
  return chapterLend(p);
}

// The SectionBuilding pass's popup (the deferred popup of a resident build, a turn ahead of the build, a target the
// build has not reached). The build runs on the loop task there, so the popup ships async only where that task's slice
// may run through the popup's refresh while the render task waits for it with the lock let go
// (EpubReaderActivity::waitForBuildRefreshUnlocked, BuildDuringRefresh.h). Elsewhere the slice would wait anyway.
constexpr bool buildsThroughPopup(const Panel& p) {
  BuildDuringRefresh::Slice slice;
  slice.refreshPending = true;
  slice.cardOffPanelBus = p.decodesThroughRefresh;
  slice.buildPageWaitOffBus = BuildDuringRefresh::waitsOffBus(true, p.refreshLeavesBusToCard);
  return p.asyncRefresh && !BuildDuringRefresh::sliceWaits(slice);
}

// A TXT or Markdown page index, built on the render task under the popup with nothing lent. The page drawn after it
// goes into the write buffer, which the popup's swap left free, and its refresh finishes the popup's first.
constexpr bool indexesThroughPopup(const Panel& p) {
  return p.asyncRefresh && p.hasSecondaryBuffer && cardReadsDuringRefresh(p);
}

}  // namespace IndexingPopupGate
