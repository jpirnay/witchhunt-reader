# Reader state

Status: open items as of 2026-11-23, from the reader-menu and page-save work (PRs #539, #540, #541), with bookmarks and
the link stack added after #785 and #787. The page-save items were found by reading the code and have not been seen on
a device.

## Mark as read can be overwritten by an older page save

`MARK_AS_READ` in `EpubReaderActivity::onReaderMenuConfirm()` writes the 100 % record while the
page drawn before the menu opened may still sit in `pendingProgressSave` (the menu was on top, so
`loop()` had not flushed it). If the reader comes back after the finished-book screen, `loop()`
writes that page over the 100 % record. The end-of-book Next in `loop()` can in principle do the same.

- Next step: drop the posted save (`pendingProgressSave.pending.exchange(false)`) where those two
  write the 100 % record, as the heap-recovery restart's live branch (`saveStateForRestart()`) already does.

## Menu actions after a layout change in the same visit

A real layout change in the reader menu (font, size, …) drops the section before `onReaderMenuConfirm()` runs the
chosen action: the menu's result applies `applyBookReaderOverrides()`, whose `relayoutWithChangedSettings()` ends in
`resetSectionKeepingPosition()`. Since #540 an untouched menu no longer does this, but after a real change:

- Dictionary, Bookmark Page (`STR_STAR_PAGE`) and Show page as QR silently do nothing
  (`openDictionary()` and `toggleStarOnCurrentPage()` return without a section).
- Go to % starts at 0 %; Go to printed page pre-fills the lowest label; Select Chapter preselects
  the spine's first entry.
- Where the change makes Publisher Fonts apply to a book not examined yet, the book is opened again and the action is
  dropped.
- Next step: take the position from `navTarget` (as `launchKOReaderSync()` does since #540), or
  run position-reading actions before the relayout.

## A sync while a chapter is still building sends the chapter start

A section dropped during an active build records no page count in `navTarget`
(`resetSectionKeepingPosition()` sets `cachedPageCount` only when no build is live), so the no-section fallback of
`launchKOReaderSync()` (`EpubReaderSync.cpp`) cannot use it and sends page 0/0.

- Next step: record the page count the build has reached, or defer the sync until the build ends.

## Front matter skipped after a first-open pull

`EpubReaderActivity::onEnter()` skips the front matter on a book's first open (no `progress.bin`) after
`applyPendingSyncSession()` has run, so a first open whose remote position is in spine 0 is moved to
the text start. Rare.

## `CMD:REBOOT` drops a posted page save

`CMD:REBOOT` → `silentRestart()` (`main.cpp`) skips `onExit()` and loses a save still posted. Test API only; a
device test should `IDLE` after a page turn before rebooting.

## Deleting a font family leaves the TXT/Markdown font pointing at it

`FontInstaller::deleteFamily` clears `sdFontFamilyName` but not `txtSdFontFamilyName`, so the TXT/Markdown reader's font
can name a family that is no longer on the card (`getTxtReaderFontId()` then falls back to the built-in font). Found
while mapping the Font Manager (`docs/design/cjk-ttf-fonts.md`, section 6.5).

- Next step: clear both, with a host test on the settings.

## Bookmarks after a relayout: what is left

Each bookmark (a "starred page" in the code: `BookmarkStore`, `StarAnchoring`) keeps the KOReader XPath of its page's
first text, that text's visible-text offset with the section format it was counted under, and the layout it was counted
in (`bookmarks.bin` v4, [../file-formats.md](../file-formats.md)). It moves onto its text in another layout, at idle
(`EpubReaderActivity::stepStarredPages()`, [../background-rendering.md](../background-rendering.md), *Starred pages*).
Left over:

- **Global Bookmarks shows old page numbers.** Its labels print the stored page (`GlobalBookmarksActivity`, the row
  label for a bookmark with no name). A bookmark moves only when its chapter is open in the reader
  (`StarAnchoring::settleKnown()`), so after a font or size change the list names the old page until that chapter has
  been visited. The jump itself lands right: it goes by the stored offset while its section format stands
  (`StarAnchoring::knownOffset()`), otherwise by the XPath (`starJumpTarget()`). Next step: show the chapter and
  position instead of a page number, or resolve the label when the list opens (a chapter parse per moved bookmark).
- **A rebuild in the same layout is not noticed.** A bookmark counts as standing when the section's layout
  fingerprint and its page count both match (`standsIn()` in `StarAnchoring.cpp`). A rebuild that keeps both but
  breaks its pages elsewhere leaves bookmarks on their old page numbers. Next step: compare the page's visible-text
  offset (`Section::getVisibleTextOffsetForPage`) with the stored one when the chapter loads.
- **The X3 has not run it.** The parse borrows the secondary framebuffer at idle (`LentSecondary` in
  `stepStarredPages()`); device results so far are Murphy and Picco. On the X3, check a large chapter's `STAR` parse
  time and the ghosting of the next page turn after a bookmark is named.

## Back after a TOC, bookmark or percent jump (CrossPoint #4239): decide

The link Back stack (`EpubLinkBackStack.h`, persisted in `linkstack.bin`) is pushed only when a link is followed
(`EpubReaderActivity::pushLinkOrigin()`) and is never cleared by a jump from the table of contents, a chapter skip,
Go to % or a bookmark. Back then still returns to an older link's origin. Choices: (a) keep that (today), (b) clear
the stack on such a jump (upstream), or (c) push the jump's origin, as KOReader's "go back" does. The return-on-backlink
part of the PR we have (`EpubLinkBackStack::returnDepth`). crosspoint-reader #4239, Erica Jensen (@hooligan333).
