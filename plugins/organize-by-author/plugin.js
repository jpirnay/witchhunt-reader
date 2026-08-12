// Organize by Author - sorts loose EPUBs into one folder per author.
//
// The plugin concept and its name come from crosspoint-reader#2734 by Justin
// Mitchell (@itsthisjustin), which describes an organize-by-author File Manager
// plugin as the reference example of the same-origin endpoint pattern. That
// branch's own plugin sources were never published, so this is an independent
// implementation of the idea; the sidecar handling below is not from it.
//
// Scans a single folder (not recursive), works out each book's author, shows a
// preview, and only moves anything when you press Apply.
//
// Author lookup prefers a Calibre-style sidecar: for "book.epub" a "book.opf"
// beside it is read instead of the book. That is a few KB rather than a
// multi-megabyte download over the device's single connection, so a library
// exported from Calibre organises almost instantly. Without a sidecar the EPUB
// itself is fetched and its OPF read with JSZip, which is correct but slow -
// the preview says which of the two happened for every book.
//
// Moving a book also moves its sidecars. The firmware resolves a cover as
// "book.jpg" beside "book.epub" and prefers it over the embedded cover, so
// leaving those behind would silently break the cover on the home screen.
//
// Everything is inside this closure: plugins share the page's global scope.
CrossPoint.registerPlugin((container, api) => {
  // Matches ReaderActivity::sidecarCoverPath() plus the metadata sidecar.
  const SIDECAR_EXTS = ['.opf', '.jpg', '.jpeg', '.png', '.bmp'];
  const DC_NS = 'http://purl.org/dc/elements/1.1/';

  container.innerHTML =
    '<h2>' + esc(api.title) + '</h2>' +
    '<p>Sorts loose EPUBs in one folder into <code>&lt;Author&gt;/</code> subfolders. ' +
    'Reads a Calibre <code>book.opf</code> sidecar when there is one, otherwise downloads ' +
    'the book to read its metadata. Nothing moves until you press Apply.</p>' +
    '<p><label>Folder <input type="text" id="oba-dir" value="/" style="width:16em"></label> ' +
    '<label><input type="checkbox" id="oba-sortname" checked> Prefer sort name (Lastname, First)</label></p>' +
    '<p><button id="oba-scan">Scan</button> ' +
    '<button id="oba-apply" disabled>Apply</button> <span id="oba-status"></span></p>' +
    '<div id="oba-out"></div>';

  const el = (id) => container.querySelector(id);
  const status = (t) => { el('#oba-status').textContent = t; };
  let plan = [];

  el('#oba-scan').onclick = async () => {
    setBusy(true);
    el('#oba-out').textContent = '';
    plan = [];
    try {
      plan = await scan(normalize(el('#oba-dir').value.trim() || '/'), el('#oba-sortname').checked);
      renderPlan();
    } catch (e) {
      status('Scan failed: ' + msg(e));
    } finally {
      setBusy(false);
      el('#oba-apply').disabled = !plan.some((p) => p.willMove);
    }
  };

  el('#oba-apply').onclick = async () => {
    const todo = plan.filter((p) => p.willMove);
    if (!todo.length || !confirm('Move ' + todo.length + ' book(s) into author folders?')) return;
    setBusy(true);
    el('#oba-apply').disabled = true;
    let done = 0, failed = 0;
    for (const item of todo) {
      status('Moving ' + (done + failed + 1) + ' of ' + todo.length + '...');
      try {
        await apply(item);
        item.note = 'moved';
        done++;
      } catch (e) {
        item.note = 'FAILED: ' + msg(e);
        failed++;
      }
      renderPlan();
    }
    status('Moved ' + done + (failed ? ', ' + failed + ' failed' : '') + '. Reload to see the new layout.');
    setBusy(false);
  };

  async function scan(dir, useSortName) {
    status('Listing ' + dir + '...');
    const entries = await getJson('/api/files?path=' + encodeURIComponent(dir));

    // Index non-directory names so a sidecar can be spotted without a request.
    const present = new Set(entries.filter((e) => !e.isDirectory).map((e) => e.name));
    const books = entries.filter((e) => !e.isDirectory && e.isEpub);
    if (!books.length) {
      status('No EPUBs directly in ' + dir + '.');
      return [];
    }

    const out = [];
    for (let i = 0; i < books.length; i++) {
      const b = books[i];
      const base = stripExt(b.name);
      const sidecar = present.has(base + '.opf') ? base + '.opf' : null;
      status('Reading ' + (i + 1) + ' of ' + books.length + ' - ' + b.name +
        (sidecar ? ' (sidecar)' : ' (downloading book)') + '...');

      let author = '', how = '';
      try {
        if (sidecar) {
          author = authorFromOpf(await getText(dl(join(dir, sidecar))), useSortName);
          how = 'book.opf';
        } else {
          author = authorFromOpf(await opfFromEpub(join(dir, b.name)), useSortName);
          how = 'in book';
        }
      } catch (e) {
        how = 'unreadable: ' + msg(e);
      }

      const folder = sanitize(author);
      out.push({
        name: b.name, base, dir, author, how, folder,
        companions: SIDECAR_EXTS.map((x) => base + x).filter((n) => present.has(n)),
        // Already filed correctly, or no usable author - either way, leave alone.
        willMove: !!folder && lastSegment(dir) !== folder
      });
    }
    status(out.filter((p) => p.willMove).length + ' of ' + out.length + ' book(s) would move.');
    return out;
  }

  async function apply(item) {
    const target = join(item.dir, item.folder);
    // 400 "Folder already exists" is the success case on a re-run.
    const created = await post('/mkdir', { name: item.folder, path: item.dir });
    if (!created.ok && !/exists/i.test(created.text)) throw new Error(created.text || created.status);

    // Book first: if a companion move fails the book is still filed, and the
    // reverse would orphan the cover next to an absent book.
    const moveBook = await post('/move', { path: join(item.dir, item.name), dest: target });
    if (!moveBook.ok) throw new Error(moveBook.text || moveBook.status);
    for (const c of item.companions) {
      await post('/move', { path: join(item.dir, c), dest: target });  // best effort
    }
  }

  // --- EPUB fallback: container.xml -> OPF, via JSZip (File Manager page) ----
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

  function authorFromOpf(text, useSortName) {
    const doc = xml(text);
    let nodes = doc.getElementsByTagNameNS(DC_NS, 'creator');
    if (!nodes.length) nodes = doc.getElementsByTagName('dc:creator');
    if (!nodes.length) nodes = doc.getElementsByTagName('creator');
    if (!nodes.length) return '';
    const n = nodes[0];
    // Calibre writes the sortable form in opf:file-as, which is what its own
    // on-disk author folders use - so prefer it when organising into folders.
    const fileAs = n.getAttribute('opf:file-as') || n.getAttribute('file-as');
    return ((useSortName && fileAs) || n.textContent || '').trim();
  }

  function xml(text) {
    const doc = new DOMParser().parseFromString(text, 'application/xml');
    if (doc.getElementsByTagName('parsererror').length) throw new Error('malformed XML');
    return doc;
  }

  // --- helpers --------------------------------------------------------------
  function renderPlan() {
    if (!plan.length) { el('#oba-out').innerHTML = '<p>Nothing to do.</p>'; return; }
    let html = '<div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse">' +
      '<tr><th align="left">Book</th><th align="left">Author</th>' +
      '<th align="left">Source</th><th align="left">Moves to</th></tr>';
    for (const p of plan) {
      html += '<tr style="border-top:1px solid rgba(128,128,128,.35)">' +
        '<td>' + esc(p.name) + (p.companions.length > 1 ?
          ' <em>+' + (p.companions.length - 1) + ' sidecar</em>' : '') + '</td>' +
        '<td>' + (esc(p.author) || '<em>unknown</em>') + '</td>' +
        '<td>' + esc(p.how) + '</td>' +
        '<td>' + (p.note ? esc(p.note) : p.willMove ? esc(p.folder) + '/' : '<em>no change</em>') +
        '</td></tr>';
    }
    el('#oba-out').innerHTML = html + '</table></div>';
  }

  function setBusy(b) { el('#oba-scan').disabled = b; }
  function dl(p) { return '/download?path=' + encodeURIComponent(p); }
  function join(dir, name) { return (dir === '/' ? '' : dir) + '/' + name; }
  function lastSegment(p) { return p.slice(p.lastIndexOf('/') + 1); }
  function stripExt(n) { const d = n.lastIndexOf('.'); return d > 0 ? n.slice(0, d) : n; }
  function msg(e) { return (e && e.message) || String(e); }

  function normalize(p) {
    if (!p.startsWith('/')) p = '/' + p;
    if (p.length > 1 && p.endsWith('/')) p = p.slice(0, -1);
    return p;
  }

  // Drops the characters /mkdir rejects, plus leading dots (which the firmware
  // treats as hidden) and trailing dots/spaces (invalid on FAT).
  function sanitize(a) {
    return String(a || '').replace(/["*:<>?\/\\|]/g, ' ')
      .replace(/\s+/g, ' ').replace(/^[.\s]+|[.\s]+$/g, '').slice(0, 64);
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

  async function post(path, fields) {
    const fd = new FormData();
    for (const k of Object.keys(fields)) fd.append(k, fields[k]);
    const r = await fetch(path, { method: 'POST', body: fd });
    return { ok: r.ok, status: r.status, text: await r.text().catch(() => '') };
  }

  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, (c) =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }
});
