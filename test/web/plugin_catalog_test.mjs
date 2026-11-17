#!/usr/bin/env node
// Tests the device.json catalog runner (src/network/html/js/plugin-catalog.js) on the Plugins page: its pure helpers
// (template variables, the four paging modes, filename sanitizing, card folders, compatibility), then whole catalogs
// opened from the installed list: offset paging with lists and search, a {page} catalog installing a bundle, a static
// catalog paged after compatibility filtering behind a remembered notice and a config file, and a sign-in catalog that
// says it cannot open.
//
// The page runs in jsdom with its includes resolved, and loads plugin-catalog.js from the source tree. The device
// (its relay, /api/fetch, /mkdir, /download) and the catalogs' servers are a fake fetch; every manifest and answer is
// made up for the test.
//
//   node test/web/plugin_catalog_test.mjs <dir with node_modules/jsdom>
// (the folder CI installs jsdom into, see .github/workflows/ci.yml). Exit status 0: every check passed.

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const deps = process.argv[2];
if (!deps) {
  console.error('usage: node test/web/plugin_catalog_test.mjs <dir with node_modules/jsdom>');
  process.exit(2);
}
const { JSDOM, VirtualConsole, ResourceLoader } = createRequire(path.join(path.resolve(deps), 'noop.js'))('jsdom');

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

class FakeResponse {
  constructor(status, body, headers) {
    this.status = status;
    this.ok = status >= 200 && status < 300;
    this._body = body;
    this._headers = headers || {};
    this.headers = { get: (name) => this._headers[name] ?? null };
  }
  async text() { return typeof this._body === 'string' ? this._body : JSON.stringify(this._body); }
  async json() { return typeof this._body === 'string' ? JSON.parse(this._body) : this._body; }
  clone() { return new FakeResponse(this.status, this._body, this._headers); }
}

// ---- The installed plugins and their device.json files --------------------------------------------------------------

const ROOT = '/.crosspoint/plugins';
const installed = [
  { name: 'news', title: 'News', root: ROOT, mount: 'settings', web: false, device: true },
  { name: 'dicts', title: 'Dictionaries', root: ROOT, mount: 'settings', web: false, device: true },
  { name: 'static', title: 'Static', root: ROOT, mount: 'settings', web: false, device: true },
  { name: 'signin', title: 'Sign-in', root: ROOT, mount: 'settings', web: false, device: true },
  { name: 'both', title: 'Both', root: ROOT, mount: 'files', web: true, device: true },
  { name: 'off', title: 'Switched off', root: ROOT, mount: 'settings', web: false, device: true, disabled: true }
];

const manifests = {
  news: {
    title: 'News', description: 'Stories, as text.',
    browse: {
      url: 'https://news.test/top?offset={offset}&length={limit}',
      headers: { 'X-Client': '{firmware_family}/{plugin_api}' },
      items: 'hits', page_size: 3,
      fields: { title: 'title', author: 'by', id: 'objectID', url: 'url' },
      lists: [{ title: 'Top' }, { title: 'New', url: 'https://news.test/new?offset={offset}&length={limit}' }],
      search: { url: 'https://news.test/search?q={query}&offset={offset}&length={limit}' }
    },
    download: { url: 'https://text.test/{url}', headers: { 'X-Return-Format': 'text' }, dest_dir: '/News', filename: '{title}.txt' }
  },
  dicts: {
    browse: { url: 'https://dict.test/all-{page}.json', items: 'items', page_size: 2, fields: { title: 'title', id: 'id' } },
    download: { dest_dir: '/.dictionaries', bundle: { base: 'base', files: 'files', subdir: '{id}' } }
  },
  static: {
    config: { file: 'config.json' },
    browse: {
      url: 'https://static.test/all.json', page_size: 2,
      fields: { title: 'name', id: 'id', url: 'link' },
      compatibility: { firmware_family: 'compat.family', requires: 'compat.requires' },
      lists: [{ title: 'Everything', notice: { title: 'Heads up', message: 'Community content.', id: 'community', once: true } }]
    },
    download: { url: '{cfg.server}{url}', dest_dir: '/Ignored' }
  },
  signin: {
    token: { file: 'token.json', path: 'token' },
    auth: { type: 'device_code', request: { url: 'https://signin.test/device' } },
    browse: { url: 'https://signin.test/books?page={page}&limit={limit}', headers: { Authorization: 'Bearer {token}' } }
  }
};

// ---- The catalogs' servers, reached through the relay ---------------------------------------------------------------

const hit = (title, id, by) => ({ title, objectID: id, by: by || 'someone', url: 'https://site.test/' + id + ' page' });
const internet = {
  'https://news.test/top?offset=0&length=4': { hits: [hit('A: "quoted"?', 'a'), hit('B', 'b'), { objectID: 'untitled' }, hit('D', 'd')] },
  'https://news.test/top?offset=3&length=4': { hits: [hit('D', 'd')] },
  // A redirect, which the relay answers unfollowed: the runner follows it.
  'https://news.test/new?offset=0&length=4': { redirect: 301, location: '/latest?offset=0&length=4' },
  'https://news.test/latest?offset=0&length=4': { hits: [hit('Fresh', 'f')] },
  'https://news.test/search?q=a%20b%2F%C3%BC&offset=0&length=4': { hits: [hit('Found', 'x')] },
  'https://dict.test/all-1.json': { items: [
    { title: 'German', id: 'de', base: 'https://dict.test/files/de', files: ['de.idx', 'de.dict', 'res/de.png'] },
    { title: 'Bad', id: 'bad', base: 'https://dict.test/files/bad/', files: ['../escape.idx'] },
    { title: 'Lookahead', id: 'next' }
  ] },
  'https://static.test/all.json': [
    { name: 'One', id: '1', link: '/books/1.epub' },
    { name: 'Other firmware', id: '2', compat: { family: 'elsewhere' } },
    { name: 'Two', id: '3', compat: { family: 'crosspoint', requires: 'catalog.lists' } },
    { name: 'Needs a dashboard', id: '4', compat: { requires: 'ui.dashboard.v1' } },
    { name: 'Three', id: '5' }
  ]
};

// ---- The device -----------------------------------------------------------------------------------------------------

const requests = [];
const relayed = [];
const fetched = [];
const folders = [];
const existing = new Set(['/News']);

function device(url, options) {
  const method = (options && options.method) || 'GET';
  const json = options && typeof options.body === 'string' ? JSON.parse(options.body) : null;
  requests.push({ url, method, json });
  if (url === '/api/plugins') return new FakeResponse(200, installed);
  if (url === '/api/plugins/hosts') return new FakeResponse(200, {});
  if (url === '/api/web/status') return new FakeResponse(200, { repo: 'jpirnay/witchhunt-reader' });
  if (url === '/api/status') return new FakeResponse(200, { version: '9.9-test' });
  const plugin = /^\/plugin\?name=([^&]+)&file=device\.json$/.exec(url);
  if (plugin) return manifests[plugin[1]] ? new FakeResponse(200, manifests[plugin[1]]) : new FakeResponse(404, 'no');
  if (url === '/download?path=' + encodeURIComponent(ROOT + '/static/config.json')) {
    return new FakeResponse(200, '{"server":"https://files.test","dest_dir":"Books"}');
  }
  if (url === '/mkdir' && method === 'POST') {
    const where = (options.body.get('path') === '/' ? '' : options.body.get('path')) + '/' + options.body.get('name');
    folders.push(where);
    if (existing.has(where)) return new FakeResponse(400, 'Folder already exists');
    existing.add(where);
    return new FakeResponse(200, 'Folder created');
  }
  if (url === '/api/relay' && method === 'POST') {
    relayed.push(json);
    const answer = internet[json.url];
    if (answer && answer.redirect) {
      return new FakeResponse(200, '', { 'X-Relay-Status': String(answer.redirect),
                                         'X-Relay-Headers': JSON.stringify([['Location', answer.location]]) });
    }
    return new FakeResponse(200, answer === undefined ? '' : JSON.stringify(answer),
                            { 'X-Relay-Status': answer === undefined ? '404' : '200', 'X-Relay-Headers': '[]' });
  }
  if (url === '/api/fetch' && method === 'POST') {
    fetched.push(json);
    if (!existing.has(json.dest.slice(0, json.dest.lastIndexOf('/')))) return new FakeResponse(502, { error: 'download failed' });
    return new FakeResponse(200, { ok: true, dest: json.dest });
  }
  return new FakeResponse(404, { error: 'unexpected ' + url });
}

class Loader extends ResourceLoader {
  fetch(url) {
    if (new URL(url).pathname === '/js/plugin-catalog.js') {
      return Promise.resolve(fs.readFileSync(path.join(HTML_DIR, 'js', 'plugin-catalog.js')));
    }
    return Promise.reject(new Error('no resource ' + url));
  }
}

let confirmAnswer = true;
const confirmed = [];
const dom = new JSDOM(resolveIncludes(fs.readFileSync(path.join(HTML_DIR, 'PluginsPage.html'), 'utf8')), {
  url: 'http://reader.test/plugins',
  runScripts: 'dangerously',
  resources: new Loader(),
  virtualConsole: new VirtualConsole(),
  beforeParse(w) {
    w.TextDecoder = TextDecoder;
    w.TextEncoder = TextEncoder;
    w.Element.prototype.scrollIntoView = () => {};
    w.confirm = (message) => { confirmed.push(message); return confirmAnswer; };
    w.fetch = async (url, options) => {
      const u = String(url);
      if (u.startsWith('https://raw.githubusercontent.com/')) return new FakeResponse(404, 'offline');
      return device(u, options);
    };
  }
});
const w = dom.window;
const doc = w.document;

async function until(condition, what) {
  for (let i = 0; i < 100; i++) {
    if (condition()) return true;
    await new Promise((r) => setTimeout(r, 20));
  }
  return check(false, 'timed out waiting for ' + what);
}
const view = () => doc.getElementById('catalog-view');
const buttons = (root) => [...root.querySelectorAll('button')];
const button = (root, label) => buttons(root).find((b) => b.textContent === label);
const titles = () => [...view().querySelectorAll('.catalog-rows strong')].map((s) => s.textContent);
const catalogStatus = () => view().querySelector('.catalog-status').textContent;
const installedRow = (title) => [...doc.querySelectorAll('#installed .plugin')]
  .find((r) => r.querySelector('strong').textContent.startsWith(title));
const lastRelay = () => relayed[relayed.length - 1];

await until(() => doc.querySelectorAll('#installed .plugin').length === installed.length, 'the installed list');

// ---- The installed list ---------------------------------------------------------------------------------------------

check(installedRow('News').textContent.includes('Adds a catalog you open here'), 'a device.json plugin says it adds a catalog');
check(installedRow('Both').textContent.includes('Adds a card on the File Manager page and a catalog you open here'),
      'a plugin with both parts names both');
check(!!button(installedRow('News'), 'Open catalog'), 'a catalog plugin has an Open catalog button');
check(!button(installedRow('Switched off'), 'Open catalog'), 'a switched-off one has none');
check(view().hidden, 'no catalog is open at first');

// ---- Loading the runner, a catalog it cannot open, and the pure helpers ---------------------------------------------

button(installedRow('Sign-in'), 'Open catalog').click();
await until(() => !!w.PluginCatalog, 'plugin-catalog.js to load');
await until(() => doc.getElementById('status').textContent.startsWith('Cannot open'), 'the sign-in refusal');
check(doc.getElementById('status').textContent.includes('needs a sign-in') && view().hidden,
      'a catalog that needs a sign-in says it cannot open (' + doc.getElementById('status').textContent + ')');

const P = w.PluginCatalog._internal;
check(P.enc('a b/ü~-_.') === 'a%20b%2F%C3%BC~-_.', '{query} and the _enc variables percent-encode all but -_.~');
check(P.escapeUrl('https://x.test/a b?q=ü&r=1') === 'https://x.test/a%20b?q=%C3%BC&r=1',
      'a URL is sent with spaces and non-ASCII escaped, the rest as written');
check(P.resolveTemplate('{a}-{cfg.b}-{missing}', { a: 1, 'cfg.b': 'x' }) === '1-x-{missing}',
      'templates fill known variables and leave unknown ones');

check(P.pagingMode({ url: 'u?o={offset}&l={limit}&p={page}' }) === 'offset', '{offset} takes precedence');
check(P.pagingMode({ url: 'u?p={page}&l={limit}' }) === 'page-limit', '{page} with {limit} is sized pages');
check(P.pagingMode({ url: 'u-{page}.json' }) === 'page', '{page} alone is the server\'s pages');
check(P.pagingMode({ url: 'u', body: '{"p":{page}}' }) === 'page', 'the body counts');
check(P.pagingMode({ url: 'u', headers: { Range: 'items={offset}' } }) === 'offset', 'so do the headers');
check(P.pagingMode({ url: 'u.json' }) === 'static', 'neither is one static answer');
check(P.pageSize({ page_size: 40 }) === 16 && P.pageSize({}) === 8 && P.pageSize({ page_size: 0 }) === 8,
      'page_size is 1..16, 8 by default');

check(P.sanitizeFilename('a/b\\c:d*e?f"g<h>i|j.epub') === 'a_b_c_d_e_f_g_h_i_j.epub', 'unsafe characters become _');
check(P.sanitizeFilename('  ..hidden.epub') === 'hidden.epub', 'leading spaces and dots go');
check(P.sanitizeFilename('name. . .') === 'name', 'trailing spaces and dots go');
check(P.sanitizeFilename('') === 'book' && P.sanitizeFilename('...') === 'book', 'nothing left is "book"');
const long = P.sanitizeFilename('x'.repeat(200) + '.epub');
check(long.length === 100 && long.endsWith('.epub'), 'a long name keeps its extension within 100 bytes');
const wide = P.sanitizeFilename('ü'.repeat(60) + '.txt');
check(wide === 'ü'.repeat(48) + '.txt', 'the limit counts UTF-8 bytes (' + wide.length + ' characters)');
check(P.sanitizeFilename('a\u0001b.txt') === 'ab.txt', 'control characters go');

check(P.cardFolder('', '/p') === '/' && P.cardFolder('/Books/', '/p') === '/Books', 'card folders: root and trimmed');
check(P.cardFolder('downloads', ROOT + '/x') === ROOT + '/x/downloads', 'a relative folder is in the plugin folder');
check(P.cardFolder('/.sleep', '/p') === '/sleep' && P.cardFolder('/.dictionaries/de', '/p') === '/dictionaries/de',
      'a hidden folder the firmware also reads visibly becomes the visible one');
check(P.cardFolder('/.sleepy', '/p') === '/.sleepy', 'other hidden folders stay');
check(P.cardFolder('/a/../b', '/p') === null, 'a folder that climbs is refused');

const C = (raw) => P.compatible(raw, { firmware_family: 'f', plugin_api_min: 'api', requires: 'r' });
check(P.compatible({}, undefined) && C({}) && C({ f: 'witchhunt' }) && C({ f: 'crosspoint', r: ['catalog.lists'] }),
      'compatible rows pass');
check(!C({ f: 'elsewhere' }) && !C({ r: 'catalog.lists,stats.native.v1' }) && !C({ api: 2 }),
      'another firmware, a missing capability or a newer plugin API do not');
check(P.listAllowed(undefined) && P.listAllowed(['catalog.notices']) && !P.listAllowed(['x']) &&
      !P.listAllowed('catalog.lists'), 'list requirements: all ours, as an array');
check(P.joinServerPath('/files/1', 'https://h.test:8443/api?x=1') === 'https://h.test:8443/files/1' &&
      P.joinServerPath('//cdn.test/a', 'https://h.test/') === '//cdn.test/a' &&
      P.joinServerPath('https://a.test/b', 'https://h.test/') === 'https://a.test/b', 'server paths join the request origin');

const D = w.PluginCatalog.describe;
check(D({ browse: { url: 'u' } }).catalog && D({ browse: { lists_url: 'u' } }).catalog, 'a JSON catalog opens');
check(!D({ browse: { url: 'u', format: 'xml' } }).catalog && D({ browse: { url: 'u', format: 'xml' } }).reason.includes('XML'),
      'an XML one says why not');
const events = D({ events: { book_finished: {} } });
check(!events.catalog && events.events && !events.reason, 'an events-only manifest has no catalog');

// ---- Offset paging, lists, search and a download --------------------------------------------------------------------

button(installedRow('News'), 'Open catalog').click();
await until(() => titles().length > 0, 'the first page of News');
check(!view().hidden && view().querySelector('h2').textContent === 'News', 'the catalog opens in its own card');
check(lastRelay().url === 'https://news.test/top?offset=0&length=4', 'the first list browses browse.url with a lookahead row');
check(lastRelay().plugin === 'news' && lastRelay().headers['X-Client'] === 'witchhunt/1',
      'through the plugin\'s relay, with its headers filled in');
check(titles().join('|') === 'A: "quoted"?|B', 'a row without a title still counts toward the page (' + titles() + ')');
check(!!button(view(), 'Next page') && !button(view(), 'Previous page'), 'the lookahead row offers a next page');
check(requests.some((r) => r.url === '/api/status'), 'the firmware version is read for {firmware_version}');

button(view(), 'Next page').click();
await until(() => titles().join('|') === 'D', 'page 2');
check(lastRelay().url === 'https://news.test/top?offset=3&length=4', 'page 2 starts at the page size');
check(!button(view(), 'Next page') && !!button(view(), 'Previous page'), 'the last page offers only the previous one');

button(view(), 'New').click();
await until(() => titles().join('|') === 'Fresh', 'the New list');
const hops = relayed.slice(-2);
check(hops[0].url === 'https://news.test/new?offset=0&length=4', 'a list browses its own url from page 1');
check(hops[1].url === 'https://news.test/latest?offset=0&length=4' && hops[1].headers['X-Client'] === 'witchhunt/1',
      'a redirect is followed, with the headers on the same site');
check(button(view(), 'New').className.includes('btn-primary'), 'the open list is marked');

const form = view().querySelector('.catalog-search');
form.querySelector('input').value = ' a b/ü ';
form.dispatchEvent(new w.Event('submit', { cancelable: true }));
await until(() => titles().join('|') === 'Found', 'the search');
check(lastRelay().url === 'https://news.test/search?q=a%20b%2F%C3%BC&offset=0&length=4', 'the search sends the query encoded');

button(view(), 'Top').click();
await until(() => titles()[0] === 'A: "quoted"?', 'the Top list again');
folders.length = 0;
view().querySelector('.catalog-rows .plugin button').click();
await until(() => catalogStatus().startsWith('Saved'), 'the download');
const story = fetched[fetched.length - 1];
check(story.url === 'https://text.test/https://site.test/a%20page', 'the download URL is filled in and escaped (' + story.url + ')');
check(story.dest === '/News/A_ _quoted__.txt', 'the file name is sanitized (' + story.dest + ')');
check(story.headers['X-Return-Format'] === 'text' && story.method === 'GET', 'with the download headers');
check(folders.join('|') === '/News', 'the folder is made first (it existed: ' + folders + ')');

// ---- A {page} catalog and a bundle ----------------------------------------------------------------------------------

button(view(), 'Close').click();
check(view().hidden && !view().children.length, 'Close empties and hides the card');
button(installedRow('Dictionaries'), 'Open catalog').click();
await until(() => titles().length > 0, 'the dictionaries');
check(lastRelay().url === 'https://dict.test/all-1.json', '{page} alone fetches the server\'s page');
check(titles().join('|') === 'German|Bad' && !!button(view(), 'Next page'), 'a row beyond the page size offers a next page');

fetched.length = 0;
folders.length = 0;
view().querySelector('.catalog-rows .plugin button').click();
await until(() => catalogStatus().startsWith('Saved'), 'the bundle');
check(fetched.map((f) => f.dest).join('|') === '/dictionaries/de/de.idx|/dictionaries/de/de.dict|/dictionaries/de/res/de.png',
      'every bundle file lands under dest_dir/<id>, the hidden folder made visible');
check(fetched[0].url === 'https://dict.test/files/de/de.idx', 'from the row\'s base, with a slash added');
check(folders.includes('/dictionaries') && folders.includes('/dictionaries/de/res'), 'with every folder made on the way');
check(catalogStatus() === 'Saved 3 files in /dictionaries/de', 'the status says where (' + catalogStatus() + ')');

fetched.length = 0;
view().querySelectorAll('.catalog-rows .plugin button')[1].click();
await until(() => catalogStatus().startsWith('Could not'), 'the unsafe bundle');
check(catalogStatus().includes('unsafe file') && !fetched.length, 'a bundle file that climbs is refused before any download');

// ---- A static catalog behind a notice, with a config file -----------------------------------------------------------

button(view(), 'Close').click();
confirmed.length = 0;
relayed.length = 0;
button(installedRow('Static'), 'Open catalog').click();
await until(() => titles().length > 0, 'the static catalog');
check(confirmed.length === 1 && confirmed[0].startsWith('Heads up') && confirmed[0].includes('Community content.'),
      'a single list with a notice asks first');
check(titles().join('|') === 'One|Two', 'rows for another firmware or a missing capability are left out before paging');
button(view(), 'Next page').click();
await until(() => titles().join('|') === 'Three', 'page 2 of the static catalog');
check(relayed.length === 1, 'a static answer is fetched once and paged here');
fetched.length = 0;
button(view(), 'Previous page').click();
await until(() => titles()[0] === 'One', 'page 1 again');
view().querySelector('.catalog-rows .plugin button').click();
await until(() => catalogStatus().startsWith('Saved'), 'the static download');
check(fetched[0].url === 'https://files.test/books/1.epub', '{cfg.*} comes from the plugin\'s config file');
check(fetched[0].dest === '/Books/One.epub', 'the config\'s dest_dir wins, and the file name defaults to {title}.epub');

button(view(), 'Close').click();
confirmed.length = 0;
button(installedRow('Static'), 'Open catalog').click();
await until(() => titles().length > 0, 'the static catalog again');
check(confirmed.length === 0, 'a notice with once and an id is not asked again');

dom.window.close();
console.log(failures ? failures + ' check(s) failed' : 'all checks passed');
process.exit(failures ? 1 : 0);
