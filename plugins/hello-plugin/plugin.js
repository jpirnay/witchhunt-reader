CrossPoint.registerPlugin((container, api) => {
  container.innerHTML =
    '<h2>' + api.title + '</h2>' +
    '<p>Loaded from the SD card as <code>' + api.name + '</code>.</p>' +
    '<button id="hp-count">Count files in /</button>' +
    '<pre id="hp-out" style="white-space:pre-wrap"></pre>';
  container.querySelector('#hp-count').onclick = async () => {
    const out = container.querySelector('#hp-out');
    out.textContent = 'Loading...';
    try {
      const r = await fetch('/api/files?path=/');
      const files = await r.json();
      out.textContent = files.length + ' entries in /\n' +
        files.slice(0, 10).map(f => '  ' + (f.name || JSON.stringify(f))).join('\n');
    } catch (e) {
      out.textContent = 'Error: ' + e.message;
    }
  };
});
