// Tests src/network/html/js/home-layout.js, the Home Screen page's copy of Lyra Tiles' layout rules:
// every case test/home_layout_file/placements.json holds, laid out by the device's own parser, filter
// and engine (HomeLayoutPlacementsTest), comes out of the page's code box for box.
//
//   node test/web/home_layout_test.mjs [deps dir, unused]
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
globalThis.window = globalThis;
vm.runInThisContext(readFileSync(join(repo, 'src/network/html/js/home-layout.js'), 'utf8'));
const { HomeLayout } = globalThis;

let failures = 0;
const check = (ok, what) => {
  if (!ok) {
    console.log('FAIL ' + what);
    failures++;
  }
  return ok;
};
const box = (b) => [b.x, b.y, b.w, b.h];

const { cases } = JSON.parse(readFileSync(join(repo, 'test/home_layout_file/placements.json'), 'utf8'));
check(cases.length > 0, 'placements.json holds cases');
for (const c of cases) {
  const { placement } = HomeLayout.layOut(c.layout, c.device);
  const got = {
    items: placement.items.map((i) => ({ widget: i.cell.widget, option: i.cell.option, box: box(i.box), full: i.full, row: i.row })),
    bottom: placement.bottom.map(box),
    dropped: placement.dropped,
    sideways: placement.sideways,
  };
  const same = JSON.stringify(got) === JSON.stringify(c.placement);
  if (!check(same, `${c.name}: the page lays it out as the device does`)) {
    console.log('  device: ' + JSON.stringify(c.placement));
    console.log('  page:   ' + JSON.stringify(got));
  }
}
console.log(`ok   ${cases.length} cases laid out as the device lays them out`);

// The presets the page offers are the device's (its Reader, Dashboard and Gallery cases).
for (const name of ['reader', 'dashboard', 'gallery']) {
  const c = cases.find((x) => x.name === `x4/${name}`);
  check(c && JSON.stringify(JSON.parse(c.layout)) === JSON.stringify(HomeLayout.PRESETS[name]),
        `the ${name} preset is the device's`);
}

// What the parser refuses, as the device does.
check(!HomeLayout.parse('').ok, 'an empty file is no layout');
check(!HomeLayout.parse('{not json').ok, 'broken JSON is no layout');
check(!HomeLayout.parse('{"version":2,"rows":[[{"widget":"book"}]]}').ok, 'another version is no layout');
check(!HomeLayout.parse('{"version":1,"rows":[]}').ok, 'no rows is no layout');
check(!HomeLayout.parse(' '.repeat(HomeLayout.MAX_BYTES + 1)).ok, 'more than 4 KB is no layout');
const fromObject = HomeLayout.parse(HomeLayout.PRESETS.dashboard);
check(fromObject.ok && fromObject.pictures[0] === '/home', 'a picture without a path is /home');

// Pictures from the web: the file, the size in the address, and the plugin that fetches it. The device.json
// files must be test/plugin_events/home_picture_plugins.json, which the device's event code is tested on
// (PluginEventCoreTest, HomePicturePluginsSubscribe).
check(HomeLayout.webPictureDest(1, 'https://x.org/a/b.PNG?q=1') === '/home/web/web-1.png', 'a .png address keeps its type');
check(HomeLayout.webPictureDest(2, 'https://x.org/today.bmp') === '/home/web/web-2.bmp', 'a .bmp address keeps its type');
check(HomeLayout.webPictureDest(1, 'https://x.org/today') === '/home/web/web-1.jpg', 'an address naming no type is a JPEG');
check(HomeLayout.fillSize('https://x.org/{width}x{height}/{width}', 218, 170) === 'https://x.org/218x170/218',
      '{width} and {height} become the size, wherever they are');
const sleep = HomeLayout.webPicturePlugin(1, { url: 'https://img.example/render?w={width}&h={height}', event: 'sleep.enter' },
                                           { w: 218, h: 170 });
const close = HomeLayout.webPicturePlugin(2, { url: 'https://img.example/today.png', event: 'reader.exit' }, { w: 448, h: 284 });
const fixture = JSON.parse(readFileSync(join(repo, 'test/plugin_events/home_picture_plugins.json'), 'utf8'));
check(JSON.stringify(JSON.parse(sleep.files['device.json'])) === JSON.stringify(fixture.sleep),
      'a picture fetched at sleep: the device.json the device is tested on');
check(JSON.stringify(JSON.parse(close.files['device.json'])) === JSON.stringify(fixture.close),
      'a picture fetched after a book: the device.json the device is tested on');
const manifest = JSON.parse(sleep.files['manifest.json']);
check(sleep.name === 'home-picture-1' && manifest.catalog === false, 'the plugin is home-picture-n, kept out of the catalog');
check(JSON.stringify(manifest.allowedHosts) === '["img.example",".img.example"]', 'it may reach the host and its subdomains');
check(manifest.homeScreen.url === 'https://img.example/render?w={width}&h={height}' && manifest.homeScreen.event === 'sleep.enter',
      'it keeps the address as written, for the page to show again');
const unshown = HomeLayout.webPicturePlugin(1, { url: 'https://x.org/{width}.jpg', event: 'nonsense' }, null);
check(unshown.url === 'https://x.org/{width}.jpg', 'a picture not shown keeps {width} unfilled');
check('sleep.enter' in JSON.parse(unshown.files['device.json']).events, 'an unknown event is fetched at sleep');

// The ready-made sources (WEB_PICTURE_PRESETS): what each fetches for a 218 x 170 widget.
const tile = { w: 218, h: 170 };
check(HomeLayout.webSourceUrl({ preset: 'photo' }, tile) === 'https://picsum.photos/218/170?grayscale', 'a photo at the widget\'s size');
check(HomeLayout.webSourceUrl({ preset: 'calendar', country: 'de', weekStart: 'sunday' }, tile) ===
      'https://crosspoint-calendar.marczyk.ovh/current.bmp?w=218&h=170&country=DE&week_start=sunday',
      'the calendar takes the country in capitals and the week start');
check(HomeLayout.webSourceUrl({ preset: 'calendar', country: 'Germany' }, null).includes('country=none&week_start=monday'),
      'a country that is not two letters is none, and weeks start on Monday unless Sunday');
check(HomeLayout.webSourceUrl({ preset: 'note', text: ' Read more \n\n books ' }, tile) ===
      'https://placehold.co/218x170/FFFFFF/000000/png?text=Read%20more%5Cnbooks&font=roboto',
      'a note: its lines trimmed, empty ones left out, joined by \\n');
const qr = new URL(HomeLayout.webSourceUrl({ preset: 'qr', data: 'https://example.org/a b' }, tile));
check(qr.searchParams.get('size') === '218x218' && qr.searchParams.get('margin') === '32' &&
      qr.searchParams.get('data') === 'https://example.org/a b',
      'a QR code is square, with a margin that keeps it whole when Home cuts it to the box');
check(HomeLayout.webSourceUrl({ preset: 'note', text: 'x' }, null) === '' && HomeLayout.webSourceUrl({ preset: 'qr', data: 'x' }, null) === '',
      'a once preset has no address without the widget\'s size');
check(['photo', 'calendar', 'sun', 'earth', 'note', 'qr'].map((preset) => HomeLayout.webPictureDest(1, { preset })).join() ===
      '/home/web/web-1.jpg,/home/web/web-1.bmp,/home/web/web-1.jpg,/home/web/web-1.jpg,/home/web/web-1.png,/home/web/web-1.png',
      'each preset keeps the type its service answers with');
check(HomeLayout.WEB_PICTURE_PRESETS.filter((p) => p.once).map((p) => p.id).join() === 'note,qr',
      'the note and the QR code are made once; the rest change and are fetched by the reader');
check(HomeLayout.webSourceProblem({ preset: 'note', text: '  ' }) !== '' && HomeLayout.webSourceProblem({ preset: 'qr' }) !== '' &&
      HomeLayout.webSourceProblem({ url: 'ftp://x' }) !== '' && HomeLayout.webSourceProblem({ preset: 'sun' }) === '',
      'a note without text, a QR code without content and a non-http address cannot be saved');
const calendar = HomeLayout.webPicturePlugin(2, { preset: 'calendar', country: 'PL', event: 'sleep.enter' }, tile);
const calendarManifest = JSON.parse(calendar.files['manifest.json']);
check(calendar.dest === '/home/web/web-2.bmp' && calendar.url.startsWith('https://crosspoint-calendar.marczyk.ovh/current.bmp?w=218&h=170&country=PL'),
      'a preset\'s plugin fetches its address to its type');
check(calendarManifest.homeScreen.preset === 'calendar' && calendarManifest.homeScreen.url.includes('{width}') &&
      JSON.stringify(calendarManifest.allowedHosts) === '["crosspoint-calendar.marczyk.ovh",".crosspoint-calendar.marczyk.ovh"]',
      'and keeps the preset, the address with {width} and {height}, and its host');

process.exit(failures ? 1 : 0);
