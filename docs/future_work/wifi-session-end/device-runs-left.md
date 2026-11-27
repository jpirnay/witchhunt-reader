# Device runs not yet done for PR 1

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open.** PR 1 ran on the Picco, then on the X4 Pro and the Murphy (2026-11-27; the figures are in
[network-stack-findings.md](../../network-stack-findings.md)). Still to run, with the procedure from the topic page:

- **The T5S3**: every site once, with the session-end figures. It clears ghosts on every screen.
- **The OPDS browser and the File Transfer mode picker after a backed-out join** on a board other than the Picco.
  Neither the X4 Pro nor the Murphy has an OPDS server set up, and their mode picker hands off only to USB Drive,
  which takes the USB port and the serial link with it.
- **The KOReader sync on the Murphy**: it has no account.
- **The background sync busy at a screen's exit** on a board other than the Picco. Through the UI it did not
  reproduce on the X4 Pro: the worker's push (4-6 s) ended before any route through the screens reached a Wi-Fi
  screen's exit. A test needs a slower job or a direct way into the screen.
- **Sleep from a site** on a board other than the Picco: deep sleep stops USB, so someone has to press the button.
- **The C3s (X4; X3 runs are opt-in)**: every site still restarts, after a full radio teardown. The log line says
  whether the secondary framebuffer is held, which explains why a C3's free figure reads above its boot baseline.

**Next step:** record each run's figures in the findings doc's "Wi-Fi session end" sections, and delete this file
when the list is empty.
