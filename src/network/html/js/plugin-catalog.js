// Upstream crosspoint-reader's on-device catalogs (a plugin's device.json), run on the Plugins page.
//
// Upstream browses a device.json on the reader, in PluginCatalogActivity: #3114 by Justin Mitchell (the catalog,
// bundles, the filename rules); #3877, #4170, #4260 and #4357 by Jadehawk (compatibility and lists, paging after
// filtering, list requirements, remembered notices); #4195, #4196, #4197, #4198, #4221 and #4273 by Arthur
// Tazhitdinov (sized pages, the _enc variables, list mapping, untitled rows, server paths). This is not a port of that
// code but the same vocabulary, run in the browser:
// requests go through the plugin's api (makePluginApi() in shared/PluginHost.js.inc), so the device fetches them, every
// host passes the user's approval, and downloads land on the card. Upstream's docs/sd-plugins.md, "Surface 3", is the
// reference for the fields.
//
// Runs: JSON catalogs, their four paging modes, static and fetched lists with notices and requirements, search, item
// compatibility filtering, the template variables ({cfg.*} from the plugin's config file), and downloads of one file
// (by URL or a url_path answer) or of a bundle. Not yet: sign-in (auth, {token}), XML catalogs, Details pages, the
// sidecar file.
//
// PluginsPage.html loads this on demand; window.PluginCatalog is its interface.
(function () {
  'use strict';

  const FIRMWARE_FAMILY = 'witchhunt';
  // A catalog entry's compatibility.firmware_family may name either: this firmware speaks upstream's plugin API.
  const FAMILIES = ['witchhunt', 'crosspoint'];
  const PLUGIN_API = 1;
  // Upstream's catalog capabilities this runner implements (its docs/sd-plugins.md, "Firmware identity").
  const CAPABILITIES = [
    'catalog.lists', 'catalog.notices', 'catalog.bundle_install', 'catalog.compatibility',
    'catalog.encoded_item_fields', 'catalog.list_index_mapping', 'catalog.list_requirements', 'catalog.server_paths'
  ];
  // Hidden folders a catalog may name, and the visible one this firmware reads as well (SleepActivity.cpp reads
  // /.sleep and /sleep, DictionaryRegistry.cpp /dictionaries and /.dictionaries). The web server refuses a dot folder
  // while Show Hidden Files is off; the visible twin works either way.
  const VISIBLE_TWINS = { '.sleep': 'sleep', '.dictionaries': 'dictionaries' };

  // ---- Pure helpers ---------------------------------------------------------------------------------------------------

  // A dotted path into JSON ("authors.0.name"); "" is the value itself.
  function getPath(value, path) {
    if (path === undefined || path === null || path === '') return value;
    let v = value;
    for (const part of String(path).split('.')) {
      if (v === null || v === undefined) return undefined;
      v = Array.isArray(v) && /^\d+$/.test(part) ? v[Number(part)] : v[part];
    }
    return v;
  }

  function text(v) {
    if (typeof v === 'string') return v;
    return typeof v === 'number' || typeof v === 'boolean' ? String(v) : '';
  }

  function utf8(s) {
    return new TextEncoder().encode(s);
  }

  function percent(bytes) {
    return Array.from(bytes, (b) => '%' + b.toString(16).toUpperCase().padStart(2, '0')).join('');
  }

  // Percent-encodes all but letters, digits and -_.~, as upstream's _enc variables and {query} do.
  function enc(v) {
    return Array.from(text(v), (c) => (/^[A-Za-z0-9\-_.~]$/.test(c) ? c : percent(utf8(c)))).join('');
  }

  // A URL as upstream sends it (pluginhttp::escapeUrl): spaces, control bytes and non-ASCII percent-encoded, the rest
  // as written.
  function escapeUrl(url) {
    return Array.from(text(url), (c) => (/^[\x21-\x7e]$/.test(c) ? c : percent(utf8(c)))).join('');
  }

  // {name} and {cfg.KEY} substituted; an unknown name stays as written, as upstream leaves it.
  function resolveTemplate(template, vars) {
    return text(template).replace(/\{([A-Za-z_][A-Za-z0-9_.]*)\}/g, (whole, name) =>
      (Object.prototype.hasOwnProperty.call(vars, name) ? text(vars[name]) : whole));
  }

  function resolveHeaders(headers, vars) {
    const out = {};
    if (headers && typeof headers === 'object' && !Array.isArray(headers)) {
      for (const [name, value] of Object.entries(headers)) out[name] = resolveTemplate(value, vars);
    }
    return out;
  }

  // How a request pages (upstream's PluginCatalogPaging.h), from the variables its url, body and headers use:
  //   offset      {offset}: rows from a position, {limit} = a page plus one lookahead row
  //   page-limit  {page} and {limit}: {limit} is the page size, and a full page means there is another
  //   page        {page} alone: the server's pages, {limit} = a page plus one lookahead row
  //   static      neither: one answer holds every row, paged here
  function pagingMode(request) {
    const parts = [text(request.url), text(request.body)].concat(Object.values(request.headers || {}).map(text));
    const uses = (key) => parts.some((p) => p.includes(key));
    if (uses('{offset}')) return 'offset';
    if (!uses('{page}')) return 'static';
    return uses('{limit}') ? 'page-limit' : 'page';
  }

  function pageSize(browse) {
    const n = Math.round(Number(browse && browse.page_size));
    return Number.isFinite(n) && n > 0 ? Math.min(16, n) : 8;
  }

  // Whether a catalog row's compatibility metadata (paths given by browse.compatibility) allows this firmware; a row
  // without any passes.
  function compatible(raw, mapping) {
    if (!mapping || typeof mapping !== 'object') return true;
    const family = text(getPath(raw, mapping.firmware_family)).trim();
    if (family && !FAMILIES.includes(family)) return false;
    const apiMin = Number(text(getPath(raw, mapping.plugin_api_min)) || 0);
    if (apiMin > PLUGIN_API) return false;
    const requires = getPath(raw, mapping.requires);
    const needed = Array.isArray(requires) ? requires.map(text) : text(requires).split(',');
    return needed.map((r) => r.trim()).filter(Boolean).every((r) => CAPABILITIES.includes(r));
  }

  // A list's requires: capability names, all of which must be ours. A malformed one hides the list.
  function listAllowed(requires) {
    if (requires === undefined || requires === null) return true;
    if (!Array.isArray(requires)) return false;
    return requires.every((r) => typeof r === 'string' && CAPABILITIES.includes(r));
  }

  // A server path ("/files/1") from an answer, joined to the origin of the request that gave it; anything else as is.
  function joinServerPath(path, requestUrl) {
    const p = text(path);
    if (!p.startsWith('/') || p.startsWith('//')) return p;
    try {
      return new URL(requestUrl).origin + p;
    } catch (e) {
      return p;
    }
  }

  // Upstream's StringUtils::sanitizeFilenamePreservingExtension: unsafe characters become "_", control characters
  // go, leading spaces and dots go, at most maxBytes of UTF-8, trailing spaces and dots trimmed, "book" if nothing is
  // left; a conventional extension (2 to 16 bytes with the dot, alphanumeric) is kept whole.
  function sanitizeRange(name, maxBytes) {
    let out = '';
    let bytes = 0;
    for (const ch of name.replace(/^[ .]+/, '')) {
      const cp = ch.codePointAt(0);
      if (cp < 32 || cp === 127) continue;
      const add = '/\\:*?"<>|'.includes(ch) ? '_' : ch;
      const n = utf8(add).length;
      if (bytes + n > maxBytes) break;
      out += add;
      bytes += n;
    }
    return out.replace(/[ .]+$/, '') || 'book';
  }

  function sanitizeFilename(name, maxBytes) {
    const max = maxBytes || 100;
    const n = text(name);
    const dot = n.lastIndexOf('.');
    const ext = dot > 0 ? n.slice(dot) : '';
    if (!/^\.[A-Za-z0-9]{1,15}$/.test(ext) || ext.length >= max) return sanitizeRange(n, max);
    return sanitizeRange(n.slice(0, dot), max - ext.length) + ext;
  }

  // A card folder from a manifest or config value: "" is the card's root, a relative one sits in the plugin's own
  // folder (upstream's inPluginDir), and a hidden top folder this firmware also reads visibly becomes that one.
  // null when it climbs ("..").
  function cardFolder(path, pluginDir) {
    let p = text(path).trim();
    if (p && !p.startsWith('/')) p = pluginDir + '/' + p;
    const parts = p.split('/').filter((s) => s !== '' && s !== '.');
    if (parts.some((s) => s === '..')) return null;
    if (parts.length && VISIBLE_TWINS[parts[0]]) parts[0] = VISIBLE_TWINS[parts[0]];
    return '/' + parts.join('/');
  }

  function joinPath(dir, name) {
    return (dir === '/' ? '' : dir) + '/' + name;
  }

  // What of a device.json runs here: {catalog, reason, events}. catalog: it can be opened on the Plugins page;
  // reason: why not, when it has one that cannot; events: it subscribes to device events, which do not run here yet.
  function describe(manifest) {
    const m = manifest && typeof manifest === 'object' ? manifest : {};
    const browse = m.browse && typeof m.browse === 'object' ? m.browse : null;
    const out = {
      catalog: false, reason: '',
      events: !!(m.events && typeof m.events === 'object' && Object.keys(m.events).length)
    };
    if (!browse) return out;
    const lists = Array.isArray(browse.lists) ? browse.lists : [];
    if (!browse.url && !browse.lists_url && !lists.some((l) => l && l.url)) return out;
    if (text(browse.format || 'json') !== 'json') {
      out.reason = 'its catalog is XML, which does not run here yet';
    } else if (m.auth || m.token) {
      out.reason = 'its catalog needs a sign-in, which does not run here yet';
    } else {
      out.catalog = true;
    }
    return out;
  }

  // ---- A catalog ------------------------------------------------------------------------------------------------------

  // One open catalog. plugin: its /api/plugins entry; manifest: its device.json; api: makePluginApi(plugin);
  // io: {readFile(cardPath) -> text or null, mkdir(parent, name), confirm(message) -> bool,
  //      remember: {get(key), set(key, value)}, firmwareVersion}.
  class Catalog {
    constructor(plugin, manifest, api, io) {
      this.plugin = plugin;
      this.m = manifest || {};
      this.browse = this.m.browse || {};
      this.api = api;
      this.io = io;
      this.dir = api.dir;
      this.vars = {};
      this.config = {};
      this.lists = [];
      this.list = null;  // the open list; null browses browse.url itself
      this.query = '';
      this.page = 1;
      this.staticRows = null;  // a static answer's compatible rows, paged here
      this.lane = Promise.resolve();
    }

    // The device serves one connection at a time, and requests side by side can stall it: a download started while
    // a page loads waits for it.
    serial(fn) {
      const next = this.lane.then(fn, fn);
      this.lane = next.catch(() => {});
      return next;
    }

    // Reads the plugin's config file, then its lists. Resolves to the lists.
    async start() {
      const vars = {
        firmware_family: FIRMWARE_FAMILY, firmware_version: this.io.firmwareVersion || '', plugin_api: PLUGIN_API,
        plugin_capabilities: CAPABILITIES.join(','), plugin_name: this.plugin.name
      };
      const configFile = this.m.config && text(this.m.config.file);
      if (configFile) {
        const file = configFile.startsWith('/') ? configFile : this.dir + '/' + configFile;
        const raw = await this.serial(() => this.io.readFile(file));
        try {
          const cfg = raw ? JSON.parse(raw) : null;
          if (cfg && typeof cfg === 'object' && !Array.isArray(cfg)) {
            this.config = cfg;
            for (const [k, v] of Object.entries(cfg)) vars['cfg.' + k] = text(v);
          }
        } catch (e) { /* upstream reads an unparsable config as none */ }
      }
      this.vars = vars;
      this.lists = (Array.isArray(this.browse.lists) ? this.browse.lists : [])
        .filter((l) => l && text(l.title) && listAllowed(l.requires));
      if (this.browse.lists_url) {
        try {
          this.lists = this.lists.concat(await this.fetchLists());
        } catch (e) {
          if (!this.lists.length && !this.browse.url) throw e;
        }
      }
      return this.lists;
    }

    // One catalog request, redirects followed: the relay answers a 3xx as it is (upstream's contract for plugins),
    // where upstream's own catalog screen follows it. A hop to another site drops the headers; a host the plugin may
    // not reach yet is asked about, as any is.
    async relay(method, url, headers, body) {
      let res;
      for (let hops = 0; ; hops++) {
        const request = { method, url: escapeUrl(url), headers, body };
        res = await this.serial(() => this.api.relay(request.method, request.url, request.headers, request.body));
        const location = (res.headers || []).find((h) => String(h[0]).toLowerCase() === 'location');
        if (res.status < 300 || res.status >= 400 || !location || hops === 5) break;
        const next = new URL(location[1], request.url);
        if (next.origin !== new URL(request.url).origin) headers = {};
        if (res.status !== 307 && res.status !== 308) {
          method = 'GET';
          body = '';
        }
        url = next.href;
      }
      if (res.status === 401 || res.status === 403) {
        throw new Error('the catalog refused the request (' + res.status + '): it may need a sign-in');
      }
      if (res.status < 200 || res.status >= 300) throw new Error('the catalog answered ' + res.status);
      try {
        return JSON.parse(res.body);
      } catch (e) {
        throw new Error('the catalog did not answer JSON');
      }
    }

    async fetchLists() {
      const b = this.browse;
      const url = resolveTemplate(b.lists_url, this.vars);
      const json = await this.relay('GET', url, resolveHeaders(b.lists_headers, this.vars), '');
      const array = getPath(json, b.lists_items || 'lists');
      const f = Object.assign({ title: 'title', url: 'url', id: 'id' }, b.lists_fields || {});
      const lists = [];
      for (const raw of Array.isArray(array) ? array : []) {
        const entry = {
          title: text(getPath(raw, f.title)), url: joinServerPath(getPath(raw, f.url), url), id: text(getPath(raw, f.id)),
          notice: raw && raw.notice, requires: raw && raw.requires
        };
        if (!entry.title || !listAllowed(entry.requires)) continue;
        if (!entry.url) delete entry.url;
        lists.push(entry);
      }
      return lists;
    }

    // Shows a list's notice; false when the user goes back. notice.once with a stable id is remembered.
    async acceptNotice(list) {
      const n = list && list.notice;
      if (!n || !text(n.message)) return true;
      const id = text(n.id);
      const remembered = n.once === true && /^[A-Za-z0-9._-]{1,64}$/.test(id);
      const key = 'plugin-notice:' + this.plugin.name + ':' + id;
      if (remembered && this.io.remember.get(key)) return true;
      const ok = await this.io.confirm(resolveTemplate(n.title || list.title, this.vars) + '\n\n' +
                                       resolveTemplate(n.message, this.vars));
      if (ok && remembered) this.io.remember.set(key, '1');
      return ok;
    }

    async openList(list) {
      if (!(await this.acceptNotice(list))) return false;
      this.list = list || null;
      this.query = '';
      this.page = 1;
      this.staticRows = null;
      return true;
    }

    search(query) {
      this.query = text(query).trim();
      this.page = 1;
      this.staticRows = null;
    }

    hasSearch() {
      return !!(this.browse.search && this.browse.search.url);
    }

    // The browse request in force: the search while there is a query, else the open list's, else browse's own.
    request() {
      const b = this.browse;
      const base = { method: (b.method || 'GET').toUpperCase(), headers: b.headers };
      if (this.query && this.hasSearch()) {
        return Object.assign(base, { url: b.search.url, body: b.search.body !== undefined ? b.search.body : b.body });
      }
      const l = this.list || {};
      return Object.assign(base, { url: l.url || b.url, body: l.body !== undefined ? l.body : b.body });
    }

    rowOf(raw) {
      const f = this.browse.fields || {};
      return {
        raw, title: text(getPath(raw, f.title || 'title')), author: text(getPath(raw, f.author)),
        id: text(getPath(raw, f.id)), url: text(getPath(raw, f.url)), description: text(getPath(raw, f.description))
      };
    }

    listVars() {
      const id = this.list ? text(this.list.id) : '';
      return { list_id: id, list_id_enc: enc(id) };
    }

    // The current page: {rows, hasPrevious, hasNext}.
    async load() {
      const req = this.request();
      const mode = pagingMode(req);
      const size = pageSize(this.browse);
      const shown = (r) => r.title && compatible(r.raw, this.browse.compatibility);
      if (mode === 'static' && this.staticRows) return this.window(this.staticRows, size);

      const vars = Object.assign({}, this.vars, this.listVars(), {
        page: this.page, offset: (this.page - 1) * size, limit: mode === 'page-limit' ? size : size + 1,
        query: enc(this.query), query_raw: this.query
      });
      const body = req.body === undefined ? '' : typeof req.body === 'string' ? req.body : JSON.stringify(req.body);
      const json = await this.relay(req.method, resolveTemplate(req.url, vars), resolveHeaders(req.headers, vars),
                                    resolveTemplate(body, vars));
      const array = getPath(json, this.browse.items || '');
      const rows = (Array.isArray(array) ? array : []).map((r) => this.rowOf(r));
      if (mode === 'static') {
        this.staticRows = rows.filter(shown);
        return this.window(this.staticRows, size);
      }
      // The server counted every row it sent, shown or not (upstream #4221).
      return {
        rows: rows.slice(0, size).filter(shown), hasPrevious: this.page > 1,
        hasNext: mode === 'page-limit' ? rows.length >= size : rows.length > size
      };
    }

    window(rows, size) {
      const start = (this.page - 1) * size;
      return { rows: rows.slice(start, start + size), hasPrevious: this.page > 1, hasNext: rows.length > start + size };
    }

    itemVars(row) {
      return Object.assign({}, this.vars, this.listVars(), {
        id: row.id, title: row.title, author: row.author, url: row.url,
        id_enc: enc(row.id), title_enc: enc(row.title), author_enc: enc(row.author), url_enc: enc(row.url)
      });
    }

    // Where downloads go: the config's dest_dir when it sets one (upstream's downloadDir()), else the manifest's.
    downloadDir(vars) {
      const fromConfig = text(this.config.dest_dir).trim();
      if (fromConfig) return cardFolder(fromConfig.startsWith('/') ? fromConfig : '/' + fromConfig, this.dir);
      return cardFolder(resolveTemplate((this.m.download || {}).dest_dir || '', vars), this.dir);
    }

    // Makes each folder of a card path in turn. /mkdir refuses one that exists, which is the usual case; a folder
    // that is really missing makes the download after it fail.
    async ensureFolder(path) {
      let parent = '/';
      for (const part of path.split('/').filter(Boolean)) {
        try {
          const where = parent;
          await this.serial(() => this.io.mkdir(where, part));
        } catch (e) { /* see above */ }
        parent = joinPath(parent, part);
      }
    }

    // Downloads a row: a bundle (base and files from the row) into <dest_dir>/<subdir>, else one file into dest_dir.
    // Resolves to the card paths written.
    async download(row) {
      const dl = this.m.download || {};
      const vars = this.itemVars(row);
      const dir = this.downloadDir(vars);
      if (dir === null) throw new Error('the catalog names an unsafe folder');

      const bundle = dl.bundle && typeof dl.bundle === 'object' ? dl.bundle : null;
      const files = bundle ? getPath(row.raw, bundle.files) : null;
      if (bundle && Array.isArray(files) && files.length) return this.downloadBundle(row, bundle, files, dir, vars);

      const headers = resolveHeaders(dl.headers, vars);
      if (dl.username || dl.password) {
        const pair = utf8(resolveTemplate(dl.username || '', vars) + ':' + resolveTemplate(dl.password || '', vars));
        headers.Authorization = 'Basic ' + btoa(String.fromCharCode(...pair));
      }
      let url = resolveTemplate(dl.url || '{url}', vars);
      let method = (dl.method || 'GET').toUpperCase();
      let body = dl.body === undefined ? '' : resolveTemplate(dl.body, vars);
      let fileHeaders = headers;
      if (dl.url_path) {
        const answer = text(getPath(await this.relay(method, url, headers, body), dl.url_path));
        if (!answer) throw new Error('the catalog named no file to download');
        // A server path is on the API's host and gets the headers; a full URL may be pre-signed and gets none.
        if (!answer.startsWith('/') || answer.startsWith('//')) fileHeaders = {};
        url = joinServerPath(answer, url);
        method = 'GET';
        body = '';
      }
      const unfilled = /\{[A-Za-z_][A-Za-z0-9_.]*\}/.exec(url);
      if (!url || unfilled) {
        throw new Error(unfilled ? 'the download address needs ' + unfilled[0] + ', which this catalog does not give'
                                 : 'the catalog named no file to download');
      }
      const name = sanitizeFilename(resolveTemplate(dl.filename || '{title}.epub', vars));
      if (name.includes('..')) throw new Error('the catalog names an unsafe file: ' + name);
      await this.ensureFolder(dir);
      const dest = joinPath(dir, name);
      await this.serial(() => this.api.fetchToSd(escapeUrl(url), dest, fileHeaders, { method, body }));
      return [dest];
    }

    // Upstream's downloadBundle(): relative files under base, into a subfolder named by bundle.subdir.
    async downloadBundle(row, bundle, files, dir, vars) {
      const subdir = resolveTemplate(bundle.subdir || '{id}', vars);
      if (!subdir || subdir === '.' || subdir.includes('..') || subdir.includes('\\') || subdir.startsWith('/')) {
        throw new Error('the catalog names an unsafe folder: ' + subdir);
      }
      let base = text(getPath(row.raw, bundle.base));
      if (!base) throw new Error('the catalog gave no address for the files');
      if (!base.endsWith('/')) base += '/';
      const target = joinPath(dir, subdir);
      const plan = files.map((f) => {
        const rel = text(f);
        if (!rel || rel.startsWith('/') || rel.includes('\\') || rel.split('/').some((s) => s === '' || s === '.' || s === '..')) {
          throw new Error('the catalog lists an unsafe file: ' + rel);
        }
        return { url: base + rel, dest: joinPath(target, rel) };
      });
      const written = [];
      for (const f of plan) {
        await this.ensureFolder(f.dest.slice(0, f.dest.lastIndexOf('/')));
        try {
          await this.serial(() => this.api.fetchToSd(escapeUrl(f.url), f.dest));
        } catch (e) {
          const err = new Error(((e && e.message) || e) + (written.length ? ' (' + written.length + ' of ' + plan.length +
                                ' files saved; download it again to finish)' : ''));
          err.written = written;
          throw err;
        }
        written.push(f.dest);
      }
      return written;
    }
  }

  // ---- The view -------------------------------------------------------------------------------------------------------

  function el(tag, props, children) {
    const node = document.createElement(tag);
    Object.assign(node, props || {});
    (children || []).forEach((c) => c && node.append(c));
    return node;
  }

  // Opens a catalog in host. Resolves to the Catalog once its first page shows (or its error does).
  async function open(host, plugin, manifest, api, io, onClose) {
    const catalog = new Catalog(plugin, manifest, api, io);
    const status = el('p', { className: 'catalog-status', role: 'status' });
    const controls = el('div', { className: 'catalog-controls' });
    const rows = el('div', { className: 'catalog-rows' });
    const pager = el('div', { className: 'catalog-pager' });
    host.replaceChildren(
      el('div', { className: 'catalog-head' }, [
        el('h2', { textContent: text(manifest.title) || plugin.title || plugin.name }),
        el('button', { className: 'btn btn-small', textContent: 'Close', onclick: () => onClose && onClose() })
      ]),
      text(manifest.description) ? el('p', { className: 'card-intro', textContent: manifest.description }) : '',
      controls, rows, pager, status);
    const say = (message, kind) => {
      status.textContent = message || '';
      status.className = 'catalog-status' + (kind ? ' status-' + kind : '');
    };
    const failure = (e) => ((e && e.message) || String(e)).replace(/^./, (c) => c.toUpperCase());

    let loading = 0;
    const showPage = async () => {
      const ticket = ++loading;
      say('Loading...', 'busy');
      rows.replaceChildren();
      pager.replaceChildren();
      let page;
      try {
        page = await catalog.load();
      } catch (e) {
        if (ticket === loading) say(failure(e), 'err');
        return;
      }
      if (ticket !== loading) return;  // the user picked another list or page meanwhile
      say(page.rows.length ? '' : 'Nothing here.');
      rows.replaceChildren(...page.rows.map(rowView));
      pager.replaceChildren(
        page.hasPrevious ? el('button', { className: 'btn btn-small', textContent: 'Previous page',
                                          onclick: () => { catalog.page--; showPage(); } }) : '',
        page.hasNext ? el('button', { className: 'btn btn-small', textContent: 'Next page',
                                      onclick: () => { catalog.page++; showPage(); } }) : '');
    };

    const rowView = (row) => {
      const button = el('button', { className: 'btn btn-small btn-primary', textContent: 'Download' });
      button.onclick = async () => {
        button.disabled = true;
        say('Downloading ' + row.title + '...', 'busy');
        try {
          const written = await catalog.download(row);
          say(written.length === 1 ? 'Saved ' + written[0]
            : 'Saved ' + written.length + ' files in ' + written[0].slice(0, written[0].lastIndexOf('/')), 'ok');
        } catch (e) {
          say('Could not download ' + row.title + ': ' + ((e && e.message) || e), 'err');
        }
        button.disabled = false;
      };
      return el('div', { className: 'plugin' }, [
        el('div', { className: 'plugin-info' }, [
          el('strong', { textContent: row.title }),
          row.author ? el('div', { className: 'plugin-meta', textContent: row.author }) : '',
          row.description ? el('div', { className: 'plugin-meta', textContent: row.description.slice(0, 300) }) : ''
        ]),
        el('div', { className: 'plugin-actions' }, [button])
      ]);
    };

    const renderControls = () => {
      const out = [];
      if (catalog.lists.length > 1 || (catalog.lists.length === 1 && !catalog.list)) {
        out.push(el('div', { className: 'catalog-lists' }, catalog.lists.map((l) => el('button', {
          className: 'btn btn-small' + (catalog.list === l && !catalog.query ? ' btn-primary' : ''),
          textContent: text(l.title),
          onclick: async () => {
            if (!(await catalog.openList(l))) return;
            renderControls();
            showPage();
          }
        }))));
      }
      if (catalog.hasSearch()) {
        const input = el('input', { type: 'search', placeholder: 'Search', value: catalog.query });
        input.setAttribute('aria-label', 'Search');
        const form = el('form', { className: 'catalog-search' }, [
          input, el('button', { className: 'btn btn-small', type: 'submit', textContent: 'Search' })
        ]);
        form.onsubmit = (ev) => {
          ev.preventDefault();
          catalog.search(input.value);
          renderControls();
          showPage();
        };
        out.push(form);
      }
      controls.replaceChildren(...out);
    };

    say('Opening...', 'busy');
    try {
      await catalog.start();
    } catch (e) {
      say(failure(e), 'err');
      return catalog;
    }
    // Upstream opens a single list at once, after its notice, and shows a picker over several. Here the picker is a
    // row of buttons, so the first of several opens too unless it asks first. No lists: browse.url is the catalog.
    const first = catalog.lists[0];
    const asks = first && first.notice && text(first.notice.message);
    if (catalog.lists.length > 1 && asks) {
      renderControls();
      say('Pick a list.');
      return catalog;
    }
    if (first && !(await catalog.openList(first))) {
      say('You went back from the notice; close this or open it again.');
      return catalog;
    }
    renderControls();
    await showPage();
    return catalog;
  }

  window.PluginCatalog = {
    open, describe, Catalog, CAPABILITIES, FIRMWARE_FAMILY,
    // For the tests.
    _internal: {
      getPath, enc, escapeUrl, resolveTemplate, pagingMode, pageSize, compatible, listAllowed, sanitizeFilename,
      cardFolder, joinServerPath
    }
  };
})();
