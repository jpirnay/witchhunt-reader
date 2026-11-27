# Device runs not yet done for PR 1

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open.** PR 1 ran on the Picco only (the results are in
[network-stack-findings.md](../../network-stack-findings.md)). Still to run, with the procedure from the topic page:

- **The account check from the reader menu's KOReader setup** (#936, merged while PR 1 was in review). On a PSRAM
  board without a KOReader account, the reader menu's Sync tab opens KOReader settings over the book. Its account
  check now carries on with the book and the menu below it. Expect: back on KOReader settings, then the menu with
  the Sync tab rebuilt. Read the internal heap during the TLS request and at the session end. The row shows only
  without an account, so this needs a device with none set up.
- **Weather's city search**, the second Settings action. It shares `endRadioLeftOnBySettingAction()` with Wi-Fi
  Networks, which ran.
- **The X4 Pro, T5S3 and Murphy**: every site once, with the session-end figures. The Murphy and T5S3 clear ghosts
  on every screen. Check that the book's first page after a KOReader sync is clean on the X4 Pro, which, like the
  Picco, relies on the armed HALF.
- **The C3s (X4; X3 runs are opt-in)**: every site still restarts, after a full radio teardown. Collect the log
  lines, but read [c3-session-end-log-reads-high.md](c3-session-end-log-reads-high.md) first.

**Next step:** record each run's figures in the findings doc's "Wi-Fi session end" section, and delete this file
when the list is empty.
