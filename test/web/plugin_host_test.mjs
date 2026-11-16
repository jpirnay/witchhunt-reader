#!/usr/bin/env node
// Tests the browser-side plugin host (src/network/html/shared/PluginHost.js.inc) against the API plugins written for
// upstream crosspoint-reader expect: api.dir, relay(method, url, headers, body) answering {status, headers, body},
// writeFile with base64, fetchToSd with headers and {method, body}, a registerAction that loads, and the host
// approval a refused relay asks for. Our own one-argument relay keeps answering a Response.
//
// The host's real scripts run in jsdom; a plugin is served through jsdom's resource loader, and the device is a fake
// fetch that records each request and answers it from a script.
//
//   node test/web/plugin_host_test.mjs <dir with node_modules/jsdom>
// (the folder CI installs jsdom into, see .github/workflows/ci.yml). Exit status 0: every check passed.

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const deps = process.argv[2];
if (!deps) {
  console.error('usage: node test/web/plugin_host_test.mjs <dir with node_modules/jsdom>');
  process.exit(2);
}
const { JSDOM, VirtualConsole, ResourceLoader } = createRequire(path.join(path.resolve(deps), 'noop.js'))('jsdom');

const SHARED = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'src', 'network', 'html', 'shared');

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
  return ok;
}

// A fetch Response, as much of it as the host reads.
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

// The device: every request is recorded; `answers` picks the reply by method and path.
const requests = [];
let answer = () => new FakeResponse(404, { error: 'no such route' });

// What an upstream plugin does at top level: register, and hand its api to the test.
const PLUGIN_JS = 'CrossPoint.registerPlugin(function (el, api) { window.__api = api; });';

class PluginLoader extends ResourceLoader {
  fetch(url) {
    if (url.includes('/plugin?name=up&file=plugin.js')) return Promise.resolve(Buffer.from(PLUGIN_JS));
    return Promise.reject(new Error('no resource ' + url));
  }
}

const page = '<!doctype html><html><body><div id="plugin-container"></div><script>' +
  fs.readFileSync(path.join(SHARED, 'FileNames.js.inc'), 'utf8') + '\n' +
  fs.readFileSync(path.join(SHARED, 'PluginHost.js.inc'), 'utf8') + '</script></body></html>';

let confirmAnswer = true;
const confirmed = [];
const dom = new JSDOM(page, {
  url: 'http://reader.test/settings',
  runScripts: 'dangerously',
  resources: new PluginLoader(),
  virtualConsole: new VirtualConsole(),
  beforeParse(w) {
    w.TextDecoder = TextDecoder;
    w.confirm = (message) => {
      confirmed.push(message);
      return confirmAnswer;
    };
    w.fetch = async (url, options) => {
      const req = { url: String(url), method: (options && options.method) || 'GET', options: options || {} };
      if (typeof req.options.body === 'string') req.json = JSON.parse(req.options.body);
      requests.push(req);
      if (req.url === '/api/plugins') {
        return new FakeResponse(200, [{ name: 'up', title: 'Upstream plugin', root: '/.crosspoint/plugins', mount: 'settings' }]);
      }
      return answer(req);
    };
  }
});
const w = dom.window;

await w.loadPlugins('settings');
const api = w.__api;
if (!check(api !== undefined, 'the plugin registered and received its api')) process.exit(1);

// ---- api.dir and registerAction -------------------------------------------------------------------------------------
check(api.dir === '/.crosspoint/plugins/up', 'api.dir is the plugin folder (' + api.dir + ')');
let threw = false;
try { api.registerAction('sync', () => {}); } catch (e) { threw = true; }
check(!threw, 'registerAction() loads without a job queue');

// ---- relay(method, url, headers, body) ------------------------------------------------------------------------------
// The device sends the header list as UTF-8 bytes that reach the page as Latin-1 code units.
const headerJson = JSON.stringify([['Content-Type', 'application/json'], ['Set-Cookie', 'a=1'], ['Set-Cookie', 'b=2'],
  ['X-Title', 'Ünïcödé']]);
const asLatin1 = String.fromCharCode(...new TextEncoder().encode(headerJson));
requests.length = 0;
answer = (req) => req.url === '/api/relay' && req.method === 'POST'
  ? new FakeResponse(200, '{"token":"t"}', { 'X-Relay-Status': '201', 'X-Relay-Headers': asLatin1 })
  : new FakeResponse(404, { error: 'unexpected ' + req.url });
const res = await api.relay('POST', 'https://example.org/login', { Authorization: 'Bearer x' }, 'user=a');
const sent = requests.find((r) => r.url === '/api/relay');
check(sent && sent.options.headers['Content-Type'] === 'application/json', 'relay posts application/json');
check(sent && sent.json.plugin === 'up' && sent.json.method === 'POST' && sent.json.url === 'https://example.org/login' &&
      sent.json.headers.Authorization === 'Bearer x' && sent.json.body === 'user=a',
      'relay sends plugin, method, url, headers and body');
check(res.status === 201, 'relay answers the upstream status');
check(res.body === '{"token":"t"}', 'relay answers the body as text');
check(Array.isArray(res.headers) && res.headers.length === 4 && res.headers[1][1] === 'a=1' && res.headers[2][1] === 'b=2',
      'relay keeps every header, duplicates included');
check(res.headers[3][1] === 'Ünïcödé', 'relay decodes UTF-8 header values');

// ---- relay(url): our own form ---------------------------------------------------------------------------------------
requests.length = 0;
answer = (req) => req.url.startsWith('/api/relay?') ? new FakeResponse(200, 'cover bytes') : new FakeResponse(404, {});
const own = await api.relay('https://covers.example.org/1.jpg');
check(requests[0].method === 'GET' && requests[0].url ===
      '/api/relay?plugin=up&url=' + encodeURIComponent('https://covers.example.org/1.jpg'),
      'relay(url) is a GET with plugin and url');
check(own instanceof FakeResponse && (await own.text()) === 'cover bytes', 'relay(url) answers a Response');

// ---- a refused host: asked, approved, tried again -------------------------------------------------------------------
requests.length = 0;
confirmed.length = 0;
confirmAnswer = true;
let approved = false;
answer = (req) => {
  if (req.url === '/api/plugins/hosts') {
    approved = true;
    return new FakeResponse(200, { ok: true });
  }
  if (req.url === '/api/relay') {
    return approved ? new FakeResponse(200, 'ok', { 'X-Relay-Status': '200', 'X-Relay-Headers': '[]' })
                    : new FakeResponse(403, { error: 'host not allowed for this plugin', host: 'wallabag.home' });
  }
  return new FakeResponse(404, {});
};
const retried = await api.relay('GET', 'https://wallabag.home/api/entries', {}, '');
check(confirmed.length === 1 && confirmed[0].includes('wallabag.home') && confirmed[0].includes('Upstream plugin'),
      'a refused host is put to the user, naming plugin and host');
const approval = requests.find((r) => r.url === '/api/plugins/hosts');
check(approval && approval.json.plugin === 'up' && approval.json.host === 'wallabag.home' && approval.json.allow === true &&
      approval.options.headers['Content-Type'] === 'application/json', 'a yes stores the approval for that plugin');
check(requests.filter((r) => r.url === '/api/relay').length === 2 && retried.status === 200,
      'the request runs again after the approval');

requests.length = 0;
confirmed.length = 0;
confirmAnswer = false;
approved = false;
let refusal = null;
try { await api.relay('GET', 'https://wallabag.home/api/entries', {}, ''); } catch (e) { refusal = e; }
check(refusal && refusal.status === 403 && refusal.result && refusal.result.host === 'wallabag.home',
      'a no leaves the refusal, with the device\'s answer attached');
check(!requests.some((r) => r.url === '/api/plugins/hosts'), 'a no stores nothing');

// ---- writeFile(path, base64) ----------------------------------------------------------------------------------------
requests.length = 0;
answer = (req) => req.url.startsWith('/api/plugin-fs?') ? new FakeResponse(200, { ok: true }) : new FakeResponse(404, {});
await api.writeFile(api.dir + '/token.json', Buffer.from('{"t":1}').toString('base64'));
const fsReq = requests[0];
check(fsReq.url === '/api/plugin-fs?plugin=up&path=' + encodeURIComponent('/.crosspoint/plugins/up/token.json'),
      'writeFile posts to plugin-fs with the path');
const part = fsReq.options.body.get('file');
const written = await new Promise((resolve) => {
  const reader = new w.FileReader();
  reader.onload = () => resolve(reader.result);
  reader.readAsText(part);
});
check(written === '{"t":1}', 'writeFile decodes a base64 string (' + written + ')');

// ---- fetchToSd(url, dest, headers, {method, body}) ------------------------------------------------------------------
requests.length = 0;
answer = (req) => req.url === '/api/fetch' ? new FakeResponse(200, { ok: true, complete: true, bytes: 10 }) : new FakeResponse(404, {});
const fetched = await api.fetchToSd('https://example.org/book.epub', '/Books/b.epub', { Authorization: 'Bearer x' },
                                    { method: 'post', body: 'id=1' });
const fetchReq = requests[0];
check(fetchReq.method === 'POST' && fetchReq.options.headers['Content-Type'] === 'application/json',
      'fetchToSd posts application/json');
check(fetchReq.json.plugin === 'up' && fetchReq.json.url === 'https://example.org/book.epub' &&
      fetchReq.json.dest === '/Books/b.epub' && fetchReq.json.headers.Authorization === 'Bearer x' &&
      fetchReq.json.method === 'POST' && fetchReq.json.body === 'id=1', 'fetchToSd sends url, dest, headers, method and body');
check(fetched.complete === true, 'fetchToSd answers the device\'s result');

dom.window.close();
console.log(failures ? failures + ' check(s) failed' : 'all checks passed');
process.exit(failures ? 1 : 0);
