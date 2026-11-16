#!/usr/bin/env node
// Tests the Plugins page's community catalog (src/network/html/PluginsPage.html): CrossPoint's Plugin Hub listed beside
// our own catalog, each entry marked with what of it runs on this firmware (communityStatus()), the plugin stores left
// out, an entry sharing a folder name with one of ours flagged, and an install fetching the files from the entry's own
// base and passing them to the device's install endpoints.
//
// The page runs in jsdom with its includes resolved; the device, GitHub and the community catalog are a fake fetch.
// The catalog entries are made up for the test: the Hub's repository has no license, so none of its content is here.
//
//   node test/web/plugins_catalog_test.mjs <dir with node_modules/jsdom>
// (the folder CI installs jsdom into, see .github/workflows/ci.yml). Exit status 0: every check passed.

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const deps = process.argv[2];
if (!deps) {
  console.error('usage: node test/web/plugins_catalog_test.mjs <dir with node_modules/jsdom>');
  process.exit(2);
}
const { JSDOM, VirtualConsole } = createRequire(path.join(path.resolve(deps), 'noop.js'))('jsdom');

const HTML_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'src', 'network', 'html');

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
  return ok;
}

process.on('unhandledRejection', () => {});

function resolveIncludes(html) {
  for (let depth = 0; depth < 8; depth++) {
    let count = 0;
    html = html.replace(/<!--\s*#include\s+file="([^"]+)"\s*-->/g, (_, rel) => {
      count++;
      return fs.readFileSync(path.join(HTML_DIR, rel), 'utf8');
    });
    if (!count) break;
  }
  return html;
}

const COMMUNITY = 'https://raw.githubusercontent.com/crosspoint-reader/crosspoint-plugin-hub/main/catalog.json';
const OWN = 'https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/plugins/catalog.json';
const HUB_BASE = 'https://raw.githubusercontent.com/someone/web-only/v1/';

const ownCatalog = { plugins: [
  { name: 'organize-by-author', title: 'Organize by Author', version: '1.0.0', mount: 'files', files: ['plugin.js'] }
] };
const crosspoint = { firmware_family: 'crosspoint', plugin_api_min: 1 };
const communityCatalog = { name: 'test hub', plugins: [
  { name: 'web-only', title: 'Web only', author: 'A', version: '1.0.0', repository: 'someone/web-only',
    base: HUB_BASE, files: ['manifest.json', 'plugin.js'], compatibility: crosspoint },
  { name: 'both-parts', title: 'Both parts', version: '2.0.0', base: 'https://example.org/b/',
    files: ['manifest.json', 'device.json', 'plugin.js'], compatibility: crosspoint },
  { name: 'screens-only', title: 'Screens only', version: '1.0.0', base: 'https://example.org/c/',
    files: ['manifest.json', 'device.json', 'README.md'], compatibility: crosspoint },
  { name: 'needs-transfer', title: 'Needs transfer', version: '1.0.0', base: 'https://example.org/d/',
    files: ['manifest.json', 'plugin.js'], compatibility: { ...crosspoint, requires: 'transfer.localsend.v1' } },
  { name: 'needs-catalog', title: 'Needs catalog', version: '1.0.0', base: 'https://example.org/e/',
    files: ['manifest.json', 'plugin.js', 'device.json'],
    compatibility: { ...crosspoint, requires: 'catalog.lists,catalog.server_paths' } },
  { name: 'other-firmware', title: 'Other firmware', version: '1.0.0', base: 'https://example.org/f/',
    files: ['plugin.js'], compatibility: { firmware_family: 'elsewhere' } },
  { name: 'protected-content', title: 'Protected', version: '1.0.5', base: 'https://example.org/g/',
    files: ['manifest.json', 'plugin.js'] },
  { name: 'pluginhub', title: 'Plugin Hub', version: '1.0.0', base: 'https://example.org/h/', files: ['device.json'] },
  { name: 'plugin-store', title: 'Plugin Store', version: '1.0.0', base: 'https://example.org/i/', files: ['plugin.js'] },
  { name: 'organize-by-author', title: 'Organize by Author (CrossPoint)', version: '1.0.1', base: 'https://example.org/j/',
    files: ['manifest.json', 'plugin.js'], compatibility: crosspoint }
] };

const requests = [];
const reply = (status, body) => ({
  ok: status >= 200 && status < 300, status,
  json: async () => (typeof body === 'string' ? JSON.parse(body) : body),
  text: async () => (typeof body === 'string' ? body : JSON.stringify(body)),
  arrayBuffer: async () => new TextEncoder().encode(typeof body === 'string' ? body : JSON.stringify(body)).buffer
});

const html = resolveIncludes(fs.readFileSync(path.join(HTML_DIR, 'PluginsPage.html'), 'utf8'));
let confirmAnswer = true;
const confirmed = [];
const dom = new JSDOM(html, {
  url: 'http://reader.test/plugins',
  runScripts: 'dangerously',
  virtualConsole: new VirtualConsole(),
  beforeParse(w) {
    w.TextDecoder = TextDecoder;
    w.confirm = (m) => { confirmed.push(m); return confirmAnswer; };
    w.fetch = async (url, options) => {
      const u = String(url);
      requests.push({ url: u, method: (options && options.method) || 'GET' });
      if (u === '/api/plugins') return reply(200, []);
      if (u === '/api/plugins/hosts') return reply(200, {});
      if (u === '/api/web/status') return reply(200, { repo: 'jpirnay/witchhunt-reader' });
      if (u === OWN) return reply(200, ownCatalog);
      if (u === COMMUNITY) return reply(200, communityCatalog);
      if (u === HUB_BASE + 'manifest.json') return reply(200, { title: 'Web only', mount: 'files' });
      if (u === HUB_BASE + 'plugin.js') return reply(200, 'CrossPoint.registerPlugin(function () {});');
      if (u.startsWith('/api/plugins/')) return reply(200, { ok: true });
      return reply(404, { error: 'unexpected ' + u });
    };
  }
});
const w = dom.window;
const settle = () => new Promise((r) => setTimeout(r, 50));
await settle();

const doc = w.document;
check(doc.querySelectorAll('#catalog .plugin').length === 1, 'our own catalog shows first');
check(!requests.some((r) => r.url === COMMUNITY), 'the community catalog is not fetched until asked for');

const community = doc.querySelector('input[name="catalog-source"][value="community"]');
community.checked = true;
community.dispatchEvent(new w.Event('change'));
await settle();

check(!doc.getElementById('community-note').hidden, 'the community note shows');
const rows = [...doc.querySelectorAll('#catalog .plugin')];
const row = (title) => rows.find((r) => r.querySelector('strong').textContent.startsWith(title));
const text = (r) => (r ? r.textContent : '');
const hasButton = (r) => !!(r && r.querySelector('button'));

check(rows.length === 8, 'every entry but the two plugin stores is listed (' + rows.length + ')');
check(!row('Plugin Hub') && !row('Plugin Store'), 'the plugin stores are left out');
check(text(row('Web only')).includes('runs here') && hasButton(row('Web only')), 'a web-only plugin runs here');
check(text(row('Web only')).includes('From github.com/someone/web-only'), 'its source repository is named');
check(text(row('Both parts')).includes('web page only') && hasButton(row('Both parts')),
      'a plugin with on-device screens installs for its web page');
check(text(row('Needs catalog')).includes('web page only') && hasButton(row('Needs catalog')),
      'a catalog requirement concerns only its on-device screens');
check(text(row('Screens only')).includes('does not run here') && !hasButton(row('Screens only')),
      'a plugin with only on-device screens is not offered for install');
check(text(row('Needs transfer')).includes('transfer.localsend.v1') && !hasButton(row('Needs transfer')),
      'a missing capability is named and blocks the install');
check(text(row('Other firmware')).includes('another firmware (elsewhere)') && !hasButton(row('Other firmware')),
      'a plugin for another firmware is not offered');
check(text(row('Protected')).includes('DRM') && !hasButton(row('Protected')), 'the DRM plugin says why it does not run');
check(text(row('Organize by Author (CrossPoint)')).includes("Witch Reader's Organize by Author"),
      'a shared folder name with one of ours is flagged');

// Install the web-only plugin: a confirmation, the files from its own base, then the device's install endpoints.
requests.length = 0;
confirmed.length = 0;
row('Web only').querySelector('button').click();
await settle();
await settle();
check(confirmed.length === 1 && confirmed[0].includes('community plugin') && confirmed[0].includes('not checked'),
      'a community install is confirmed first');
check(requests.some((r) => r.url === HUB_BASE + 'plugin.js') && requests.some((r) => r.url === HUB_BASE + 'manifest.json'),
      'the files are fetched from the entry\'s own base');
const device = requests.filter((r) => r.url.startsWith('/api/plugins/')).map((r) => r.url.split('?')[0]);
check(device[0] === '/api/plugins/begin' && device.includes('/api/plugins/file') && device.includes('/api/plugins/commit'),
      'and installed through begin, file and commit');
check(doc.getElementById('status').textContent.includes('File Manager'),
      'the status names the page from the plugin\'s own manifest (' + doc.getElementById('status').textContent + ')');

// A no to the confirmation installs nothing.
requests.length = 0;
confirmAnswer = false;
row('Both parts').querySelector('button').click();
await settle();
check(!requests.some((r) => r.url.startsWith('/api/plugins/begin')), 'a no installs nothing');

dom.window.close();
console.log(failures ? failures + ' check(s) failed' : 'all checks passed');
process.exit(failures ? 1 : 0);
