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
// compatibility filtering, the template variables ({cfg.*} from the plugin's config file, {token} from its token
// file), sign-in by password grant or device code, and downloads of one file (by URL or a url_path answer) or of a
// bundle. Not yet: XML catalogs, Details pages, the sidecar file, and token or config files outside the plugin's
// folder.
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

  // A file a manifest names (token.file, config.file): a relative one is in the plugin's folder (upstream's inPluginDir).
  function pluginFile(file, pluginDir) {
    const f = text(file).trim();
    return !f ? '' : f.startsWith('/') ? f : pluginDir + '/' + f;
  }

  // Whether a path is inside the plugin's own folder, the only place the web server lets a plugin keep a file of its
  // own (pluginWriteTarget(), /download's exact reads). Upstream allows any path but the credential stores.
  function inPluginFolder(path, pluginDir) {
    return !!pluginDir && path.startsWith(pluginDir + '/') && !path.split('/').includes('..');
  }

  // A token file's JSON: the token alone, nested at token.path, as upstream's saveTokenToFile() writes it.
  function tokenJson(path, value) {
    const segs = text(path || 'token').split('.').filter(Boolean);
    const root = {};
    let node = root;
    segs.slice(0, -1).forEach((seg) => { node = node[seg] = {}; });
    node[segs[segs.length - 1] || 'token'] = value;
    return JSON.stringify(root);
  }

  // The sign-in a manifest's auth asks for: 'password' or 'device_code' (upstream's default type), '' for none (a token
  // the plugin's web card writes, or no sign-in), or 'other:<type>' for one upstream does not define.
  function signInKind(m) {
    const a = m && m.auth;
    if (!a || typeof a !== 'object' || !(a.request && a.request.url)) return '';
    const type = text(a.type || 'device_code');
    return type === 'password' || type === 'device_code' ? type : 'other:' + type;
  }

  function signInNeeded(message) {
    const e = new Error(message);
    e.signIn = true;
    return e;
  }

  // What of a device.json runs here: {catalog, reason, events}. catalog: it can be opened on the Plugins page;
  // reason: why not, when it has one that cannot; events: it subscribes to device events, which do not run here yet.
  function describe(manifest, pluginDir) {
    const m = manifest && typeof manifest === 'object' ? manifest : {};
    const browse = m.browse && typeof m.browse === 'object' ? m.browse : null;
    const out = {
      catalog: false, reason: '',
      events: !!(m.events && typeof m.events === 'object' && Object.keys(m.events).length)
    };
    if (!browse) return out;
    const lists = Array.isArray(browse.lists) ? browse.lists : [];
    if (!browse.url && !browse.lists_url && !lists.some((l) => l && l.url)) return out;
    const kind = signInKind(m);
    const outside = [m.token && m.token.file, m.config && m.config.file].map((f) => text(f).trim())
      .find((f) => f.startsWith('/') && !inPluginFolder(f, pluginDir));
    if (text(browse.format || 'json') !== 'json') {
      out.reason = 'its catalog is XML, which does not run here yet';
    } else if (kind.startsWith('other:')) {
      out.reason = 'its sign-in ("' + kind.slice(6) + '") is not one this page knows';
    } else if (outside) {
      out.reason = 'it keeps ' + outside + ' outside its own folder, which this firmware does not allow';
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
      this.cancelled = false;  // set to stop a device-code sign-in that is waiting
      this.wait = io.wait || ((ms) => new Promise((resolve) => setTimeout(resolve, ms)));
      this.now = io.now || (() => Date.now());
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
      // A token from a sign-in this visit is kept: one without a token.file lives only here.
      if (this.vars.token) vars.token = this.vars.token;
      this.vars = vars;
      await this.ensureToken();
      this.lists = (Array.isArray(this.browse.lists) ? this.browse.lists : [])
        .filter((l) => l && text(l.title) && listAllowed(l.requires));
      if (this.browse.lists_url) {
        try {
          this.lists = this.lists.concat(await this.authorized(() => this.fetchLists()));
        } catch (e) {
          if (e.signIn || (!this.lists.length && !this.browse.url)) throw e;
        }
      }
      return this.lists;
    }

    // ---- Sign-in (upstream's PluginCatalogActivity: loadToken, refreshCredentialToken, beginAuth, pollAuth) ----

    signInKind() {
      return signInKind(this.m);
    }

    tokenFile() {
      return pluginFile(this.m.token && this.m.token.file, this.dir);
    }

    // {token} from token.file, which a sign-in here or the plugin's web card wrote. With none, a password grant mints
    // one; otherwise a sign-in is needed.
    async ensureToken() {
      if (!this.tokenFile()) return;
      if (await this.loadToken()) return;
      if (this.signInKind() === 'password' && (await this.mintPassword())) return;
      throw signInNeeded('this catalog needs a sign-in');
    }

    async loadToken() {
      const raw = await this.serial(() => this.io.readFile(this.tokenFile()));
      let token = '';
      try {
        token = raw ? text(getPath(JSON.parse(raw), this.m.token.path || 'token')) : '';
      } catch (e) { /* upstream reads only a JSON token file */ }
      this.vars.token = token;
      return !!token;
    }

    async saveToken(value) {
      this.vars.token = value;
      const file = this.tokenFile();
      if (!file) return;
      const json = tokenJson(this.m.token.path, value);
      await this.serial(() => this.api.writeFile(file, new Blob([json], { type: 'application/json' })));
    }

    // A password grant: auth.request with the config's credentials, the token at auth.token_path. False when the
    // service turns it down or answers no token.
    async mintPassword() {
      const a = this.m.auth;
      const r = a.request;
      let res;
      try {
        res = await this.relayRaw((r.method || 'POST').toUpperCase(), resolveTemplate(r.url, this.vars),
                                  resolveHeaders(r.headers, this.vars), resolveTemplate(r.body || '', this.vars));
      } catch (e) {
        if (e && e.result && e.result.host) throw e;  // a host the user did not allow
        return false;
      }
      if (res.status < 200 || res.status >= 300) return false;
      let token = '';
      try {
        token = text(getPath(JSON.parse(res.body), a.token_path || 'access_token'));
      } catch (e) {
        return false;
      }
      if (!token) return false;
      await this.saveToken(token);
      return true;
    }

    // Runs request(); when it is refused for its sign-in and the catalog has a password grant, mints a new token and
    // runs it once more. request() must fill its templates when called, so the retry carries the new token.
    async authorized(request) {
      try {
        return await request();
      } catch (e) {
        if (!e.signIn || this.signInKind() !== 'password' || !(await this.mintPassword())) throw e;
        return request();
      }
    }

    // A device-code sign-in: auth.request answers a code to enter at a web address; auth.poll, with {device_code}, is
    // asked every interval seconds (at least 3, 5 when unsaid) until it answers a token, which is saved to token.file.
    // slow_down adds 5 s; access_denied, expired_token and the code's own expiry (at least 60 s, 900 when unsaid) end
    // it. show({code, url}) is called once the code is known.
    async deviceSignIn(show) {
      const a = this.m.auth;
      const r = a.request;
      const res = await this.relayRaw((r.method || 'POST').toUpperCase(), resolveTemplate(r.url, this.vars),
                                      resolveHeaders(r.headers, this.vars), resolveTemplate(r.body || '', this.vars));
      let json = null;
      try {
        json = JSON.parse(res.body);
      } catch (e) { /* below */ }
      if (res.status < 200 || res.status >= 300 || !json) throw new Error('the sign-in service answered ' + res.status);
      const code = text(getPath(json, a.code_path || 'user_code'));
      const url = text(getPath(json, a.verify_url_path || 'verification_uri'));
      const device = text(getPath(json, a.device_code_path || 'device_code'));
      if (!code || !device) throw new Error('the sign-in service gave no code');
      let interval = Math.max(3, Number(getPath(json, a.interval_path || 'interval')) || 5) * 1000;
      const deadline = this.now() + Math.max(60, Number(getPath(json, a.expires_path || 'expires_in')) || 900) * 1000;
      this.cancelled = false;
      show({ code, url });
      const p = a.poll || {};
      for (;;) {
        await this.wait(interval);
        if (this.cancelled) throw new Error('the sign-in was cancelled');
        if (this.now() >= deadline) throw new Error('the code expired; sign in again');
        const v = Object.assign({}, this.vars, { device_code: device });
        let answer;
        try {
          answer = await this.relayRaw((p.method || 'POST').toUpperCase(), resolveTemplate(p.url, v), resolveHeaders(p.headers, v),
                                       resolveTemplate(p.body || '', v));
        } catch (e) {
          if (e && e.result && e.result.host) throw e;  // a host the user did not allow
          continue;  // the device could not reach the service this time: keep asking
        }
        let reply = null;
        try {
          reply = JSON.parse(answer.body);
        } catch (e) {
          continue;
        }
        const token = text(getPath(reply, a.token_path || 'access_token'));
        if (token) {
          await this.saveToken(token);
          return;
        }
        const error = text(getPath(reply, a.error_path || 'error'));
        if (error === 'slow_down') interval += 5000;
        if (error === 'access_denied') throw new Error('the sign-in was declined');
        if (error === 'expired_token') throw new Error('the code expired; sign in again');
        // authorization_pending, or anything else: keep asking
      }
    }

    // One catalog request, redirects followed: the relay answers a 3xx as it is (upstream's contract for plugins),
    // where upstream's own catalog screen follows it. A hop to another site drops the headers; a host the plugin may
    // not reach yet is asked about, as any is.
    async relayRaw(method, url, headers, body) {
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
      return res;
    }

    // relayRaw() for a JSON answer: a 401 or 403 is a sign-in error, any other non-2xx an error.
    async relay(method, url, headers, body) {
      const res = await this.relayRaw(method, url, headers, body);
      if (res.status === 401 || res.status === 403) {
        throw signInNeeded('the catalog refused the request (' + res.status + ')');
      }
      if (res.status < 200 || res.status >= 300) throw new Error('the catalog answered ' + res.status);
      try {
        return JSON.parse(res.body);
      } catch (e) {
        throw new Error('the catalog did not answer JSON');
      }
    }

    // browse.lists_url's lists, as upstream reads them: title, url and id from the lists_fields paths, requires from
    // lists_fields.requires (default "requires"), body and notice by those names. An entry needs a title and a url or
    // an id; one with only an id browses browse.url with {list_id}. No usable entry counts as a failed fetch.
    async fetchLists() {
      const b = this.browse;
      const url = resolveTemplate(b.lists_url, this.vars);
      const json = await this.relay('GET', url, resolveHeaders(b.lists_headers, this.vars), '');
      const array = getPath(json, b.lists_items || 'lists');
      const f = Object.assign({ title: 'title', url: 'url', id: 'id', requires: 'requires' }, b.lists_fields || {});
      const lists = [];
      for (const raw of Array.isArray(array) ? array : []) {
        const entry = {
          title: text(getPath(raw, f.title)), url: joinServerPath(getPath(raw, f.url), url), id: text(getPath(raw, f.id)),
          requires: getPath(raw, f.requires)
        };
        const body = getPath(raw, 'body');
        const notice = getPath(raw, 'notice');
        if (typeof body === 'string') entry.body = body;
        if (notice && typeof notice === 'object') entry.notice = notice;
        if (!entry.title || (!entry.url && !entry.id) || !listAllowed(entry.requires)) continue;
        if (!entry.url) delete entry.url;
        lists.push(entry);
      }
      if (!lists.length) throw new Error('the catalog offered no lists');
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

      const body = req.body === undefined ? '' : typeof req.body === 'string' ? req.body : JSON.stringify(req.body);
      const json = await this.authorized(() => {
        const vars = Object.assign({}, this.vars, this.listVars(), {
          page: this.page, offset: (this.page - 1) * size, limit: mode === 'page-limit' ? size : size + 1,
          query: enc(this.query), query_raw: this.query
        });
        return this.relay(req.method, resolveTemplate(req.url, vars), resolveHeaders(req.headers, vars),
                          resolveTemplate(body, vars));
      });
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

      // Filled for each attempt, so a retry after a new sign-in carries the new token.
      const headersNow = () => {
        const v = this.itemVars(row);
        const headers = resolveHeaders(dl.headers, v);
        if (dl.username || dl.password) {
          const pair = utf8(resolveTemplate(dl.username || '', v) + ':' + resolveTemplate(dl.password || '', v));
          headers.Authorization = 'Basic ' + btoa(String.fromCharCode(...pair));
        }
        return headers;
      };
      let url = resolveTemplate(dl.url || '{url}', vars);
      let method = (dl.method || 'GET').toUpperCase();
      let body = dl.body === undefined ? '' : resolveTemplate(dl.body, vars);
      let sendHeaders = true;
      if (dl.url_path) {
        const apiUrl = url;
        const apiBody = body;
        const answer = text(getPath(await this.authorized(() => this.relay(method, apiUrl, headersNow(), apiBody)), dl.url_path));
        if (!answer) throw new Error('the catalog named no file to download');
        // A server path is on the API's host and gets the headers; a full URL may be pre-signed and gets none.
        sendHeaders = answer.startsWith('/') && !answer.startsWith('//');
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
      await this.authorized(() => this.fetchFile(url, dest, sendHeaders ? headersNow() : {}, { method, body }));
      return [dest];
    }

    // fetchToSd, with the file server's 401 or 403 (which /api/fetch reports) as a sign-in error.
    async fetchFile(url, dest, headers, options) {
      try {
        return await this.serial(() => this.api.fetchToSd(escapeUrl(url), dest, headers, options));
      } catch (e) {
        const r = e && e.result;
        if (r && !r.host && (r.unauthorized || r.status === 401 || r.status === 403)) {
          throw signInNeeded('the server refused the request' + (r.status ? ' (' + r.status + ')' : ''));
        }
        throw e;
      }
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
          await this.fetchFile(f.url, f.dest);
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
    const signInBox = el('div', { className: 'catalog-signin' });
    const status = el('p', { className: 'catalog-status', role: 'status' });
    const controls = el('div', { className: 'catalog-controls' });
    const rows = el('div', { className: 'catalog-rows' });
    const pager = el('div', { className: 'catalog-pager' });
    host.replaceChildren(
      el('div', { className: 'catalog-head' }, [
        el('h2', { textContent: text(manifest.title) || plugin.title || plugin.name }),
        el('button', {
          className: 'btn btn-small', textContent: 'Close',
          onclick: () => { catalog.cancelled = true; if (onClose) onClose(); }
        })
      ]),
      text(manifest.description) ? el('p', { className: 'card-intro', textContent: manifest.description }) : '',
      signInBox, controls, rows, pager, status);
    const say = (message, kind) => {
      status.textContent = message || '';
      status.className = 'catalog-status' + (kind ? ' status-' + kind : '');
    };
    const failure = (e) => ((e && e.message) || String(e)).replace(/^./, (c) => c.toUpperCase());

    // What a sign-in error calls for: a device-code sign-in runs here; anything else is the plugin's web card's to do.
    // Resolves true once signed in. A catalog gets two sign-ins per opening, so a token the service keeps refusing
    // ends in a message, not a loop.
    let signIns = 0;
    const MOUNTS = { files: 'File Manager', settings: 'Settings' };
    const signIn = async (e) => {
      const why = failure(e);
      const kind = catalog.signInKind();
      if (kind === 'password') {
        say(why + '. Check the account details on its card on the ' + (MOUNTS[plugin.mount] || 'Settings') + ' page.', 'err');
        return false;
      }
      if (kind !== 'device_code') {
        if (!catalog.tokenFile()) {
          say(why + '.', 'err');  // a catalog without a sign-in: nothing to offer
          return false;
        }
        say(why + '. ' + (plugin.web === false ? 'Nothing on this page can sign it in.'
          : 'Sign in on its card on the ' + (MOUNTS[plugin.mount] || 'Settings') + ' page, then open the catalog again.'), 'err');
        return false;
      }
      if (++signIns > 2) {
        say(why + '.', 'err');
        return false;
      }
      say(why + '. Asking for a sign-in code...', 'busy');
      try {
        await catalog.deviceSignIn(({ code, url }) => {
          const link = /^https?:\/\//i.test(url) ? el('a', { href: url, target: '_blank', rel: 'noopener', textContent: url }) : '';
          signInBox.replaceChildren(
            el('p', {}, ['To sign in, open ', link || 'the service\'s sign-in page', ' and enter this code:']),
            el('p', { className: 'catalog-code', textContent: code }),
            el('button', { className: 'btn btn-small', textContent: 'Cancel', onclick: () => { catalog.cancelled = true; } }));
          say('Waiting for the sign-in...', 'busy');
        });
      } catch (err) {
        signInBox.replaceChildren();
        say(failure(err), 'err');
        return false;
      }
      signInBox.replaceChildren();
      say('Signed in.', 'ok');
      return true;
    };

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
        if (ticket !== loading) return;
        if (e.signIn && (await signIn(e))) {
          showPage();
          return;
        }
        if (!e.signIn) say(failure(e), 'err');
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
          if (e.signIn && (await signIn(e))) {
            say('Signed in. Download ' + row.title + ' again.', 'ok');
          } else if (!e.signIn) {
            say('Could not download ' + row.title + ': ' + ((e && e.message) || e), 'err');
          }
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

    for (;;) {
      say('Opening...', 'busy');
      try {
        await catalog.start();
        break;
      } catch (e) {
        if (e.signIn && (await signIn(e))) continue;
        if (!e.signIn) say(failure(e), 'err');
        return catalog;
      }
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
      cardFolder, joinServerPath, tokenJson, signInKind
    }
  };
})();
