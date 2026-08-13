// Metadata Editor - edits a book's title, author, language, series, series
// index and description, and saves them to a "book.opf" sidecar.
//
// It never rewrites the EPUB. The firmware prefers a sidecar over the metadata
// embedded in the book (docs/sidecar-files.md), so writing one is enough to
// change what the reader displays - without a multi-megabyte round trip, without
// any risk of corrupting the book, and reversible by deleting one small file.
//
// An existing sidecar is edited in place as XML rather than regenerated, so
// everything this editor does not manage - publisher, identifiers, dates, the
// Calibre UUID, guide entries - survives untouched. Only when there is no
// sidecar at all is a fresh minimal one created.
//
// Reading order matches the firmware's: an existing book.opf wins; otherwise
// the book's own OPF is read via JSZip, which costs a full download.
//
// Everything is inside this closure: plugins share the page's global scope.
CrossPoint.registerPlugin((container, api) => {
  const DC_NS = 'http://purl.org/dc/elements/1.1/';
  const OPF_NS = 'http://www.idpf.org/2007/opf';

  // name -> how it is stored in an OPF. "dc" elements carry their value as text;
  // "meta" fields are Calibre's <meta name=... content=.../> form.
  const FIELDS = [
    { key: 'title', label: 'Title', kind: 'dc', tag: 'title' },
    { key: 'author', label: 'Author', kind: 'dc', tag: 'creator' },
    { key: 'language', label: 'Language', kind: 'dc', tag: 'language', hint: 'e.g. en, de' },
    { key: 'series', label: 'Series', kind: 'meta', metaName: 'calibre:series' },
    { key: 'seriesIndex', label: 'Series index', kind: 'meta', metaName: 'calibre:series_index' },
    { key: 'description', label: 'Description', kind: 'dc', tag: 'description', multiline: true }
  ];

  container.innerHTML =
    '<h2>' + esc(api.title) + '</h2>' +
    '<p>Edits a book\'s metadata and saves it as a <code>book.opf</code> sidecar beside it. ' +
    'The book itself is never modified. Clearing a field removes it from the sidecar, so the ' +
    'book\'s own value shows again.</p>' +
    '<p><label>Folder <input type="text" id="me-dir" value="' + esc(api.currentPath) +
    '" style="width:16em"></label> ' +
    '<button id="me-scan">Scan</button> <span id="me-status"></span></p>' +
    '<div id="me-list"></div>' +
    '<div id="me-form"></div>';

  const el = (id) => container.querySelector(id);
  const status = (t) => { el('#me-status').textContent = t; };
  let dir = api.currentPath;
  let books = [];
  let editing = null;   // { name, base, hasSidecar }
  let sourceDoc = null; // parsed existing sidecar, or null when creating fresh

  el('#me-scan').onclick = async () => {
    dir = normalize(el('#me-dir').value.trim() || '/');
    el('#me-form').textContent = '';
    el('#me-list').textContent = '';
    editing = null;
    try {
      status('Listing ' + dir + '...');
      const entries = await getJson('/api/files?path=' + encodeURIComponent(dir));
      const names = new Set(entries.filter((e) => !e.isDirectory).map((e) => e.name));
      books = entries.filter((e) => !e.isDirectory && e.isEpub)
        .map((e) => ({ name: e.name, base: stripExt(e.name), hasSidecar: names.has(stripExt(e.name) + '.opf') }));
      renderList();
      status(books.length + ' book(s)');
    } catch (e) {
      status('Failed: ' + msg(e));
    }
  };

  function renderList() {
    if (!books.length) { el('#me-list').innerHTML = '<p>No EPUBs directly in this folder.</p>'; return; }
    let html = '<div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse">' +
      '<tr><th align="left">Book</th><th align="left">Sidecar</th><th></th></tr>';
    books.forEach((b, i) => {
      html += '<tr style="border-top:1px solid rgba(128,128,128,.35)">' +
        '<td>' + esc(b.name) + '</td>' +
        '<td>' + (b.hasSidecar ? 'book.opf' : '<em>none yet</em>') + '</td>' +
        '<td><button data-i="' + i + '" class="me-edit">Edit</button></td></tr>';
    });
    el('#me-list').innerHTML = html + '</table></div>';
    container.querySelectorAll('.me-edit').forEach((btn) => {
      btn.onclick = () => openEditor(books[Number(btn.dataset.i)]);
    });
  }

  async function openEditor(book) {
    editing = book;
    sourceDoc = null;
    el('#me-form').innerHTML = '<p>Loading metadata...</p>';
    let values;
    try {
      if (book.hasSidecar) {
        status('Reading ' + book.base + '.opf...');
        sourceDoc = xml(await getText(dl(join(dir, book.base + '.opf'))));
        values = readFrom(sourceDoc);
      } else {
        status('No sidecar - downloading ' + book.name + ' to read its metadata...');
        values = readFrom(xml(await opfFromEpub(join(dir, book.name))));
      }
    } catch (e) {
      el('#me-form').innerHTML = '<p class="plugin-error">Could not read metadata: ' + esc(msg(e)) + '</p>';
      status('');
      return;
    }
    renderForm(book, values);
    status('');
  }

  function renderForm(book, values) {
    let html = '<h3>' + esc(book.name) + '</h3>';
    for (const f of FIELDS) {
      const v = esc(values[f.key] || '');
      html += '<p><label style="display:block"><strong>' + esc(f.label) + '</strong>' +
        (f.hint ? ' <span style="opacity:.7">' + esc(f.hint) + '</span>' : '') + '<br>' +
        (f.multiline
          ? '<textarea id="me-f-' + f.key + '" rows="4" style="width:100%">' + v + '</textarea>'
          : '<input type="text" id="me-f-' + f.key + '" value="' + v + '" style="width:100%">') +
        '</label></p>';
    }
    html += '<p><button id="me-save">Save sidecar</button> ' +
      '<button id="me-cancel">Cancel</button> <span id="me-saved"></span></p>' +
      '<hr><div id="me-cover"></div>';
    el('#me-form').innerHTML = html;
    el('#me-cancel').onclick = () => { el('#me-form').textContent = ''; editing = null; };
    el('#me-save').onclick = save;
    renderCover(book, values);
  }

  // --- cover sidecar --------------------------------------------------------
  // A cover beside the book overrides the one embedded in it, exactly like the
  // metadata sidecar, and is written the same way: the book is never touched.
  //
  // No image is ever fetched cross-origin. A page served from the device cannot
  // read a response from goodreads.com or books.google.com - they send no CORS
  // headers, and drawing such an image to a canvas taints it, so its bytes could
  // never be saved anyway. Hence the three sources here: a local file, the
  // clipboard (which is how you take a cover from any site at all - right-click
  // the image, Copy image, paste here), and Open Library, which does serve both
  // its search API and its covers with Access-Control-Allow-Origin.
  function renderCover(book, values) {
    const host = el('#me-cover');
    host.innerHTML =
      '<h3>Cover</h3>' +
      '<p id="me-cover-state">Checking...</p>' +
      '<div id="me-cover-preview"></div>' +
      '<p><label>Replace from file <input type="file" id="me-cover-file" accept="image/*"></label></p>' +
      '<p id="me-paste" tabindex="0" style="border:1px dashed rgba(128,128,128,.6);padding:10px">' +
      'Click here and press Ctrl+V to paste a copied image. Works with any site: copy the ' +
      'image in your browser, then paste it here.</p>' +
      '<p><label>Search <select id="me-source">' +
      '<option value="openlibrary">Open Library</option>' +
      '<option value="goodreads">Goodreads</option>' +
      '<option value="google">Google Books</option>' +
      '</select></label> <button id="me-search">Find covers</button> ' +
      '<button id="me-remove">Remove cover</button> <span id="me-cover-msg"></span></p>' +
      '<div id="me-results"></div>';

    refreshCoverState();

    el('#me-cover-file').onchange = async (e) => {
      const file = e.target.files && e.target.files[0];
      if (file) await installCover(file, file.name);
    };

    el('#me-paste').onpaste = async (e) => {
      const items = (e.clipboardData && e.clipboardData.items) || [];
      for (const item of items) {
        if (item.type && item.type.indexOf('image/') === 0) {
          e.preventDefault();
          await installCover(item.getAsFile(), 'pasted.' + item.type.split('/')[1]);
          return;
        }
      }
      coverMsg('No image found in the clipboard.');
    };

    el('#me-remove').onclick = removeCover;
    el('#me-search').onclick = () => searchCovers(el('#me-source').value, values);
  }

  function coverMsg(t) {
    const m = el('#me-cover-msg');
    if (m) m.textContent = t;
  }

  // Must stay in step with SidecarFiles::kCoverExtensions in the firmware: the
  // device resolves the cover by this order, so writing a .jpg while an older
  // .png remains would be decided by the list rather than by what you chose.
  const COVER_EXTS = ['.jpg', '.jpeg', '.png', '.bmp', '.JPG', '.JPEG', '.PNG', '.BMP'];

  async function currentCoverName() {
    const entries = await getJson('/api/files?path=' + encodeURIComponent(dir));
    const names = new Set(entries.filter((e) => !e.isDirectory).map((e) => e.name));
    for (const ext of COVER_EXTS) {
      if (names.has(editing.base + ext)) return editing.base + ext;
    }
    return null;
  }

  async function refreshCoverState() {
    try {
      const name = await currentCoverName();
      el('#me-cover-state').textContent = name
        ? 'Sidecar cover: ' + name
        : 'No cover sidecar - the cover inside the book is used.';
      el('#me-cover-preview').innerHTML = name
        // Cache-busted: the filename stays the same when the image changes.
        ? '<img alt="cover" style="max-height:160px" src="' + dl(join(dir, name)) + '&t=' + Date.now() + '">'
        : '';
    } catch (e) {
      el('#me-cover-state').textContent = 'Could not read the folder: ' + msg(e);
    }
  }

  async function installCover(blob, sourceName) {
    if (!blob) return;
    const ext = coverExtFor(blob.type, sourceName);
    if (!ext) {
      coverMsg('Unsupported image type - use JPEG, PNG or BMP.');
      return;
    }
    coverMsg('Saving...');
    try {
      // Drop any other variant first, so exactly one cover sidecar exists.
      const existing = await currentCoverName();
      if (existing && existing !== editing.base + ext) await del(join(dir, existing));

      const fd = new FormData();
      fd.append('file', blob, editing.base + ext);
      const r = await fetch('/upload?path=' + encodeURIComponent(dir), { method: 'POST', body: fd });
      if (!r.ok) throw new Error('upload ' + r.status);
      coverMsg('Saved ' + editing.base + ext);
      await refreshCoverState();
    } catch (e) {
      coverMsg('Failed: ' + msg(e));
    }
  }

  async function removeCover() {
    coverMsg('');
    try {
      const name = await currentCoverName();
      if (!name) {
        coverMsg('There is no cover sidecar to remove.');
        return;
      }
      if (!confirm('Delete ' + name + '? The cover inside the book will be used again.')) return;
      await del(join(dir, name));
      coverMsg('Removed ' + name);
      await refreshCoverState();
    } catch (e) {
      coverMsg('Failed: ' + msg(e));
    }
  }

  function coverExtFor(mime, name) {
    const m = (mime || '').toLowerCase();
    if (m === 'image/jpeg' || m === 'image/jpg') return '.jpg';
    if (m === 'image/png') return '.png';
    if (m === 'image/bmp' || m === 'image/x-ms-bmp') return '.bmp';
    const n = (name || '').toLowerCase();
    if (n.endsWith('.jpg') || n.endsWith('.jpeg')) return '.jpg';
    if (n.endsWith('.png')) return '.png';
    if (n.endsWith('.bmp')) return '.bmp';
    return '';
  }

  // Covers come from three sources with different constraints.
  //
  // Open Library serves both its search API and its images with CORS, so it is
  // fetched directly and never troubles the device.
  //
  // Goodreads has no public API - it was retired in 2020 - so its search page is
  // scraped, which means this breaks if they change their markup. Google Books
  // does have an API and it does send CORS, so the search is direct; only its
  // images are on books.google.com, which does not.
  //
  // For both of those the relay is used for ONE thing: reading the bytes of the
  // cover actually chosen. Previews are plain <img> tags pointing straight at the
  // remote thumbnail, because displaying a cross-origin image was never
  // restricted - only reading its pixels is. So the device does work for the one
  // cover you pick, not for the whole grid.
  const SOURCES = {
    openlibrary: { label: 'Open Library', search: searchOpenLibrary, direct: true },
    goodreads: { label: 'Goodreads', search: searchGoodreads, direct: false },
    google: { label: 'Google Books', search: searchGoogleBooks, direct: false }
  };

  async function searchCovers(sourceKey, values) {
    const source = SOURCES[sourceKey] || SOURCES.openlibrary;
    const results = el('#me-results');
    const title = (el('#me-f-title').value || values.title || '').trim();
    const author = (el('#me-f-author').value || values.author || '').trim();
    if (!title) {
      coverMsg('Enter a title first.');
      return;
    }

    coverMsg('Searching ' + source.label + '...');
    results.textContent = '';
    try {
      const hits = await source.search(title, author);
      if (!hits.length) {
        coverMsg('No covers found on ' + source.label + '.');
        return;
      }
      renderHits(hits, source);
    } catch (e) {
      coverMsg(source.label + ' search failed: ' + msg(e));
    }
  }

  // hit = { thumb, full, label, direct }
  function renderHits(hits, source) {
    const results = el('#me-results');
    coverMsg(hits.length + ' result(s) - click one to use it');
    results.innerHTML = hits.map((h, i) =>
      '<figure style="display:inline-block;margin:4px;text-align:center;width:110px">' +
      '<img data-i="' + i + '" class="me-hit" style="max-height:140px;cursor:pointer"' +
      ' src="' + h.thumb + '" alt="" loading="lazy">' +
      '<figcaption style="font-size:.8em">' + esc(h.label || '') + '</figcaption>' +
      '</figure>').join('');

    results.querySelectorAll('.me-hit').forEach((img) => {
      img.onclick = async () => {
        const hit = hits[Number(img.dataset.i)];
        coverMsg('Fetching cover...');
        try {
          // Direct where the host allows it; through the device where it does not.
          const resp = source.direct ? await fetch(hit.full) : await api.relay(hit.full);
          if (!resp.ok) throw new Error('cover ' + resp.status);
          await installCover(await resp.blob(), 'cover.jpg');
          results.textContent = '';
        } catch (e) {
          coverMsg('Could not fetch that cover: ' + msg(e));
        }
      };
    });
  }

  async function searchOpenLibrary(title, author) {
    const url = 'https://openlibrary.org/search.json?limit=8&fields=title,author_name,cover_i' +
      '&title=' + encodeURIComponent(title) +
      (author ? '&author=' + encodeURIComponent(author) : '');
    const data = await (await fetch(url)).json();
    return (data.docs || []).filter((d) => d.cover_i).map((d) => ({
      thumb: 'https://covers.openlibrary.org/b/id/' + d.cover_i + '-M.jpg',
      full: 'https://covers.openlibrary.org/b/id/' + d.cover_i + '-L.jpg',
      label: (d.author_name || []).join(', ')
    }));
  }

  async function searchGoodreads(title, author) {
    const query = title + (author ? ' ' + author : '');
    const html = await (await api.relay(
      'https://www.goodreads.com/search?q=' + encodeURIComponent(query))).text();
    const doc = new DOMParser().parseFromString(html, 'text/html');

    return Array.from(doc.querySelectorAll('img.bookCover')).slice(0, 8).map((img) => {
      const thumb = img.getAttribute('src') || '';
      // Search results are ~50px wide. Goodreads encodes the size as a "._SX50_"
      // segment before the extension; dropping it yields the full-size cover
      // (verified: 1.8KB thumbnail vs 266KB full).
      const full = thumb.replace(/\._S[XY]\d+_(?=\.[a-z]+$)/i, '');
      return { thumb, full, label: (img.getAttribute('alt') || '').trim() };
    }).filter((h) => h.thumb);
  }

  async function searchGoogleBooks(title, author) {
    const q = 'intitle:' + title + (author ? ' inauthor:' + author : '');
    const url = 'https://www.googleapis.com/books/v1/volumes?maxResults=8&q=' + encodeURIComponent(q);
    const data = await (await fetch(url)).json();
    return (data.items || []).map((item) => {
      const info = item.volumeInfo || {};
      const links = info.imageLinks || {};
      const thumb = links.thumbnail || links.smallThumbnail;
      if (!thumb) return null;
      // Served over http in the payload, and edge=curl draws a page-curl effect
      // that has no business on a cover.
      const clean = thumb.replace(/^http:/, 'https:').replace(/&edge=curl/, '');
      return { thumb: clean, full: clean, label: (info.authors || []).join(', ') };
    }).filter(Boolean);
  }

  async function del(path) {
    const fd = new FormData();
    fd.append('path', path);
    fd.append('type', 'file');
    const r = await fetch('/delete', { method: 'POST', body: fd });
    if (!r.ok) throw new Error('delete ' + r.status + ' ' + (await r.text().catch(() => '')));
  }

  async function save() {
    if (!editing) return;
    const values = {};
    for (const f of FIELDS) values[f.key] = el('#me-f-' + f.key).value.trim();

    el('#me-save').disabled = true;
    el('#me-saved').textContent = 'Saving...';
    try {
      // Edit the existing sidecar so unmanaged fields survive; only build a new
      // document when there is nothing to preserve.
      const doc = sourceDoc || freshDoc();
      writeInto(doc, values);
      const serialized = '<?xml version="1.0" encoding="utf-8"?>\n' +
        new XMLSerializer().serializeToString(doc.documentElement) + '\n';
      await upload(editing.base + '.opf', serialized);

      editing.hasSidecar = true;
      sourceDoc = doc;
      renderList();
      el('#me-saved').textContent = 'Saved ' + editing.base + '.opf';
    } catch (e) {
      el('#me-saved').textContent = 'Failed: ' + msg(e);
    } finally {
      el('#me-save').disabled = false;
    }
  }

  // --- OPF read/write -------------------------------------------------------
  function readFrom(doc) {
    const out = {};
    for (const f of FIELDS) {
      if (f.kind === 'dc') {
        const n = dcElement(doc, f.tag);
        out[f.key] = n ? (n.textContent || '').trim() : '';
      } else {
        const n = metaElement(doc, f.metaName);
        out[f.key] = n ? (n.getAttribute('content') || '').trim() : '';
      }
    }
    return out;
  }

  function writeInto(doc, values) {
    const metadata = metadataElement(doc);
    for (const f of FIELDS) {
      const value = values[f.key];
      if (f.kind === 'dc') {
        let n = dcElement(doc, f.tag);
        if (!value) { if (n && n.parentNode) n.parentNode.removeChild(n); continue; }
        if (!n) {
          n = doc.createElementNS(DC_NS, 'dc:' + f.tag);
          metadata.appendChild(n);
        }
        n.textContent = value;
      } else {
        let n = metaElement(doc, f.metaName);
        if (!value) { if (n && n.parentNode) n.parentNode.removeChild(n); continue; }
        if (!n) {
          n = doc.createElementNS(OPF_NS, 'meta');
          n.setAttribute('name', f.metaName);
          metadata.appendChild(n);
        }
        n.setAttribute('content', value);
      }
    }
    // The firmware also accepts the EPUB 3 spelling of series. Leaving a stale
    // one behind would give two sources of truth whose winner depends on
    // document order, so drop them once we manage the Calibre pair.
    for (const prop of ['belongs-to-collection', 'group-position']) {
      Array.from(doc.getElementsByTagName('meta'))
        .filter((m) => m.getAttribute('property') === prop)
        .forEach((m) => m.parentNode && m.parentNode.removeChild(m));
    }
  }

  function metadataElement(doc) {
    const found = doc.getElementsByTagName('metadata')[0] ||
      doc.getElementsByTagNameNS(OPF_NS, 'metadata')[0];
    if (found) return found;
    const m = doc.createElementNS(OPF_NS, 'metadata');
    doc.documentElement.appendChild(m);
    return m;
  }

  function dcElement(doc, tag) {
    return doc.getElementsByTagNameNS(DC_NS, tag)[0] || doc.getElementsByTagName('dc:' + tag)[0] || null;
  }

  function metaElement(doc, name) {
    return Array.from(doc.getElementsByTagName('meta')).find((m) => m.getAttribute('name') === name) || null;
  }

  function freshDoc() {
    return xml(
      '<?xml version="1.0" encoding="utf-8"?>\n' +
      '<package xmlns="' + OPF_NS + '" version="2.0" unique-identifier="uuid_id">\n' +
      '  <metadata xmlns:dc="' + DC_NS + '" xmlns:opf="' + OPF_NS + '"/>\n' +
      '</package>\n');
  }

  async function opfFromEpub(path) {
    if (typeof JSZip === 'undefined') throw new Error('JSZip unavailable on this page');
    const r = await fetch(dl(path));
    if (!r.ok) throw new Error('download ' + r.status);
    const zip = await JSZip.loadAsync(await r.blob());
    const containerFile = zip.file('META-INF/container.xml');
    if (!containerFile) throw new Error('no container.xml');
    const rootfile = xml(await containerFile.async('string')).getElementsByTagName('rootfile')[0];
    const opfPath = rootfile && rootfile.getAttribute('full-path');
    if (!opfPath) throw new Error('no rootfile');
    const opfFile = zip.file(opfPath);
    if (!opfFile) throw new Error('missing ' + opfPath);
    return opfFile.async('string');
  }

  // --- helpers --------------------------------------------------------------
  async function upload(filename, text) {
    const fd = new FormData();
    // The path is a query parameter, matching how the File Manager itself
    // uploads; /upload overwrites an existing file of the same name.
    fd.append('file', new Blob([text], { type: 'application/oebps-package+xml' }), filename);
    const r = await fetch('/upload?path=' + encodeURIComponent(dir), { method: 'POST', body: fd });
    if (!r.ok) throw new Error('upload ' + r.status + ' ' + (await r.text().catch(() => '')));
  }

  function xml(text) {
    const doc = new DOMParser().parseFromString(text, 'application/xml');
    if (doc.getElementsByTagName('parsererror').length) throw new Error('malformed XML');
    return doc;
  }

  async function getJson(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(url + ' -> ' + r.status);
    return r.json();
  }

  async function getText(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(url + ' -> ' + r.status);
    return r.text();
  }

  function dl(p) { return '/download?path=' + encodeURIComponent(p); }
  function join(d, name) { return (d === '/' ? '' : d) + '/' + name; }
  function stripExt(n) { const d = n.lastIndexOf('.'); return d > 0 ? n.slice(0, d) : n; }
  function msg(e) { return (e && e.message) || String(e); }

  function normalize(p) {
    if (!p.startsWith('/')) p = '/' + p;
    if (p.length > 1 && p.endsWith('/')) p = p.slice(0, -1);
    return p;
  }

  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, (c) =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }
});
