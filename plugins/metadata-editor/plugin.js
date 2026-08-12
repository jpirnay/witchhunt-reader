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
    '<p><label>Folder <input type="text" id="me-dir" value="/" style="width:16em"></label> ' +
    '<button id="me-scan">Scan</button> <span id="me-status"></span></p>' +
    '<div id="me-list"></div>' +
    '<div id="me-form"></div>';

  const el = (id) => container.querySelector(id);
  const status = (t) => { el('#me-status').textContent = t; };
  let dir = '/';
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
      '<button id="me-cancel">Cancel</button> <span id="me-saved"></span></p>';
    el('#me-form').innerHTML = html;
    el('#me-cancel').onclick = () => { el('#me-form').textContent = ''; editing = null; };
    el('#me-save').onclick = save;
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
