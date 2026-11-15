# Reader state

Status: open items as of 2026-10-21, from the reader-menu and page-save work (PRs #539, #540, #541);
bookmarks and the link stack added 2026-11-15. The 2026-10-21 items were found by reading the code and have not
been seen on a device.

## Mark as read can be overwritten by an older page save

`MARK_AS_READ` in `EpubReaderActivity::onReaderMenuConfirm()` writes the 100 % record while the
page drawn before the menu opened may still sit in `pendingProgressSave` (the menu was on top, so
`loop()` had not flushed it). If the reader comes back after the finished-book screen, `loop()`
writes that page over the 100 % record. The end-of-book Next can in principle do the same.

- Next step: drop the posted save (`pendingProgressSave.pending.exchange(false)`) where those two
  write the 100 % record, as the heap-recovery restart's live branch already does.

## Menu actions after a layout change in the same visit

A real layout change in the reader menu (font, size, …) drops the section
(`resetSectionKeepingPosition()`) before `onReaderMenuConfirm()` runs the chosen action. Since
#540 an untouched menu no longer does this, but after a real change:

- Dictionary, Bookmark Page (`STR_STAR_PAGE`) and Show page as QR silently do nothing
  (`EpubReaderActivity::toggleStarOnCurrentPage` returns without a section, `EpubReaderActivity.cpp:3329`).
- Go to % starts at 0 %; Go to printed page pre-fills the lowest label; Select Chapter preselects
  the spine's first entry.
- Next step: take the position from `navTarget` (as `launchKOReaderSync()` does since #540), or
  run position-reading actions before the relayout.

## A sync while a chapter is still building sends the chapter start

A section dropped during an active build records no page count in `navTarget`, so
`launchKOReaderSync()`'s no-section fallback cannot use it and sends page 0/0.

- Next step: record the page count the build has reached, or defer the sync until the build ends.

## Front matter skipped after a first-open pull

`EpubReaderActivity::onEnter()` skips the front matter on a book's first open after
`applyPendingSyncSession()` has run, so a first open whose remote position is in spine 0 is moved to
the text start. Rare.

## Dead `setReaderOverrides()` overloads

`RecentBooksStore`'s narrower `setReaderOverrides()` overloads still take the bionic override as
`bool` (the bug #540 fixed elsewhere) and are only called by each other. Remove them.

## `CMD:REBOOT` drops a posted page save

`CMD:REBOOT` → `silentRestart()` skips `onExit()` and loses a save still posted. Test API only; a
device test should `IDLE` after a page turn before rebooting.

## Deleting a font family leaves the TXT/Markdown font pointing at it

`FontInstaller::deleteFamily` clears `sdFontFamilyName` but not `txtSdFontFamilyName`, so the TXT/Markdown reader's font
can name a family that is no longer on the card. Found while mapping the Font Manager
(`docs/design/cjk-ttf-fonts.md`, section 6.5).

- Next step: clear both, with a host test on the settings.

## Bookmarks after a relayout: what is left

Since 2.52 each bookmark (a "starred page" in the code: `BookmarkStore`, `StarAnchoring`) keeps the KOReader XPath of
its page's first text and the layout it was counted in (`bookmarks.bin` v3; the text's offset is worked out in the
session and not stored), and moves onto its text in another layout
(`src/StarAnchoring.h`, run at idle by `EpubReaderActivity::stepStarredPages()`, `EpubReaderActivity.cpp:1393`).
Left over:

- **Global Bookmarks shows old page numbers.** Its labels print the stored page (`GlobalBookmarksActivity.cpp:112-115`).
  A bookmark moves only when its chapter is open in the reader (`StarAnchoring::settle`, `StarAnchoring.cpp:54-68`), so
  after a font or size change the list names the old page until that chapter has been visited. The jump itself lands
  right (it goes by the text offset when the session has one, otherwise by the XPath). Next step: show the chapter and position instead of a page
  number, or resolve the label when the list opens (a chapter parse per moved bookmark).
- **A rebuild in the same layout is not noticed.** A bookmark counts as standing when the section's layout
  fingerprint and its page count both match (`standsIn`, `StarAnchoring.cpp:49-51`). A rebuild that keeps both but
  breaks its pages elsewhere leaves bookmarks on their old page numbers. Next step: compare the page's visible-text
  offset (`Section::getVisibleTextOffsetForPage`) with the stored one when the chapter loads.
- **The X3 has not run it.** The parse borrows the secondary framebuffer at idle (`LentSecondary`,
  `EpubReaderActivity.cpp:1431`); device results so far are Murphy and Picco. On the X3, check a large chapter's
  `STAR` parse time and the ghosting of the next page turn after a bookmark is named.
- **Accepted: one slower page turn per relayout.** When a bookmark settles onto the page on screen, the reader redraws
  that page for the status-bar star (`EpubReaderActivity.cpp:1440-1446`). If that redraw has already started when a
  page turn is pressed, the turn waits for its refresh: +0.4-0.5 s (Picco, 2 of 4 trials), once per relayout and only
  when a bookmark lands on the page on screen. A redraw found while input waits is deferred instead
  (`starMarkerRedraw_`). Recorded so it is not taken for a new bug.

## Back after a TOC, bookmark or percent jump (CrossPoint #4239): decide

The link Back stack (`EpubLinkBackStack.h`, persisted in `linkstack.bin`) is pushed only when a link is followed
(`EpubLinkBackStack::push`, `EpubReaderActivity.cpp:7856`) and is never cleared by a jump from the table of contents,
a chapter skip, Go to % or a bookmark. Back then still returns to an older link's origin. Choices: (a) keep that
(today), (b) clear the stack on such a jump (upstream), or (c) push the jump's origin, as KOReader's "go back" does.
The return-on-backlink part of the PR we have (`EpubLinkBackStack::returnDepth`). crosspoint-reader #4239, Erica Jensen
(@hooligan333).
