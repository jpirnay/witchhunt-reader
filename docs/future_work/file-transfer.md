# File transfer

Status: open items from the web-protection fixes of 2.52 (#781) and the upstream triage of the same day.
The web file manager and WebDAV judge every path through `WebPathGuard` and `ProtectedPaths`
(`lib/FsHelpers/ProtectedPaths.h`; the rules are in `docs/webserver-endpoints.md`, "Protected paths");
what a book's cache does when a file changes is in [book-cache.md](book-cache.md).

## USB Transfer applies no protected-path rule

**Open.** `SerialTransferDevice` (`src/SerialTransferDevice.cpp`) reads, writes, lists, renames and removes
whatever path the host names (`fileBegin`, `fileReadBegin`, `listDirBegin`, `renameFile`, `removeFile`).
It asks neither `ProtectedPaths` nor `WebPathGuard`, so the credential stores under `/.crosspoint`
(`wifi.json`, `opds.json`, `koreader.json`: `ProtectedPaths::CREDENTIAL_FILES`) can be read and changed
over the cable. Left as it is: it needs physical access, and the protocol runs only while the USB Transfer
screen is open (`SerialTransferActivity` owns the device).

**Next step.** Decide whether USB Transfer should refuse the protected paths as the web server does, at least the
credential stores. The host tools are MicroReader's (USER_GUIDE, "USB Transfer"): check how they show a refused path.

## Deleting full folders and hidden files from the web and WebDAV (matcha-reader #186): decide

**Open.** The web file manager refuses a folder that is not empty (`CrossPointWebServer::handleDelete`,
"folder not empty"), and WebDAV DELETE of one answers 409 (`WebDAVHandler::handleDelete`), though RFC 4918
makes DELETE of a collection recursive. An item whose own name starts with a dot cannot be deleted from
either, with Show Hidden Files on or off (`ProtectedPaths::isProtectedPath`, `Target::Item`). matcha deletes
folders recursively and lets the file browsers delete dot entries.

**Options.**
- Port crosspoint-reader #4031 (merged upstream): the web Delete stops refusing an item because its name
  starts with a dot; the credential stores, the system folders, the root and folders that are not empty
  stay refused. No recursive delete.
- Also allow a recursive delete, as matcha and RFC 4918 do; then decide what it may reach under
  `/.crosspoint` and other hidden folders.
- Leave both as they are.

matcha-reader #186, Eszter Schuffert (eszter007).
