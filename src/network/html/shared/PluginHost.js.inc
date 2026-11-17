// --- browser-side plugin host (shared by Settings + File Manager) -----------
// Ported from crosspoint-reader PR #2734 ("feat: Add browser-side plugin system
// with SD card support", Justin Mitchell / @itsthisjustin). The discovery flow,
// the registerPlugin handshake and the mount convention are his design; this
// version leaves out crypto (the browser's crypto.subtle covers it) and the
// plugin job queue.
//
// Discovers plugins from the SD card via /api/plugins, loads each plugin.js,
// and hands it a container element to render into. A plugin is just JS on the
// card, so adding one needs no firmware build.
//
// Plugins run in the page with full same-origin authority: they call the web
// server's endpoints (/api/files, /mkdir, /move, /delete, /upload, /download)
// directly with fetch(). Beyond the card, the device fetches on a plugin's
// behalf through relay (/api/relay) and fetchToSd (POST /api/fetch), and
// writeFile (POST /api/plugin-fs) stores a small file. Both outbound calls reach
// only the hosts the plugin's manifest lists in "allowedHosts" or the user
// approved for it (POST /api/plugins/hosts): a refused host is named, and the
// host asks the user before it tries again.
//
// Plugins written for upstream crosspoint-reader run too: api.dir, the
// relay(method, url, headers, body) form answering {status, headers, body},
// writeFile(path, base64) and fetchToSd(url, dest, headers, {method, body}) follow
// its plugin-host.js (#3114, Justin Mitchell, and later PRs). registerAction()
// only warns: there is no job queue here.
window.CrossPoint = window.CrossPoint || {
  _pending: null,
  // Called by a plugin at top level; loadPlugins() picks the function up after
  // that script finishes loading. Safe because plugins load strictly one at a
  // time (see loadPlugins), so _pending never belongs to two plugins at once.
  registerPlugin(fn) { this._pending = fn; }
};

// Device capabilities this firmware implements. A plugin may declare what it
// needs as "requires" in its manifest, and one asking for something absent is
// refused with a readable card instead of failing somewhere inside its own code.
//
// "crypto" is deliberately absent: the browser's own crypto.subtle covers
// hashing, HMAC, AES and RSA, so a device-side implementation would duplicate it.
const PLUGIN_CAPABILITIES = ['relay', 'fetchToSd', 'writeFile', 'pluginFile', 'dir'];

// Raises the device's error, with its JSON answer (and a refused host's name) attached.
async function pluginApiError(r, path) {
  let result = null;
  try { result = await r.json(); } catch (e) { /* not JSON */ }
  const err = new Error(path + ' ' + r.status + (result && result.error ? ': ' + result.error : ''));
  err.status = r.status;
  if (result) err.result = result;
  return err;
}

// Runs request(), and when the device refuses the host, asks the user once whether this plugin may reach it; a yes
// is stored on the device (per plugin, kept across updates) and the request runs again.
async function withHostApproval(plugin, request) {
  let r = await request();
  if (r.status !== 403) return r;
  let refused = null;
  try { refused = await r.clone().json(); } catch (e) { return r; }
  if (!refused || !refused.host) return r;
  const label = plugin.title || plugin.name;
  if (!window.confirm('Allow the plugin "' + label + '" to connect to ' + refused.host + '?\n\n' +
                      'The device will reach this host on the plugin\'s behalf. You can take this back on the Plugins page.')) {
    return r;
  }
  const saved = await fetch('/api/plugins/hosts', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ plugin: plugin.name, host: refused.host, allow: true })
  });
  if (!saved.ok) throw await pluginApiError(saved, '/api/plugins/hosts');
  return request();
}

// writeFile() data: a string is base64, as upstream's API defines it; a Blob or ArrayBuffer is sent as it is.
function pluginFileBody(data) {
  if (typeof data !== 'string') return data;
  return new Blob([Uint8Array.from(atob(data), (c) => c.charCodeAt(0))]);
}

function missingCapabilities(plugin) {
  const required = Array.isArray(plugin.requires) ? plugin.requires : [];
  return required.filter((cap) => PLUGIN_CAPABILITIES.indexOf(cap) === -1);
}

// `mount` selects which page's plugins to load, matching the manifest's "mount"
// field ("settings" or "files"); plugins omitting it default to "settings".
//
// Strictly sequential: the device serves one connection at a time, and parallel
// requests on page load can stall a response long enough to trip the firmware
// watchdog. Await this alongside the page's other loaders, never beside them.
async function loadPlugins(mount) {
  const host = document.getElementById('plugin-container');
  if (!host) return;

  let list;
  try {
    list = await (await fetch('/api/plugins')).json();
  } catch (e) {
    return;  // no card, no plugins folder, or a device busy elsewhere
  }

  for (const p of list) {
    if ((p.mount || 'settings') !== mount) continue;
    if (p.disabled) continue;  // switched off on the Plugins page; still installed

    const container = document.createElement('div');
    container.className = 'card';
    container.id = 'plugin-' + p.name;
    host.appendChild(container);

    const missing = missingCapabilities(p);
    if (missing.length) {
      showPluginError(container, p, 'needs ' + missing.join(', ') +
        ', which this firmware does not provide.');
      continue;
    }

    CrossPoint._pending = null;
    try {
      await new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = '/plugin?name=' + encodeURIComponent(p.name) + '&file=plugin.js';
        s.onload = resolve;
        s.onerror = () => reject(new Error('load ' + p.name));
        document.body.appendChild(s);
      });
      if (typeof CrossPoint._pending !== 'function') {
        throw new Error(p.name + ' did not call registerPlugin()');
      }
      CrossPoint._pending(container, makePluginApi(p));
    } catch (e) {
      showPluginError(container, p, 'failed to load: ' + ((e && e.message) || e));
    }
  }
}

// The api a plugin receives, for an entry of /api/plugins: what loadPlugins() hands a plugin.js, and what the Plugins
// page's catalog runner (js/plugin-catalog.js) uses for a device.json, so both reach the device the same way and through
// the same host approval.
function makePluginApi(p) {
  return {
    name: p.name,
    // The plugin's own folder on the card, where it may keep its settings and token: writeFile() may write there,
    // and /download?path=<dir>/<file> reads them back.
    dir: (p.root || '/.crosspoint/plugins') + '/' + p.name,
    // Fetch a URL the browser cannot reach itself, via the device, from a host this plugin may reach.
    //   relay(url): GET, redirects followed within those hosts; resolves to a Response.
    //   relay(method, url, headers, body): upstream's form; resolves to {status, headers: [[name, value]...],
    //   body} with the body as text, and a 3xx comes back unfollowed. Headers make the request a credentialed
    //   one: the device verifies the server's certificate.
    relay: async (...args) => {
      if (args.length <= 1) {
        const url = args[0];
        const r = await withHostApproval(p, () => fetch('/api/relay?plugin=' + encodeURIComponent(p.name) +
          '&url=' + encodeURIComponent(url)));
        if (!r.ok) throw new Error('relay ' + r.status + ': ' + (await r.text().catch(() => '')));
        return r;
      }
      const [method, url, headers, body] = args;
      const r = await withHostApproval(p, () => fetch('/api/relay', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ plugin: p.name, method, url, headers: headers || {}, body: body || '' })
      }));
      if (!r.ok) throw await pluginApiError(r, '/api/relay');
      // Header values arrive as Latin-1 code units: re-decode the UTF-8 JSON.
      const raw = r.headers.get('X-Relay-Headers') || '[]';
      const list = JSON.parse(new TextDecoder().decode(Uint8Array.from(raw, (c) => c.charCodeAt(0))));
      let text;
      try {
        text = await r.text();
      } catch (e) {
        const err = new Error('/api/relay 502: relay failed; large bodies need fetchToSd');
        err.status = 502;
        throw err;
      }
      return { status: Number(r.headers.get('X-Relay-Status')), headers: list, body: text };
    },
    // Download straight to the card - one transfer instead of relaying
    // through the browser and uploading the bytes back. headers and
    // {method, body} are upstream's; the answer reports the whole file.
    fetchToSd: async (url, dest, headers, options) => {
      const method = ((options && options.method) || 'GET').toUpperCase();
      const r = await withHostApproval(p, () => fetch('/api/fetch', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ plugin: p.name, url, dest, headers: headers || {}, method,
                               body: (options && options.body) || '' })
      }));
      if (!r.ok) throw await pluginApiError(r, '/api/fetch');
      return r.json();
    },
    // Write one small file, in the plugin's own folder or where /upload may write. `data` is base64 when a
    // string (upstream's API), or a Blob or ArrayBuffer.
    writeFile: async (path, data) => {
      const r = await fetch('/api/plugin-fs?plugin=' + encodeURIComponent(p.name) +
        '&path=' + encodeURIComponent(path),
        { method: 'POST', body: uploadForm(pluginFileBody(data)) });
      if (!r.ok) throw await pluginApiError(r, '/api/plugin-fs');
      return r.json();
    },
    // Upstream's job queue does not exist here; a plugin that registers actions still loads.
    registerAction: (action) => {
      console.warn('Plugin ' + p.name + ': registerAction(' + action + ') is not supported on this firmware');
    },
    // Present so a plugin written against the upstream API fails with a
    // sentence rather than "api.crypto is not a function".
    crypto: () => {
      throw new Error('api.crypto is not available on this firmware - use the ' +
        "browser's crypto.subtle instead");
    },
    // manifest.json's title, falling back to the folder name. The host
    // renders no chrome - the plugin owns its whole card - so this is here
    // for a plugin that wants its heading to track the manifest instead of
    // hardcoding it.
    title: p.title || p.name,
    // URL of another file in this plugin's folder, for a plugin that ships
    // its own assets. Flat: the server serves no subdirectories.
    pluginFile: (f) => '/plugin?name=' + encodeURIComponent(p.name) +
      '&file=' + encodeURIComponent(f),
    // Folder the File Manager is showing, so a plugin can act on what the
    // user is already looking at instead of asking them to type a path
    // again. A getter, not a captured value, so it stays right if the page
    // ever navigates in place rather than reloading. Reads the host page's
    // global; typeof keeps it safe on Settings, which has no file browser
    // and where this is always '/'.
    get currentPath() {
      return typeof currentPath === 'string' ? currentPath : '/';
    }
  };
}

function showPluginError(container, plugin, detail) {
  const title = document.createElement('h2');
  title.textContent = plugin.title || plugin.name;
  const msg = document.createElement('p');
  msg.className = 'plugin-error';
  msg.textContent = 'This plugin ' + detail;
  container.replaceChildren(title, msg);
}
