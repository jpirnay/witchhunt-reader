// Lyra Tiles' Home as the reader lays it out, for the Home Screen page's preview: the layout file's
// rules (src/components/themes/lyra/HomeLayoutFile.cpp), the shortcuts Home leaves out
// (HomeTilesLayout::withAvailable) and the placement (HomeTilesLayout::place), in the same integer
// arithmetic. The device cannot draw Home while it serves this page, so the browser does it.
//
// Kept in step by test/web/home_layout_test.mjs, which lays out every case the device's own code laid
// out in test/home_layout_file/placements.json and wants the same boxes.
(function () {
  'use strict';

  // C++ integer division: towards zero.
  const div = (a, b) => Math.trunc(a / b);
  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

  // HomeTilesLayout::kRegular and kCompact.
  const DIMENSIONS = {
    regular: {
      gap: 12, sidePadding: 16, bottomPadding: 8, bottomRowHeight: 60, minTileHeight: 104, maxTileHeight: 136,
      maxSidewaysTileHeight: 180, stripHeight: 130, stripLabelHeight: 28, stripPadding: 6, minCardHeight: 360,
      cardPadding: 14, cardFootHeight: 36, coverToText: 18, coverToFoot: 10, bookMinHeight: 240,
      bookCompactHeight: 210, clockHeight: 170, clockFullHeight: 130, calendarHeight: 200, summaryHeight: 170,
      summaryFullHeight: 160, weatherHeight: 170, weatherFullHeight: 130, pictureHeight: 170, pictureMinHeight: 200,
    },
    compact: {
      gap: 6, sidePadding: 8, bottomPadding: 4, bottomRowHeight: 36, minTileHeight: 56, maxTileHeight: 72,
      maxSidewaysTileHeight: 90, stripHeight: 130, stripLabelHeight: 28, stripPadding: 4, minCardHeight: 360,
      cardPadding: 8, cardFootHeight: 24, coverToText: 10, coverToFoot: 6, bookMinHeight: 120,
      bookCompactHeight: 120, clockHeight: 96, clockFullHeight: 80, calendarHeight: 128, summaryHeight: 96,
      summaryFullHeight: 104, weatherHeight: 96, weatherFullHeight: 80, pictureHeight: 96, pictureMinHeight: 110,
    },
  };

  const MAX_ROWS = 6;
  const MAX_PICTURES = 2;
  const MAX_BOTTOM = 3;
  const MAX_BYTES = 4096;
  const STRIP_BOOKS = 3;
  const PICTURE_FOLDER = '/home';

  // A shortcut's "to", by destination number (HomeTilesLayout::destination).
  const DESTINATIONS = ['library', 'stats', 'bookmarks', 'transfer', 'weather', 'recent'];
  // The destinations with a menu entry: one the layout does not show is behind More. Recent has none.
  const MENU_DESTINATIONS = ['library', 'stats', 'bookmarks', 'transfer', 'weather'];
  // The widgets named by "widget" alone.
  const PLAIN = ['also', 'clock', 'calendar', 'week', 'weather'];
  // The widgets that may share a row; book and also take the row across.
  const PAIRS = new Set(['shortcut', 'clock', 'calendar', 'week', 'weather', 'picture']);
  // The widgets a layout holds once: the first is kept, a later one left out.
  const SINGLE = ['book', 'also'];
  // The widgets that open something when chosen.
  const OPENS = new Set(['shortcut', 'week', 'calendar', 'weather']);

  // HomeTilesLayout::presets, as layout files.
  const PRESETS = {
    reader: { version: 1, rows: [
      [{ widget: 'book' }],
      [{ widget: 'also' }],
      [{ widget: 'shortcut', to: 'library' }, { widget: 'shortcut', to: 'stats' }],
      [{ widget: 'shortcut', to: 'transfer' }, { widget: 'shortcut', to: 'bookmarks' }],
    ] },
    dashboard: { version: 1, rows: [
      [{ widget: 'book' }],
      [{ widget: 'clock' }, { widget: 'picture' }],
      [{ widget: 'week' }, { widget: 'shortcut', to: 'library' }],
    ] },
    gallery: { version: 1, rows: [
      [{ widget: 'picture' }],
      [{ widget: 'book', size: 'compact' }],
      [{ widget: 'shortcut', to: 'library' }, { widget: 'shortcut', to: 'transfer' }],
    ] },
  };

  // ---- The layout file, as HomeLayoutFile::parse reads it -------------------------------------------

  // The cell an object describes ({widget, option}), or null for one the reader does not know. A
  // picture takes the slot of its path among `pictures`, adding it while there is room.
  function cellFrom(object, pictures) {
    if (!object || typeof object !== 'object' || Array.isArray(object)) return null;
    const widget = typeof object.widget === 'string' ? object.widget : '';
    if (PLAIN.includes(widget)) return { widget, option: 0 };
    if (widget === 'book') return { widget, option: object.size === 'compact' ? 1 : 0 };
    if (widget === 'shortcut') {
      const destination = DESTINATIONS.indexOf(typeof object.to === 'string' ? object.to : '');
      return destination < 0 ? null : { widget, option: destination };
    }
    if (widget === 'picture') {
      const path = typeof object.path === 'string' && object.path !== '' ? object.path : PICTURE_FOLDER;
      for (let i = 0; i < MAX_PICTURES; i++) {
        if (!pictures[i]) pictures[i] = path;
        if (pictures[i] === path) return { widget, option: i };
      }
      return null;  // more pictures than a layout keeps
    }
    return null;
  }

  // A layout file (its text, or the object) as the reader reads it: {ok, rows, pictures}. Each row is
  // {cells: [{widget, option}], source}: `source` is the file's row it came from.
  function parse(input) {
    const result = { ok: false, rows: [], pictures: [] };
    let doc = input;
    if (typeof input === 'string') {
      if (input.length === 0 || new TextEncoder().encode(input).length > MAX_BYTES) return result;
      try {
        doc = JSON.parse(input);
      } catch {
        return result;
      }
    }
    if (!doc || typeof doc !== 'object' || doc.version !== 1) return result;
    const rows = Array.isArray(doc.rows) ? doc.rows : [];
    // One book card and one Also reading row: a second is left out as an unknown widget is.
    const kept = new Set();
    for (let source = 0; source < rows.length; source++) {
      if (result.rows.length >= MAX_ROWS) break;
      const cells = [];
      for (const object of Array.isArray(rows[source]) ? rows[source] : []) {
        if (cells.length < 2) {
          const cell = cellFrom(object, result.pictures);
          if (cell && !(SINGLE.includes(cell.widget) && kept.has(cell.widget))) cells.push(cell);
        }
      }
      if (cells.length === 2 && !(PAIRS.has(cells[0].widget) && PAIRS.has(cells[1].widget))) cells.length = 1;
      if (cells.length > 0) result.rows.push({ cells, source });
      for (const cell of cells) kept.add(cell.widget);
    }
    result.ok = result.rows.length > 0;
    return result;
  }

  // HomeTilesLayout::withAvailable: the shortcuts to destinations not in `available` (names) left out.
  function withAvailable(rows, available) {
    const has = new Set(available);
    const shown = [];
    for (const row of rows) {
      const cells = row.cells.filter((c) => c.widget !== 'shortcut' || has.has(DESTINATIONS[c.option]));
      if (cells.length > 0) shown.push({ cells, source: row.source });
    }
    return shown;
  }

  // ---- Placement: HomeTilesLayout::place ------------------------------------------------------------

  const sideways = (content) => content.w > content.h;
  const tileHeight = (content, d) => clamp(div(content.h * 16 + 50, 100), d.minTileHeight, d.maxTileHeight);

  function need(row, tileH, d) {
    const n = { height: 0, flex: false, optional: false, book: false, tile: false };
    const full = row.cells.length === 1;
    let fixed = 0;
    let floor = 0;
    for (const cell of row.cells) {
      switch (cell.widget) {
        case 'book':
          n.book = true;
          if (cell.option === 1) {
            fixed = Math.max(fixed, d.bookCompactHeight);
          } else {
            n.flex = true;
            floor = Math.max(floor, d.bookMinHeight);
          }
          break;
        case 'also':
          n.optional = true;
          fixed = Math.max(fixed, d.stripHeight);
          break;
        case 'shortcut':
          n.tile = true;
          fixed = Math.max(fixed, tileH);
          break;
        case 'clock':
          fixed = Math.max(fixed, full ? d.clockFullHeight : d.clockHeight);
          break;
        case 'calendar':
          fixed = Math.max(fixed, d.calendarHeight);
          break;
        case 'week':
          fixed = Math.max(fixed, full ? d.summaryFullHeight : d.summaryHeight);
          break;
        case 'weather':
          fixed = Math.max(fixed, full ? d.weatherFullHeight : d.weatherHeight);
          break;
        case 'picture':
          if (full) {
            n.flex = true;
            floor = Math.max(floor, d.pictureMinHeight);
          } else {
            fixed = Math.max(fixed, d.pictureHeight);
          }
          break;
      }
    }
    n.height = n.flex ? Math.max(floor, fixed) : fixed;
    return n;
  }

  function stack(needs, on, avail, d) {
    let fixed = 0;
    let flexCount = 0;
    let rows = 0;
    const heights = needs.map(() => 0);
    needs.forEach((n, r) => {
      if (!on[r]) return;
      ++rows;
      if (n.flex) ++flexCount;
      else fixed += n.height;
    });
    const left = avail - fixed - (rows > 1 ? (rows - 1) * d.gap : 0);
    let given = 0;
    let seen = 0;
    needs.forEach((n, r) => {
      if (!on[r]) return;
      if (n.flex) {
        ++seen;
        heights[r] = seen === flexCount ? left - given : div(left, flexCount);
        given += heights[r];
      } else {
        heights[r] = n.height;
      }
    });
    return heights;
  }

  function needed(needs, on, d) {
    let total = 0;
    let rows = 0;
    needs.forEach((n, r) => {
      if (!on[r]) return;
      total += n.height;
      ++rows;
    });
    return total + (rows > 1 ? (rows - 1) * d.gap : 0);
  }

  // Leave out rows until the rest fit: Also reading first (quietly), then from the bottom up, never a
  // row that holds the book card. Returns the bits of the rows left out loudly.
  function fit(needs, on, avail, d) {
    let dropped = 0;
    while (needed(needs, on, d) > avail) {
      let victim = -1;
      for (let r = needs.length - 1; r >= 0; --r) {
        if (on[r] && needs[r].optional) {
          victim = r;
          break;
        }
      }
      const quiet = victim >= 0;
      if (victim < 0) {
        for (let r = needs.length - 1; r >= 0; --r) {
          if (on[r] && !needs[r].book) {
            victim = r;
            break;
          }
        }
      }
      if (victim < 0) break;
      on[victim] = false;
      if (!quiet) dropped |= 1 << victim;
    }
    return dropped;
  }

  function keepStripOnlyBesideATallCard(rows, needs, on, avail, d) {
    let stripRow = -1;
    let bookRow = -1;
    rows.forEach((row, r) => {
      if (!on[r]) return;
      if (needs[r].optional) stripRow = r;
      if (row.cells.some((c) => c.widget === 'book' && c.option !== 1)) bookRow = r;
    });
    if (stripRow < 0) return;
    if (bookRow < 0) {
      on[stripRow] = false;
      return;
    }
    if (stack(needs, on, avail, d)[bookRow] < d.minCardHeight) on[stripRow] = false;
  }

  function placeBottom(area, count, d) {
    const n = clamp(count, 1, MAX_BOTTOM);
    const y = area.y + area.h - d.bottomRowHeight;
    const h = d.bottomRowHeight;
    if (n === 1) return [{ x: area.x, y, w: area.w, h }];
    if (n === 2) {
      const halfW = div(area.w - d.gap, 2);
      return [{ x: area.x, y, w: halfW, h }, { x: area.x + area.w - halfW, y, w: halfW, h }];
    }
    const w = div(area.w - 2 * d.gap, 3);
    return [{ x: area.x, y, w, h }, { x: area.x + div(area.w - w, 2), y, w, h }, { x: area.x + area.w - w, y, w, h }];
  }

  function placeRows(items, rows, on, heights, x, w, y, d) {
    rows.forEach((row, r) => {
      if (!on[r]) return;
      const h = heights[r];
      if (row.cells.length === 1) {
        items.push({ cell: row.cells[0], box: { x, y, w, h }, full: true, row: r });
      } else {
        const halfW = div(w - d.gap, 2);
        items.push({ cell: row.cells[0], box: { x, y, w: halfW, h }, full: false, row: r });
        items.push({ cell: row.cells[1], box: { x: x + w - halfW, y, w: halfW, h }, full: false, row: r });
      }
      y += h + d.gap;
    });
  }

  const holdsBook = (row) => row.cells.some((c) => c.widget === 'book' || c.widget === 'also');

  // `content` {x, y, w, h}: the screen above the button hints; `top`: the first line under the status
  // line; `rows` as parse() and withAvailable() give them; `bottomCount`: More and Settings, or
  // Settings alone. {items, bottom, dropped, sideways}: items in reading order; dropped, bit r: rows[r]
  // did not fit.
  function place(content, top, rows, bottomCount, d) {
    rows = rows.slice(0, MAX_ROWS);
    const p = { items: [], bottom: [], dropped: 0, sideways: sideways(content) };
    const bottom = content.y + content.h - d.bottomPadding;
    if (!p.sideways || !rows.some(holdsBook)) {
      const x = content.x + d.sidePadding;
      const w = content.w - 2 * d.sidePadding;
      p.bottom = placeBottom({ x, y: top, w, h: bottom - top }, bottomCount, d);
      const avail = bottom - d.bottomRowHeight - d.gap - top;
      const tileH = tileHeight(content, d);
      const needs = rows.map((row) => need(row, tileH, d));
      const on = rows.map(() => true);
      keepStripOnlyBesideATallCard(rows, needs, on, avail, d);
      p.dropped |= fit(needs, on, avail, d);
      placeRows(p.items, rows, on, stack(needs, on, avail, d), x, w, top, d);
      return p;
    }
    // Sideways: the rows with the book card and Also reading on the left, the rest and the bottom row
    // on the right.
    const leftX = content.x + d.sidePadding;
    const leftW = div(content.w * 56 + 50, 100) - d.sidePadding;
    const rightX = leftX + leftW + d.gap;
    const rightW = content.x + content.w - d.sidePadding - rightX;
    p.bottom = placeBottom({ x: rightX, y: top, w: rightW, h: bottom - top }, bottomCount, d);
    const needs = rows.map((row) => need(row, d.minTileHeight, d));
    const cap = rows.map(() => 0);
    needs.forEach((n, r) => {
      if (n.tile) {
        cap[r] = Math.max(n.height, d.maxSidewaysTileHeight);
        n.flex = true;
      }
    });
    const left = rows.map(holdsBook);
    const right = left.map((l) => !l);
    const leftAvail = bottom - top;
    keepStripOnlyBesideATallCard(rows, needs, left, leftAvail, d);
    p.dropped |= fit(needs, left, leftAvail, d);
    placeRows(p.items, rows, left, stack(needs, left, leftAvail, d), leftX, leftW, top, d);
    const rightAvail = bottom - d.bottomRowHeight - d.gap - top;
    p.dropped |= fit(needs, right, rightAvail, d);
    const rightHeights = stack(needs, right, rightAvail, d).map((h, r) => (cap[r] > 0 && h > cap[r] ? cap[r] : h));
    placeRows(p.items, rows, right, rightHeights, rightX, rightW, top, d);
    return p;
  }

  // ---- Inside the card and the Also reading row: HomeTilesLayout::card and strip --------------------

  function card(box, d) {
    const innerW = box.w - 2 * d.cardPadding;
    const coverAreaH = Math.max(0, box.h - 2 * d.cardPadding - d.cardFootHeight - d.coverToFoot);
    let coverH = coverAreaH;
    let coverW = div(coverH * 3, 5);
    const maxCoverW = div(innerW * 9, 20);
    if (coverW > maxCoverW) {
      coverW = maxCoverW;
      coverH = div(coverW * 5, 3);
    }
    const cover = { x: box.x + d.cardPadding, y: box.y + d.cardPadding, w: coverW, h: coverH };
    const textX = cover.x + coverW + d.coverToText;
    const textW = box.x + box.w - d.cardPadding - textX;
    return {
      cover,
      text: { x: textX, y: box.y + d.cardPadding, w: Math.max(0, textW), h: coverAreaH },
      foot: { x: box.x + d.cardPadding, y: box.y + box.h - d.cardPadding - d.cardFootHeight, w: Math.max(0, innerW),
        h: d.cardFootHeight },
    };
  }

  function strip(box, d) {
    const inner = { x: box.x + d.stripPadding, y: box.y + d.stripPadding, w: box.w - 2 * d.stripPadding,
      h: box.h - 2 * d.stripPadding };
    const slotW = div(inner.w - (STRIP_BOOKS - 1) * d.gap, STRIP_BOOKS);
    const slotH = inner.h - d.stripLabelHeight;
    const slots = [];
    for (let i = 0; i < STRIP_BOOKS; i++) {
      const slot = { x: inner.x + i * (slotW + d.gap), y: inner.y + d.stripLabelHeight, w: slotW, h: slotH };
      slots.push({ slot, cover: { x: slot.x, y: slot.y, w: div(slotH * 3, 5), h: slotH } });
    }
    return slots;
  }

  // ---- Home as the reader shows it ------------------------------------------------------------------

  // The bottom row: Settings, and More before it when an available destination with a menu entry is not
  // among the layout's shortcuts (LyraTilesTheme::homeShownDestinations, collectHomeMenuEntries).
  function bottomCount(placement, available) {
    const shown = new Set(placement.items.filter((i) => i.cell.widget === 'shortcut').map((i) => DESTINATIONS[i.cell.option]));
    return MENU_DESTINATIONS.some((name) => available.includes(name) && !shown.has(name)) ? 2 : 1;
  }

  // Home for `layout` (a file's text or object) on `device`: {content: [x,y,w,h], top, compact, available}
  // as GET /api/home-layout answers. {ok, rows, pictures, placement}; placement.items[].row indexes rows,
  // and rows[r].source the file's row.
  function layOut(layout, device) {
    const parsed = parse(layout);
    const rows = withAvailable(parsed.rows, device.available);
    const [x, y, w, h] = device.content;
    const content = { x, y, w, h };
    const d = device.compact ? DIMENSIONS.compact : DIMENSIONS.regular;
    // As the theme does: which destinations the layout shows is asked of a placement with More.
    const withMore = place(content, device.top, rows, 2, d);
    const placement = place(content, device.top, rows, bottomCount(withMore, device.available), d);
    return { ok: parsed.ok, rows, pictures: parsed.pictures, placement, dimensions: d };
  }

  // ---- Pictures from the web ------------------------------------------------------------------------
  //
  // A picture a server renders: the reader downloads it from a URL when an event happens, through a small
  // plugin the page installs (docs/sd-plugins.md, "Events"), and Home shows the file. Kept in a folder of
  // its own, so a widget that takes a picture from /home does not pick it.

  const WEB_PICTURE_FOLDER = '/home/web';
  const WEB_PICTURE_PLUGIN = 'home-picture-';
  // The events a picture can be fetched on: going to sleep (the reader joins Wi-Fi for it), or after a book
  // is closed (sent at the next sleep or Wi-Fi connection).
  const WEB_PICTURE_EVENTS = ['sleep.enter', 'reader.exit'];

  // Ready-made sources, checked 2026-11-24: each answers a JPEG, PNG or BMP without a key. `url` may hold
  // {width} and {height} (the widget's size, filled when the page saves) and the preset's own fields; `ext`
  // is the type the service answers with. A `once` preset never changes: the page fetches it in the browser
  // as it saves (the service allows that), and the reader never asks the service again.
  const WEB_PICTURE_PRESETS = [
    { id: 'photo', ext: '.jpg', url: 'https://picsum.photos/{width}/{height}?grayscale' },
    { id: 'calendar', ext: '.bmp',
      url: 'https://crosspoint-calendar.marczyk.ovh/current.bmp?w={width}&h={height}&country={country}&week_start={week_start}' },
    { id: 'sun', ext: '.jpg', url: 'https://sdo.gsfc.nasa.gov/assets/img/latest/latest_512_HMIIC.jpg' },
    { id: 'earth', ext: '.jpg', url: 'https://cdn.star.nesdis.noaa.gov/GOES19/ABI/FD/GEOCOLOR/339x339.jpg' },
    { id: 'note', ext: '.png', once: true },
    { id: 'qr', ext: '.png', once: true },
  ];
  const presetOf = (source) => WEB_PICTURE_PRESETS.find((p) => p.id === source.preset) || null;

  // The URL with {width} and {height} set to the widget's size, so a server can render the picture to fit.
  const fillSize = (url, width, height) => url.replaceAll('{width}', String(width)).replaceAll('{height}', String(height));

  // Where a source fetches from: with `size` ({w, h}) the address itself; without, the address with {width}
  // and {height} still in it ('' for a once preset, which needs the size).
  function webSourceUrl(source, size) {
    const preset = presetOf(source);
    if (preset && preset.id === 'note') {
      if (!size) return '';
      // One line a line of the text; placehold.co breaks lines at "\n".
      const text = String(source.text || '').split(/\r?\n/).map((l) => l.trim()).filter(Boolean).join('\\n');
      return `https://placehold.co/${size.w}x${size.h}/FFFFFF/000000/png?text=${encodeURIComponent(text)}&font=roboto`;
    }
    if (preset && preset.id === 'qr') {
      if (!size) return '';
      // A square the box's longer side, with a margin inside it wide enough that Home's cut to the box (it
      // fills the box and cuts what is over) leaves the whole code and a quiet zone.
      const side = Math.max(size.w, size.h);
      const margin = Math.ceil((side - Math.min(size.w, size.h)) / 2) + 8;
      return `https://api.qrserver.com/v1/create-qr-code/?size=${side}x${side}&margin=${margin}&data=${encodeURIComponent(source.data || '')}`;
    }
    let template = preset ? preset.url : String(source.url || '');
    if (preset && preset.id === 'calendar') {
      const country = /^[A-Za-z]{2}$/.test(source.country || '') ? source.country.toUpperCase() : 'none';
      template = template.replace('{country}', country).replace('{week_start}', source.weekStart === 'sunday' ? 'sunday' : 'monday');
    }
    return size ? fillSize(template, size.w, size.h) : template;
  }

  // The type a source answers with: a preset's, else as the address's path names it, else JPEG.
  function webSourceExt(source) {
    const preset = presetOf(source);
    if (preset) return preset.ext;
    let path = '';
    try {
      path = new URL(source.url).pathname.toLowerCase();
    } catch {
      path = '';
    }
    return /\.png$/.test(path) ? '.png' : /\.bmp$/.test(path) ? '.bmp' : '.jpg';
  }

  // What keeps a source from being saved, '' when nothing does.
  function webSourceProblem(source) {
    const preset = presetOf(source);
    if (preset && preset.id === 'note') return String(source.text || '').trim() ? '' : 'A note needs its text.';
    if (preset && preset.id === 'qr') return String(source.data || '').trim() ? '' : 'A QR code needs the link or text it holds.';
    if (preset) return '';
    return /^https?:\/\/[^/\s]+/i.test(source.url || '') ? '' : 'A picture from the web needs an address starting with https:// (or http://).';
  }

  // The file the n-th picture from the web goes to. `source` is {preset, ...} or {url}; a string is a URL.
  function webPictureDest(n, source) {
    return `${WEB_PICTURE_FOLDER}/web-${n}${webSourceExt(typeof source === 'string' ? { url: source } : source)}`;
  }

  // Whether a source's address takes the widget's size ({width}, {height}): it cannot be fetched before its row is
  // shown on the reader. The reader fills no placeholder, so it would ask for "{width}" on every sleep.
  const webSourceNeedsSize = (source) => /\{(width|height)\}/.test(webSourceUrl(source, null));

  // The plugin that fetches the n-th picture from the web: {name, dest, url, host, files: {'device.json',
  // 'manifest.json'}}. `source` is {url, event} or a preset's {preset, ..., event}; `size` the widget's {w, h}, or
  // null when it is not shown: then null for an address that needs the size. Not for a once preset, which no plugin
  // fetches.
  function webPicturePlugin(n, source, size) {
    if (!size && webSourceNeedsSize(source)) return null;
    const name = `${WEB_PICTURE_PLUGIN}${n}`;
    const dest = webPictureDest(n, source);
    const template = webSourceUrl(source, null);
    const url = size ? webSourceUrl(source, size) : template;
    let host = '';
    try {
      host = new URL(url).hostname;
    } catch {
      host = '';
    }
    const event = WEB_PICTURE_EVENTS.includes(source.event) ? source.event : WEB_PICTURE_EVENTS[0];
    const handler = { download: { url, dest } };
    if (event !== 'sleep.enter') handler.connect = true;  // sleep.enter joins Wi-Fi on its own
    const device = { events: { [event]: handler } };
    const homeScreen = { url: template, event, dest };
    if (source.preset) homeScreen.preset = source.preset;
    const manifest = {
      title: `Home screen picture ${n}`,
      description: `Fetches ${template} to ${dest} for the Home screen. Made by the Home Screen page, which replaces or removes it.`,
      version: '1',
      catalog: false,
      // The host and its subdomains: picture services often redirect to a CDN under their own name.
      allowedHosts: host ? [host, `.${host}`] : [],
      homeScreen,
    };
    return { name, dest, url, host, files: { 'device.json': JSON.stringify(device), 'manifest.json': JSON.stringify(manifest) } };
  }

  window.HomeLayout = {
    DIMENSIONS, PRESETS, DESTINATIONS, MENU_DESTINATIONS, PAIRS, SINGLE, OPENS, MAX_ROWS, MAX_PICTURES, MAX_BYTES,
    PICTURE_FOLDER, WEB_PICTURE_FOLDER, WEB_PICTURE_PLUGIN, WEB_PICTURE_EVENTS, WEB_PICTURE_PRESETS, parse, withAvailable,
    place, card, strip, bottomCount, layOut, webPictureDest, fillSize, webSourceUrl, webSourceExt, webSourceProblem,
    webSourceNeedsSize, webPicturePlugin,
  };
})();
