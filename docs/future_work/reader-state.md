# Reader state

Status: open items as of 2026-10-21, from the reader-menu and page-save work (PRs #539, #540, #541).
None of them has been seen on a device yet; each was found by reading the code.

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

- Dictionary, Star Page and Show page as QR silently do nothing.
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
