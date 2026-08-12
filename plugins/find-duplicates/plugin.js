// Find Duplicates - reports files that share an exact byte size.
//
// Grown from the "library tidy tools (dedupe, bulk rename, move-by-metadata)"
// idea in crosspoint-reader#2734 by Justin Mitchell (@itsthisjustin). That
// branch shipped no plugin sources, so the implementation here is our own.
//
// Report only: this plugin never writes, moves or deletes anything. It also
// never downloads a book - it works purely from the /api/files listing, so
// scanning a large library costs one small request per folder and no book
// traffic at all.
//
// Everything lives inside the registerPlugin closure. Plugins share one page
// and one global scope, so a top-level `function escapeHtml()` here would
// collide with the host page and with other plugins.
CrossPoint.registerPlugin((container, api) => {
  // Descending into these would walk the book cache, which holds thousands of
  // files and is not part of the user's library.
  const SKIP_DOT_DIRS = true;
  const MAX_FOLDERS = 200;
  const MAX_FILES = 3000;

  container.innerHTML =
    '<h2>' + esc(api.title) + '</h2>' +
    '<p>Lists files that share an exact size - usually the same book stored twice. ' +
    'Reads only the folder listing, so it never downloads a book, and never changes anything.</p>' +
    '<p><label>Start folder <input type="text" id="fd-root" value="/" style="width:16em"></label> ' +
    '<label><input type="checkbox" id="fd-epub" checked> EPUBs only</label></p>' +
    '<p><button id="fd-scan">Scan</button> <span id="fd-status"></span></p>' +
    '<div id="fd-out"></div>';

  const el = (id) => container.querySelector(id);
  const status = (t) => { el('#fd-status').textContent = t; };

  el('#fd-scan').onclick = async () => {
    const root = el('#fd-root').value.trim() || '/';
    const epubOnly = el('#fd-epub').checked;
    el('#fd-scan').disabled = true;
    el('#fd-out').textContent = '';
    try {
      const files = await walk(root, epubOnly);
      render(groupBySize(files));
    } catch (e) {
      status('Failed: ' + ((e && e.message) || e));
    } finally {
      el('#fd-scan').disabled = false;
    }
  };

  // Breadth-first, one request at a time. The device serves a single
  // connection, so concurrent listing requests would only queue anyway.
  async function walk(root, epubOnly) {
    const queue = [normalize(root)];
    const files = [];
    let folders = 0;

    while (queue.length && folders < MAX_FOLDERS && files.length < MAX_FILES) {
      const dir = queue.shift();
      folders++;
      status('Scanning ' + dir + ' (' + folders + ' folders, ' + files.length + ' files)...');

      let entries;
      try {
        const r = await fetch('/api/files?path=' + encodeURIComponent(dir));
        if (!r.ok) throw new Error(r.status + ' on ' + dir);
        entries = await r.json();
      } catch (e) {
        continue;  // unreadable folder should not abort the whole scan
      }

      for (const entry of entries) {
        const name = entry.name || '';
        if (!name || name === '.' || name === '..') continue;
        const full = (dir === '/' ? '' : dir) + '/' + name;
        if (entry.isDirectory) {
          if (SKIP_DOT_DIRS && name.startsWith('.')) continue;
          queue.push(full);
        } else if (!epubOnly || entry.isEpub) {
          files.push({ path: full, size: entry.size || 0 });
        }
      }
    }

    const capped = folders >= MAX_FOLDERS || files.length >= MAX_FILES;
    status(files.length + ' files in ' + folders + ' folders' +
      (capped ? ' (stopped at the scan limit - narrow the start folder)' : ''));
    return files;
  }

  function groupBySize(files) {
    const bySize = new Map();
    for (const f of files) {
      if (!f.size) continue;  // zero-length files are not interesting duplicates
      if (!bySize.has(f.size)) bySize.set(f.size, []);
      bySize.get(f.size).push(f);
    }
    return [...bySize.values()]
      .filter((g) => g.length > 1)
      .sort((a, b) => b[0].size - a[0].size);
  }

  function render(groups) {
    const out = el('#fd-out');
    if (!groups.length) {
      out.innerHTML = '<p>No duplicates found.</p>';
      return;
    }
    const wasted = groups.reduce((n, g) => n + g[0].size * (g.length - 1), 0);
    let html = '<p><strong>' + groups.length + ' group(s)</strong>, about ' +
      kb(wasted) + ' recoverable if you delete one copy from each.</p>' +
      '<div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse">' +
      '<tr><th align="left">Size</th><th align="left">Copies</th><th align="left">Paths</th></tr>';
    for (const g of groups) {
      html += '<tr style="border-top:1px solid rgba(128,128,128,.35)">' +
        '<td valign="top">' + kb(g[0].size) + '</td>' +
        '<td valign="top">' + g.length + '</td>' +
        '<td>' + g.map((f) => esc(f.path)).join('<br>') + '</td></tr>';
    }
    out.innerHTML = html + '</table></div>' +
      '<p><em>Same size is a strong hint, not proof. Check before deleting anything ' +
      '- deletion is not done here, use the file list above.</em></p>';
  }

  function normalize(p) {
    if (!p.startsWith('/')) p = '/' + p;
    if (p.length > 1 && p.endsWith('/')) p = p.slice(0, -1);
    return p;
  }

  function kb(n) {
    if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
    if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
    return n + ' B';
  }

  function esc(s) {
    return String(s).replace(/[&<>"']/g, (c) =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }
});
