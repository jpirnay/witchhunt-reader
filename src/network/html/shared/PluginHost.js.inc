// --- browser-side plugin host (shared by Settings + File Manager) -----------
// Ported from crosspoint-reader PR #2734 ("feat: Add browser-side plugin system
// with SD card support", Justin Mitchell / @itsthisjustin). The discovery flow,
// the registerPlugin handshake and the mount convention are his design; this
// version drops the device-capability API (relay/crypto/fetch/plugin-fs) and
// the plugin job queue, keeping only the loader.
//
// Discovers plugins from the SD card via /api/plugins, loads each plugin.js,
// and hands it a container element to render into. A plugin is just JS on the
// card, so adding one needs no firmware build.
//
// Plugins run in the page with full same-origin authority: they call the web
// server's existing endpoints (/api/files, /mkdir, /move, /delete, /upload,
// /download) directly with fetch(). The firmware deliberately exposes no
// outbound-request or crypto endpoint - a plugin can reach the SD card, not
// the internet.
window.CrossPoint = window.CrossPoint || {
  _pending: null,
  // Called by a plugin at top level; loadPlugins() picks the function up after
  // that script finishes loading. Safe because plugins load strictly one at a
  // time (see loadPlugins), so _pending never belongs to two plugins at once.
  registerPlugin(fn) { this._pending = fn; }
};

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

    const container = document.createElement('div');
    container.className = 'card';
    container.id = 'plugin-' + p.name;
    host.appendChild(container);

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
      CrossPoint._pending(container, {
        name: p.name,
        // manifest.json's title, falling back to the folder name. The host
        // renders no chrome - the plugin owns its whole card - so this is here
        // for a plugin that wants its heading to track the manifest instead of
        // hardcoding it.
        title: p.title || p.name,
        // URL of another file in this plugin's folder, for a plugin that ships
        // its own assets. Flat: the server serves no subdirectories.
        pluginFile: (f) => '/plugin?name=' + encodeURIComponent(p.name) +
          '&file=' + encodeURIComponent(f)
      });
    } catch (e) {
      const title = document.createElement('h2');
      title.textContent = p.title || p.name;
      const msg = document.createElement('p');
      msg.className = 'plugin-error';
      msg.textContent = 'Failed to load plugin: ' + ((e && e.message) || e);
      container.replaceChildren(title, msg);
    }
  }
}
