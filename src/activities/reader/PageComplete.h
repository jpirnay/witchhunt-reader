#pragma once

// Whether the page the reader asked for is complete on the panel: its frame, and its gray where it gets one. It is
// what the serial CMD:IDLE:SCREEN waits for (EpubReaderActivity::hasPendingScreenWork()), and so what a bench times
// as "page complete".
//
// A chapter still being laid out (Background-C) is not part of it once its page is up: the rest of the build changes
// nothing on the panel, and a plain CMD:IDLE waits for it as background work. Counted as screen work, it made the
// answer the length of the chapter and left a script no way to press during a build. X3, 2026-11-16, a cold open in
// two passes: page on the panel at 4.16 s, chapter laid out at 6.85 s, idle answered at 7.68 s, and the "turn during
// the build" that followed it was sent after the build.
//
// The decision alone, apart from the reader, so the host can test it.
namespace PageComplete {

// The page a live chapter build has put on the panel (displayBuildPage()).
struct BuildPage {
  bool shown = false;      // it is the page the reader is on; false under the Indexing popup, or after a turn
  bool hasImages = false;  // drawn again once the build is done, with its pictures
  bool grayOwed = false;   // anti-aliasing is on for it and its gray is not on the panel yet
};

struct Reader {
  bool sectionLoaded = false;
  bool buildLive = false;           // Background-C is laying the section out
  bool reading = false;             // not in a blocking build
  bool turnHandedToRender = false;  // a pre-rendered page is on its way to the panel
  bool grayPassOwed = false;        // a deferred gray pass is armed
  BuildPage buildPage;              // read only while buildLive
};

// The page a live build shows is complete: nothing more comes to the panel for it until the reader moves.
constexpr bool buildPageComplete(const BuildPage& page) { return page.shown && !page.hasImages && !page.grayOwed; }

// The panel is about to change without input.
constexpr bool screenWorkPending(const Reader& reader) {
  if (!reader.sectionLoaded || reader.turnHandedToRender || reader.grayPassOwed) return true;
  return reader.buildLive ? !buildPageComplete(reader.buildPage) : !reader.reading;
}

}  // namespace PageComplete
