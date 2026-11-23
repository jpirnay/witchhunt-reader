# The reader's priority scheme: what is left

Status: open items as of 2026-11-23. The scheme (complete page, then the page's own pictures, then the rest, input
above all), its rules R0-R8, what each panel controller needs from the framebuffers, and what shipped (#808, #811,
#813, #815-#819, #823) are in [../design/reader-priority-scheme.md](../design/reader-priority-scheme.md). The finding
numbers (V) and decisions (D) below are that record's; PRs cite them.

Bench for every check below: *The Odyssey*, BOOK IV, the 2.53 bench profile, n >= 3, AA off / two passes / One-Pass,
X3 and X4 first.

## The X4's resident build keeps the gray for the build's end (V1)

**Open.** On the X3 and on the X4's borrowed builds the page a chapter build draws gets its gray once its own refresh is
over (`BuildPageGray` `Pass::Redraw`, #823). An X4 build that keeps the secondary buffer (`IncrementalResident`, small
chapters) still gets `Pass::AtBuildEnd`: it runs on the heap its low-heap guard watches (`stepCurrentSectionBuild()`),
and the pass's page and glyph slots would come from that heap.

**Where.** `BuildPageGray::passFor()`, `EpubReaderActivity::buildPageGrayPass()`.

**Next step.** A place for the pass's page and glyph slots that is not the guarded heap. With the secondary buffer
resident, the S3s' `Pass::Restore` would then apply (inference, not tried).

**Related, One-Pass on the C3s.** A page drawn while a build holds the buffer is never one-pass: its gray comes as a
second, two-pass refresh about 0.65 s later (decision D1, option A). A true one-pass there needs the mask composed in
strips against the old frame still in the write buffer, and an SDK start that takes a staged mask (D1, option C):
possible by the controllers' needs, not measured.

## Build-through pages never get their gray (V3)

**Open.** On a chapter's last `BG_BUILD_THROUGH_PAGES` (3) pages Background-B keeps the borrowed buffer through the
turns (X3, X4). Those pages go out B/W, and nothing arms their gray when B gives the buffer back. X4, 2.53 bench: page
71 of 74 shown during the build-through with no AA line, build done 0.3 s later, next press 1.3 s after that.

**Where.** `EpubReaderActivity::recoverSecondaryBufferIfNeeded()` (keeps B's borrow through the turn),
`renderContents()` (AA gated on a resident buffer), `endBackgroundBorrow()` (re-arms nothing).

**Next step.** The `Pass::Redraw` treatment for a build-through page, whose build also holds the secondary buffer as
its arena; or, at the least, arm the gray for the page still on screen when B gives the buffer back.

## Look-ahead lends the frame on screen (V2 (ii), decision D5)

**Open.** Idle borrowers (Background-B, the image lane, the bookmark parse) take the secondary buffer, which holds the
frame on screen. After the lend `beginOnePassMask()` refuses, so the next turn takes two passes instead of one (X3,
X4). Since #817 a reader at a steady pace meets far fewer borrows (X3, a page every 2.5 s for 40 turns, One-Pass: 32
turns composed one-pass, 7 two-pass replays), but every borrow still costs the turn after it, and a finished
look-ahead build's redraw (see the page count below) takes two passes for the same reason. On the X4 a borrow between
two slow turns sent the next turn down the two-pass path once in 10 turns.

**Where.** SDK: `FreeInkDisplay::borrowSecondaryBuffer()` lends `frameBufferActive`;
`borrowSecondaryBufferDuringRefresh()` lends the other block only while a refresh is pending;
`returnSecondaryBuffer()` clears `_activeMatchesGlass`. Reader: `beginBackgroundBorrow()`, `warmPageForImageLane()`,
`stepStarredPages()` (`LentSecondary`).

**Next step (option (d) of the design record).** On the fork's `witchhunt`: a lend of the write buffer with no refresh
pending, and a return that keeps `_activeMatchesGlass` when nothing drew meanwhile. Then B, the lane and the bookmark
parse use it, and the reader drops the pre-render at the borrow and re-arms it after (D5: option A, the write buffer,
the pre-render redone in 60 ms; option B, today, the frame on screen). Risks: a draw outside `render()` while the
write buffer is lent; ghosting if "nothing drew" is wrong. Host test (R3): an SDK host runner where a lend returned untouched leaves `secondaryHoldsFrameOnScreen()` and
`beginOnePassMask()` working, and a lend that was drawn into does not. Check on the X3 and X4: one-pass on every FAST
turn at 2.5 s and at 15 s a page; no stale frame by screenshot and by eye.

## A page turn on an S3 board discards Background-B's live build

**Open.** `HalDisplay::hasSecondaryBuffer()` reports false while the PSRAM arena is lent, and every render that is not a
build-through ends B's borrow (`recoverSecondaryBufferIfNeeded()` -> `endBackgroundBorrow()`). The S3 boards have no
build-through (`BuildThroughGate::keepsFastRefresh()`), so a fast reader's next chapter is laid out on arrival. B's
build there holds nothing the page needs: the arena is PSRAM, and the frame on screen stays in the secondary buffer.
Not measured.

**Next step.** Keep B's build through page turns where the lend is a PSRAM arena, then measure a fast reader's chapter
crossings on the X4 Pro and the Picco.

## The X4: RED one frame behind after a deferred single-buffer FAST

**Open.** A blocking single-buffer FAST on the SSD1677 rewrites both RAM planes with the page after its waveform; a
deferred one does not (`Ssd1677Driver::displayImpl()` resyncs only when not async, `displayStart()`). While the
secondary buffer is away (a C build in single-buffer fast differential, a build-through), each further deferred FAST
leaves RED one frame behind: a second page drawn during a borrowed build diffs against RED two frames back (#813). The
`Pass::Redraw` cleanup writes the page to RED, so a page that gets its gray during the build is reseeded; with AA off
nothing is. By the code, not seen on a panel.

**Next step.** Reseed RED in the deferred finish of a single-buffer FAST (`Ssd1677Driver::displayFinish()`), on
`witchhunt`. Check: three FAST turns during a build with AA off, no ghost by eye.

## The X3 and the T5S3: the build idles through its page's refresh (V7)

**Open.** On the X4 the chapter build runs through the refresh of the page it drew (#813); on the Picco and the
Murphy their drivers defer (#811). On the X3 the slice still waits: the next build line came 0.85 s after the trigger
(20 of 20 in #813's logs). `HalDisplay::refreshLeavesBusToCard()` leaves the X3 out because its finish can read the
painted frame (`asyncRefreshHoldsFrameBuffer()`), and the T5S3 because a host task clocks its waveform out and card
traffic beside it has not been tried.

Since #860 the UC8253's finish reads the frame only after a full sync; the UC8279d X3's finish still sends it to DTM1
after every refresh (`Uc8279Driver::displayFinish()`). The X3 route would need that difference in the predicate
(inference from the driver code, not run). The X3's SectionBuilding popup blocks for the same reason (#819).

**Where.** `BuildDuringRefresh::sliceWaits()`, `HalDisplay::refreshLeavesBusToCard()`.

**Next step.** On a UC8253 X3: the off-bus wait for the build page's refresh, with a predicate that excludes the full
sync and the UC8279d; then a run with the section files' CRC32 compared with master's, as #813 did on the X4.

## The page count (V9 (b), (c), decision D3)

**Open.**
- **(b) A finished look-ahead build redraws the page on screen for its chapter count.** When the build firms up the
  count of a chapter that spans spine files, `stepBackgroundSectionBuild()` sets `statusRefreshDeferred_` and
  `endBackgroundBorrow()` asks for the redraw: a refresh, and a two-pass gray because B borrowed the buffer (see the
  look-ahead above), for one number. This is one of the two places that break rule R7.
- **(c) The first estimate is coarse.** `Section::estimatedTotalPages()` projects from `activeBuildPercent()`, an
  integer percent: `1/~7` for a chapter of 66 pages.

**Next step.** An estimate from the bytes consumed, never below the built count and exact at the end; the final count
drawn by the next refresh that happens anyway (D3, option A, recommended). Host test (R6): the estimate's bounds, and a
finished build that asks for no update when only the count changed. Check: the first page's count within 20 % of the
final one; no refresh between the build's end and the next turn.

Related: the unmerged branch `feat/chapter-count-backfill` (no PR) lays out a chapter's earlier files for its count.

## A page with an uncached picture decodes before it draws (V4, decision D2)

**Open.** A turn onto such a page decodes first and draws after: `renderContents()` warms the image caches before the
B/W render. A decode is 1-4 s, once 6.3 s. During a build and in the pre-render the page already comes first.

**Decide.** A: text and placeholder first, picture after (two refreshes). B: decode first (today). C: A only above a
size threshold. Recommended: C, its threshold from a measurement of decode times.

## Order slips on the loop task (V10)

**Open.** `loop()` writes the posted progress save (about 30 ms on the X3) and runs `serviceAutoSync()` (on a push
interval a chapter parse for the XPath) before `serviceBackgroundWork()` reaches `runDeferredGrayscalePass()`.
`stepGlyphStoreRoom()` and `stepStarredPages()` run before `stepBackgroundSectionBuild()`, so before the image lane
(rule R4). The lane waits `IMAGE_LANE_SETTLE_MS` (300 ms) and for the pre-render (`lookAheadMayRun()`). Only the save
was measured.

**Next step.** Lift the order of `serviceBackgroundWork()` into a pure function and host-test it for every combination
(gray owed, build live, picture on screen, look-ahead due: rules R1, R4); then move the steps that test shows out of
order.

## First window and partial sections (decision D6)

**Decide.** A: not now. B: design CrossPoint's model (lay out a window, build the rest as look-ahead) for chapters above
some length. Recommended: A, looked at again if long chapters still feel slow. Before the C3 gray during the build, the
complete page on a cold open took about 4.66 + 0.034 N s on the X3 and 4.56 + 0.033 N s on the X4 for N pages, about
15 s at 300. #823 measured a 66-page chapter only.

**Next step.** A cold open of a 300-page chapter on the X3: gray on the panel within about 0.5 s of the page, the
lowest free heap not below today's.

## A second core on the S3 boards (decision D10)

**Decide.** A: nothing now, one measurement. B: a decode worker for the page on screen. C: the chapter build on core 0.
Recommended: A; B only if the measurement shows seconds; not C. The analysis is in the design record, section 6.

**Next step.** On the X4 Pro, a picture page during a build (picture on screen at 5.2 s, chapter at 10 s): split the
time into decode CPU, card and panel (`Image lane: ... warmed in X ms` against the build's `parse done`), and measure
what a KOReader sync costs a turn. If the decode is a small part, drop B.

## `fastRefreshNeedsHostFrame()` answers for more boards than read `prev`

**Open.** The SDK answers true for every board of ours but the X3 (`_panelSel == PanelSel::X4`), though only the
SSD1677 and the Murphy's UC8253 read the host's previous frame. The reader asks it when it picks a build mode
(`chooseSectionBuildMode()`), arms a HALF after a popup, and gates build-through and B's RED seed
(`BuildThroughGate`). What it costs the X4 Pro, the Picco and the T5S3 is not measured.

**Next step.** Key it on the driver's own need (SDK), then check what the reader's callers do differently on those
three boards.

## Follow-ups named in PR bodies, not yet checked

From the PRs' own open points; not verified here.

- **#808:** not run on the X4 Pro, the T5S3 (single push in both AA modes) and the Murphy (no AA without
  `FREEINK_MURPHY_GRAY`). A page left during the build costs the build a pass in two-pass mode (kept: One-Pass is to
  become the default). Serial `CMD:BTN` cannot preempt a running pass; a real key at the plane gates was not tried.
- **#811:** still blocking: the two-pass overlay, a pre-rendered page turn (150 ms of power-off under the lock per turn
  on the Picco), the reader's Indexing popup at a crossing; on the Murphy also 0.35 s at each refresh's start (reset,
  init, planes). Not run: deep sleep with a refresh in flight, the gray sleep cover, the light-sleep guard on battery.
- **#817:** a reader settled at a pace between the one that beat the build and that plus 1.5 s gets no look-ahead
  before the chapter's last pages. Not run on the X4.
- **#818:** with a double-press action on a page key a press stays in flight for the 300 ms window after its release,
  so a turn during a build pauses it about 0.4 s (by code; the bench has those actions off). A real key's wait was not
  measured. Touch is not counted as a press in flight.
- **#819:** the X3's SectionBuilding popup still blocks (see the X3 and the T5S3 above). The T5S3's card traffic beside
  the LGFX waveform.
- **#823:** an eye check on the glass with the owner's settings: page 1 B/W, its gray about 0.7 s later while the
  chapter builds; no doubled or offset letters, a crisp status bar with no ghost digits, no residue on turns during and
  after the build. The abort path with a real key within the ~120 ms of planes. The X4's borrowed builds take the pass
  and were not run on an X4.
