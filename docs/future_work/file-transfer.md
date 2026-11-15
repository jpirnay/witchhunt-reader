# File transfer

Status: open items as of 2026-11-15, from the web-protection fixes of 2.52 and the upstream triage of that day.
The web file manager and WebDAV judge every path through `WebPathGuard` and `ProtectedPaths`
(`lib/FsHelpers/ProtectedPaths.h`); what a book's cache does when a file changes is in [book-cache.md](book-cache.md).

## USB Transfer applies no protected-path rule

**Open.** `SerialTransferDevice` (`src/SerialTransferDevice.cpp`) reads, writes, lists, renames and removes whatever
path the host names (`fileBegin` :165, `fileReadBegin` :237, `listDirBegin` :271, `renameFile` :302, `removeFile`
:342). It asks neither `ProtectedPaths` nor `WebPathGuard`, so the credential stores under `/.crosspoint` (`wifi.json`,
`opds.json`, `koreader.json`, `ProtectedPaths.h:51`) can be read and changed over the cable. Left as it is in 2.52:
it needs physical access, and the protocol runs only while the USB Transfer screen is open (`SerialTransferActivity`
owns the device).

**Next step.** Decide whether USB Transfer should refuse the protected paths as the web server does, at least the
credential stores. The host tools are MicroReader's (USER_GUIDE, "USB Transfer"): check how they show a refused path.

## Deleting full folders and hidden files from the web and WebDAV (matcha-reader #186): decide

**Open.** The web file manager refuses a folder that is not empty (`src/network/CrossPointWebServer.cpp:1940-1948`),
and WebDAV DELETE of one answers 409 (`src/network/WebDAVHandler.cpp:445-452`), though RFC 4918 makes DELETE of a
collection recursive. matcha deletes folders recursively and lets the file browsers delete dot entries.

**Next step.** A policy decision, together with crosspoint-reader #4031 (ProtectedPaths), which is waiting on the
maintainer: what a recursive delete may reach under `/.crosspoint` and other hidden folders. matcha-reader #186,
Eszter Schuffert (eszter007).
