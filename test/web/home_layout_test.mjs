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

process.exit(failures ? 1 : 0);
