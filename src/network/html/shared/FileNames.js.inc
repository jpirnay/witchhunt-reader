// --- names the browser hands the card (shared by the pages that upload or name files) ----------
// A Mac gives the page file names in NFD, Unicode's decomposed form ("e" followed by a combining
// accent), for files picked or dropped from Finder. FAT stores the name as it arrives, so the card
// then holds a name that differs from the NFC one ("é" as one character) that Windows, Linux and the
// reader's own keyboard write: Finder neither lists nor opens such a file on the card, and it matches
// no name typed elsewhere. Every name that comes from the browser (a picked or dropped file or
// folder, a typed name) therefore goes to the card through nfcName(). A name the card listed is
// sent back unchanged: normalized, it would no longer find the file.
//
// Ported from crosspoint-reader PR #3961 (Sung-jin Brian Hong, @serialx), which normalized the File
// Manager's uploads; one helper here for every page that sends such a name.
function nfcName(name) {
  // String.prototype.normalize is in every browser since 2016; without it the name goes as it is.
  return typeof name === 'string' && typeof name.normalize === 'function' ? name.normalize('NFC') : name;
}

// The body the reader's file routes take: `data` (a string, Blob or ArrayBuffer) as one multipart
// file, with its target in the URL. Not the bare bytes: the web server reads no query arguments for
// a raw body. Passed to fetch() without a Content-Type, which the browser sets with the boundary.
function uploadForm(data) {
  const form = new FormData();
  form.append('file', new Blob([data]), 'file');
  return form;
}
