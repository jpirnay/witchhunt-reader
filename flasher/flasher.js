// Witch Reader web flasher.
//
// Writes a release's full-install image (bootloader, partition table, otadata and firmware, from
// 0x0), or just the firmware at 0x10000 with otadata reset, or a local .bin, over WebSerial with
// esptool-js (Espressif, Apache-2.0), then optionally installs the web interface over the same
// cable. The page and its images come from GitHub Pages (scripts/stage_flasher_site.py writes
// manifest.json); nothing else is fetched.
//
// Two reset sequences follow esptool (Espressif, GPL-2.0-or-later), which esptool-js 0.7.0 lacks:
// the S3 watchdog reset (esptool/targets/esp32s3.py, ESP32S3ROM.watchdog_reset) and the RTS hard
// reset (esptool/reset.py, HardReset), whose esptool-js version never raises RTS.

import { ESPLoader, Transport } from "esptool-js";
import * as lib from "./lib.js";
import { DeviceLink, installWebBundle, sleep } from "./serial-link.js";

const ESPRESSIF_VID = 0x303a;
const $ = (id) => document.getElementById(id);

const state = {
  manifest: null,
  board: lib.BOARDS[0],
  port: null,
  transport: null,
  loader: null,
  chip: null,
  sdm: false, // Secure Download Mode: writes only, no reads, no stub
  locked: null, // why writing would leave the device unbootable, if it would
  flashSize: null,
  table: undefined, // entries read at 0x8000; null when there is no valid table
  tableKind: null,
  custom: null, // { bytes, info } of the chosen local .bin
  customWeb: null, // { name, data } of the chosen local web bundle
  busy: false,
};

// ---- Log and messages ---------------------------------------------------------------------------

function log(text) {
  const pre = $("log");
  pre.textContent += text + "\n";
  pre.scrollTop = pre.scrollHeight;
}

const terminal = {
  clean() {},
  writeLine: (data) => log(data),
  write: (data) => log(data),
};

function note(parent, kind, ...parts) {
  const div = document.createElement("div");
  div.className = "note " + kind;
  for (const part of parts) div.append(part);
  parent.append(div);
  return div;
}

function status(text) {
  $("status").textContent = text;
}

// A device that cannot be reached, refuses writes or would not start what is written: some X4 Pro
// units are locked by eFuses to SD-card updates.
function lockedNote(parent, lead) {
  const app = boardImages().app;
  const parts = [lead, " It can only be updated from the SD card: copy "];
  if (app) {
    const a = link(app.file, app.file.split("/").pop());
    a.download = app.file.split("/").pop();
    parts.push(a);
  } else {
    parts.push("the release's firmware .bin for this device");
  }
  parts.push(" to the card's top folder and install it on the device, under Settings > System > System Update > " +
    "SD Card Firmware Update on the original firmware, or Settings > System > SD Firmware Update on Witch Reader.");
  return note(parent, "bad", ...parts);
}

function link(href, text) {
  const a = document.createElement("a");
  a.href = href;
  a.textContent = text;
  return a;
}

// ---- Manifest -----------------------------------------------------------------------------------

async function loadManifest() {
  try {
    const res = await fetch("manifest.json", { cache: "no-cache" });
    if (!res.ok) throw new Error(res.status);
    state.manifest = await res.json();
  } catch (e) {
    state.manifest = null;
  }
  const rel = $("release");
  if (!state.manifest) {
    rel.textContent = "No release images on this site: only a custom file can be written.";
    return;
  }
  const m = state.manifest;
  rel.textContent = "Release: ";
  rel.append(m.url ? link(m.url, m.name) : m.name);
  const links = $("app-links");
  links.textContent = "Firmware images of this release: ";
  lib.BOARDS.forEach((b, i) => {
    const app = m.boards[b.id] && m.boards[b.id].app;
    if (!app) return;
    if (i) links.append(", ");
    const a = link(app.file, b.label);
    a.download = app.file.split("/").pop();
    links.append(a);
  });
}

async function fetchImage(entry) {
  const res = await fetch(entry.file, { cache: "no-cache" });
  if (!res.ok) throw new Error(`could not load ${entry.file} (${res.status})`);
  const data = new Uint8Array(await res.arrayBuffer());
  const sha = lib.toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", data)));
  if (data.length !== entry.size || sha !== entry.sha256) throw new Error(`${entry.file} is not the file the release lists`);
  return data;
}

// ---- Board picker -------------------------------------------------------------------------------

function renderBoards() {
  const box = $("boards");
  for (const b of lib.BOARDS) {
    const label = document.createElement("label");
    label.className = "choice";
    const input = document.createElement("input");
    input.type = "radio";
    input.name = "board";
    input.value = b.id;
    input.checked = b === state.board;
    input.addEventListener("change", () => {
      state.board = b;
      refresh();
    });
    label.append(input, " ", b.label);
    const small = document.createElement("small");
    small.textContent = b.chip;
    label.append(small);
    box.append(label);
  }
}

const boardImages = () => (state.manifest && state.manifest.boards[state.board.id]) || { app: null, full: null };

// ---- Connect ------------------------------------------------------------------------------------

// esptool's HardReset (reset.py): RTS raised, then released. esptool-js 0.7.0 only releases it,
// which on a USB Serial/JTAG port (DTR and RTS already low) changes nothing.
class RtsHardReset {
  constructor(transport) {
    this.transport = transport;
  }
  async reset() {
    await this.transport.setRTS(true);
    await sleep(100);
    await this.transport.setRTS(false);
  }
}

async function connect() {
  if (!("serial" in navigator)) return;
  const dev = $("device");
  dev.replaceChildren();
  let port;
  try {
    port = await navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF_VID }] });
  } catch (e) {
    return; // the user closed the picker
  }
  setBusy(true);
  status("Connecting...");
  state.port = port;
  state.transport = new Transport(port, false);
  state.loader = new ESPLoader({
    transport: state.transport,
    baudrate: 921600,
    romBaudrate: 115200,
    terminal,
    resetConstructors: { hardReset: (transport) => new RtsHardReset(transport) },
  });
  try {
    await state.loader.main();
  } catch (e) {
    log("connect: " + (e && e.message ? e.message : e));
    await dropConnection();
    note(dev, "warn", "The device's ROM loader did not answer (" + String(e && e.message ? e.message : e) + "). " +
      "Wake the device, close other programs that use the port, and try again.");
    lockedNote(dev, "If it never answers, this device is probably locked.");
    status("");
    setBusy(false);
    return;
  }
  state.chip = state.loader.chip.CHIP_NAME;
  state.sdm = !!state.loader.secureDownloadMode;
  await readSecurity();
  await readFlashLayout();
  $("connect").hidden = true;
  $("disconnect").hidden = false;
  status("");
  setBusy(false);
}

async function readSecurity() {
  state.locked = null;
  try {
    const info = await state.loader.getSecurityInfo();
    const flags = Object.entries(info.parsedFlags || {}).filter(([, on]) => on).map(([name]) => name);
    log(`security info: flags 0x${info.flags.toString(16)} [${flags.join(", ") || "none"}], ` +
      `flash_crypt_cnt ${info.flashCryptCnt}, key purposes [${info.keyPurposes.join(", ")}]`);
    // Flash encryption is on while an odd number of SPI_BOOT_CRYPT_CNT bits is set.
    const crypt = info.flashCryptCnt.toString(2).split("1").length - 1;
    if (info.parsedFlags && info.parsedFlags.SECURE_BOOT_EN) state.locked = "Secure Boot is enabled";
    else if (crypt % 2 === 1) state.locked = "flash encryption is enabled";
  } catch (e) {
    log("security info: not available (" + (e && e.message ? e.message : e) + ")");
  }
}

async function readFlashLayout() {
  state.table = undefined;
  state.tableKind = null;
  state.flashSize = null;
  if (state.sdm) {
    state.tableKind = {
      kind: "unknown",
      why: "the chip is in Secure Download Mode (locked by an eFuse): it takes writes but refuses to be read",
    };
    return;
  }
  try {
    state.flashSize = await state.loader.detectFlashSize();
  } catch (e) {
    log("flash size: " + e);
  }
  try {
    const blob = await state.loader.readFlash(lib.TABLE_OFFSET, lib.TABLE_SIZE);
    state.table = lib.parseTable(blob);
    if (state.table) log("partition table:\n" + lib.describeTable(state.table));
    state.tableKind = lib.classifyTable(state.table, oursTable());
  } catch (e) {
    log("read partition table: " + (e && e.message ? e.message : e));
    state.tableKind = { kind: "unknown", why: "the chip refused to read its flash" };
  }
}

// The release's table, or the one in a chosen custom full image.
function oursTable() {
  if (state.manifest && state.manifest.table) return state.manifest.table;
  if (state.custom && state.custom.info.kind === "full") return state.custom.info.table;
  return null;
}

async function dropConnection() {
  try {
    if (state.transport) await state.transport.disconnect();
  } catch (e) {
    // already closed
  }
  state.transport = null;
  state.loader = null;
  state.chip = null;
  state.table = undefined;
  state.tableKind = null;
  $("connect").hidden = false;
  $("disconnect").hidden = true;
}

// ---- What can be written ------------------------------------------------------------------------

function refresh() {
  const dev = $("device");
  const plan = $("plan");
  plan.replaceChildren();
  const imgs = boardImages();
  const mode = document.querySelector('input[name="mode"]:checked').value;
  $("custom").hidden = mode !== "custom";

  if (state.loader) {
    dev.replaceChildren();
    const p = document.createElement("p");
    p.textContent = `Connected: ${state.chip}` + (state.flashSize ? `, ${state.flashSize} flash` : "") +
      (state.sdm ? ", Secure Download Mode" : "") + ".";
    dev.append(p);
    if (state.chip !== state.board.chip) {
      note(dev, "bad", `This is an ${state.chip}, but the ${state.board.label} has an ${state.board.chip}. ` +
        "Pick the right device above, or connect the right one.");
    }
    if (state.locked) {
      lockedNote(dev, `This device is locked: ${state.locked}, so firmware written over USB would not start on it.`);
    }
    if (state.tableKind) {
      const t = state.tableKind;
      if (state.table === undefined && t.kind === "unknown") {
        note(dev, "warn", `Partition table: unknown, because ${t.why}. Use the full install.`);
      } else if (t.kind === "ours") {
        note(dev, "ok", "Partition table: Witch Reader's. Full install or update only.");
      } else if (t.kind === "stock") {
        note(dev, "warn", `Partition table: ${t.why}. This is the case where the interface stays in English; ` +
          "the full install fixes it. The update alone would not.");
      } else {
        note(dev, "warn", `Partition table: ${t.why}. Use the full install.`);
      }
    }
  }

  for (const input of document.querySelectorAll("input")) input.disabled = false;
  $("connect").disabled = !("serial" in navigator);
  $("disconnect").disabled = false;
  $("reconnect").disabled = false;
  const fullInput = document.querySelector('input[name="mode"][value="full"]');
  const updateInput = document.querySelector('input[name="mode"][value="update"]');
  fullInput.disabled = !imgs.full;
  $("full-info").textContent = imgs.full
    ? "The boot loader, the partition table and the firmware, written from the start of the flash. Needed when " +
      "the device runs other firmware, or an old layout of this one."
    : "This release has no full-install image for this device. Use update only on a device that already runs " +
      "Witch Reader, or a custom file.";
  const ours = state.tableKind && state.tableKind.kind === "ours";
  updateInput.disabled = !imgs.app || (state.loader && !ours);
  $("update-info").textContent = !imgs.app
    ? "This release has no firmware image for this device."
    : "Just the firmware. Only for a device that already has this reader's partition table" +
      (state.loader && !ours ? ": this one does not." : ".");
  if (document.querySelector('input[name="mode"]:checked').disabled) {
    const firstFree = [fullInput, updateInput].find((i) => !i.disabled);
    (firstFree || document.querySelector('input[name="mode"][value="custom"]')).checked = true;
    return refresh();
  }
  $("erase").disabled = state.sdm || !isFullWrite(mode);
  if ($("erase").disabled) $("erase").checked = false;
  $("web").disabled = !webBundleFor(mode);
  if ($("web").disabled) $("web").checked = false;

  const problem = writeProblem(mode);
  if (problem) note(plan, "warn", problem);
  $("write").disabled = !!problem;
  if (state.busy) {
    for (const el of document.querySelectorAll("input, button")) el.disabled = true;
  }
}

const isFullWrite = (mode) => mode === "full" || (mode === "custom" && state.custom && state.custom.info.kind === "full");

function webBundleFor(mode) {
  if (mode === "custom") return state.customWeb || (state.manifest && state.manifest.web);
  return state.manifest && state.manifest.web;
}

function writeProblem(mode) {
  if (!state.loader) return "Connect the device first.";
  if (state.chip !== state.board.chip) return "The connected chip does not match the device picked above.";
  if (state.locked) return "This device is locked; update it from the SD card.";
  if (mode === "custom") {
    if (!state.custom) return "Choose a .bin file.";
    const info = state.custom.info;
    if (info.kind === "unknown") return `That file is ${info.why}.`;
    if (info.chip !== state.chip) return `That file is built for an ${info.chip}, this device is an ${state.chip}.`;
    if (info.kind === "app" && !(state.tableKind && state.tableKind.kind === "ours")) {
      return "A firmware image alone needs this reader's partition table on the device, which it does not have " +
        "(or which could not be read). Use a full image.";
    }
  }
  if (isFullWrite(mode) && state.flashSize && state.flashSize !== "16MB") {
    return `The images are made for 16 MB of flash; this device reports ${state.flashSize}.`;
  }
  return null;
}

// ---- Custom file --------------------------------------------------------------------------------

async function chooseCustom(file) {
  state.custom = null;
  const info = $("custom-info");
  info.textContent = "";
  if (file) {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const detected = lib.detectImage(bytes);
    state.custom = { bytes, info: detected, name: file.name };
    if (detected.kind === "full") {
      info.textContent = `${file.name}: a full image for the ${detected.chip}, ${bytes.length} bytes, written from 0x0.`;
    } else if (detected.kind === "app") {
      info.textContent = `${file.name}: a firmware image for the ${detected.chip}, ${bytes.length} bytes, ` +
        "written at 0x10000 with the boot selection reset.";
    } else {
      info.textContent = `${file.name}: ${detected.why}.`;
    }
    if (state.loader && state.table !== undefined) state.tableKind = lib.classifyTable(state.table, oursTable());
  }
  refresh();
}

async function chooseCustomWeb(file) {
  state.customWeb = null;
  if (file) {
    const data = new Uint8Array(await file.arrayBuffer());
    const sha = lib.toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", data)));
    if (lib.bundleNameMatches(file.name, sha)) state.customWeb = { name: file.name, data };
    else log(`${file.name}: not a web bundle whose name matches its SHA-256; ignored`);
  }
  refresh();
}

// ---- Write --------------------------------------------------------------------------------------

async function buildPlan(mode) {
  const imgs = boardImages();
  const otadata = async () =>
    state.manifest && state.manifest.otadata
      ? fetchImage(state.manifest.otadata)
      : new Uint8Array(lib.OTADATA_SIZE).fill(0xff); // erased otadata also boots app0
  if (mode === "full") {
    const data = await fetchImage(imgs.full);
    const info = lib.detectImage(data);
    if (info.kind !== "full" || info.chip !== state.chip) throw new Error("the full image does not match this device");
    return { files: [{ data, address: 0 }], what: `the full install of ${state.manifest.name}` };
  }
  if (mode === "update") {
    const data = await fetchImage(imgs.app);
    const info = lib.detectImage(data);
    if (info.kind !== "app" || info.chip !== state.chip) throw new Error("the firmware image does not match this device");
    return {
      files: [{ data, address: lib.APP_OFFSET }, { data: await otadata(), address: lib.OTADATA_OFFSET }],
      what: `the firmware of ${state.manifest.name}`,
    };
  }
  const { bytes, info, name } = state.custom;
  if (info.kind === "full") return { files: [{ data: bytes, address: 0 }], what: name };
  return {
    files: [{ data: bytes, address: lib.APP_OFFSET }, { data: await otadata(), address: lib.OTADATA_OFFSET }],
    what: name,
  };
}

function confirmText(mode, plan) {
  const lines = [`Write ${plan.what} to the ${state.board.label}?`];
  if (isFullWrite(mode)) {
    if (state.tableKind && state.tableKind.kind !== "ours") {
      lines.push("", "This replaces the firmware that is on it now. Anything the old firmware kept in the device's " +
        "memory (on original firmware: its Wi-Fi passwords and settings) is lost; the SD card is not touched. " +
        "There is no way back without a backup of the whole flash.");
    } else {
      lines.push("", "The SD card is not touched.");
    }
    if ($("erase").checked) lines.push("", "The whole flash is erased first.");
  }
  return lines.join("\n");
}

async function write() {
  const mode = document.querySelector('input[name="mode"]:checked').value;
  const after = $("after");
  after.replaceChildren();
  $("reconnect").hidden = true;
  let plan;
  setBusy(true);
  try {
    status("Loading the image...");
    plan = await buildPlan(mode);
  } catch (e) {
    note(after, "bad", "Could not prepare the image: " + (e.message || e));
    status("");
    setBusy(false);
    return;
  }
  if (!window.confirm(confirmText(mode, plan))) {
    status("");
    setBusy(false);
    return;
  }
  const progress = $("progress");
  progress.hidden = false;
  progress.value = 0;
  const total = plan.files.reduce((n, f) => n + f.data.length, 0);
  const done = plan.files.map(() => 0);
  try {
    status($("erase").checked ? "Erasing the flash, then writing..." : "Writing...");
    await state.loader.writeFlash({
      fileArray: plan.files,
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: $("erase").checked,
      compress: true,
      reportProgress: (i, written, size) => {
        done[i] = (written / size) * plan.files[i].data.length;
        progress.value = (100 * done.reduce((a, b) => a + b, 0)) / total;
      },
      // Secure Download Mode refuses the flash MD5 read-back.
      calculateMD5Hash: state.sdm ? undefined : (image) => lib.md5Hex(image),
    });
  } catch (e) {
    log("write: " + (e && e.message ? e.message : e));
    note(after, "warn", "Writing failed (" + (e && e.message ? e.message : e) + "). Reconnect and write again: " +
      "until a write completes, the device starts its old firmware, or none.");
    lockedNote(after, "If it fails again at the start, the device refuses writes: it is locked.");
    status("");
    await dropConnection();
    setBusy(false);
    return;
  }
  progress.value = 100;
  note(after, "ok", state.sdm ? "Written (Secure Download Mode cannot verify it)." : "Written and verified.");
  status("Restarting the device...");
  await restart();
  await dropConnection();
  refresh();

  const bundle = $("web").checked ? await webBundle(mode) : null;
  if (bundle) {
    await installBundle(bundle);
  } else {
    note(after, "ok", "Done. The device starts the new firmware now.");
    status("");
  }
  setBusy(false);
}

// The C3 boards leave the ROM loader by an RTS reset, the S3 boards by the RTC watchdog, as esptool's
// --after watchdog-reset (the RTS path can leave an S3 in download mode). Secure Download Mode
// refuses register writes, so there it falls back to RTS, as esptool does.
async function restart() {
  try {
    // usingUsbOtg false: every board here is on the chip's USB Serial/JTAG port, and asking the
    // chip would read a register, which Secure Download Mode refuses.
    if (state.board.reset === "watchdog" && !state.sdm) await watchdogResetS3(state.loader);
    else await state.loader.after("hard_reset", false);
  } catch (e) {
    log("reset: " + (e && e.message ? e.message : e));
  }
}

async function watchdogResetS3(loader) {
  const RTC_CNTL_BASE = 0x60008000;
  const WDTWPROTECT = RTC_CNTL_BASE + 0xb0;
  const WDTCONFIG0 = RTC_CNTL_BASE + 0x98;
  const WDTCONFIG1 = RTC_CNTL_BASE + 0x9c;
  const WDT_WKEY = 0x50d83aa1;
  log("Hard resetting with a watchdog...");
  await loader.writeReg(WDTWPROTECT, WDT_WKEY); // unlock
  await loader.writeReg(WDTCONFIG1, 2000); // timeout
  await loader.writeReg(WDTCONFIG0, ((1 << 31) | (5 << 28) | (1 << 8) | 2) >>> 0); // enable
  await loader.writeReg(WDTWPROTECT, 0); // lock
  await sleep(500);
}

async function webBundle(mode) {
  try {
    if (mode === "custom" && state.customWeb) return state.customWeb;
    const web = state.manifest && state.manifest.web;
    if (!web) return null;
    return { name: web.name, data: await fetchImage(web) };
  } catch (e) {
    note($("after"), "warn", "The web interface could not be loaded (" + (e.message || e) + "). It installs itself " +
      "the first time you open the device's web pages.");
    return null;
  }
}

// After the restart the device leaves the USB bus and comes back as a new port. The browser lets
// the page reopen a port it was granted without asking, so look for it among those.
async function findPort(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  await sleep(2000);
  while (Date.now() < deadline) {
    const ports = await navigator.serial.getPorts();
    for (const port of ports) {
      const info = port.getInfo();
      if (info.usbVendorId !== ESPRESSIF_VID) continue;
      const link = new DeviceLink(port, log);
      try {
        await link.open();
        return link;
      } catch (e) {
        // not back yet, or still the old port
      }
    }
    await sleep(1000);
  }
  return null;
}

async function installBundle(bundle, link = null) {
  const after = $("after");
  status("Waiting for the device to start...");
  link = link || (await findPort(30000));
  if (!link) {
    note(after, "warn", "The device did not come back on USB by itself. Wake it (see \"If something goes " +
      "wrong\"), then press \"Pick the device again\" to install the web interface, or leave it: the pages " +
      "install themselves the first time you open them.");
    $("reconnect").hidden = false;
    state.pendingBundle = bundle;
    status("");
    return;
  }
  try {
    const result = await installWebBundle(link, bundle, (text) => status(text));
    log("web interface: " + result);
    if (result === "installed" || result === "already installed") {
      note(after, "ok", `Done. The web interface is ${result}; the device restarts to Home.`);
    } else {
      note(after, "warn", `The web interface was not installed over USB (${result}). The firmware is installed; ` +
        "the pages install themselves the first time you open them.");
    }
  } catch (e) {
    log("web interface: " + (e && e.message ? e.message : e));
    note(after, "warn", "The firmware is installed, but the web interface could not be installed over USB (" +
      (e && e.message ? e.message : e) + "). The pages install themselves the first time you open them.");
  } finally {
    await link.close();
    status("");
  }
}

async function reconnect() {
  let port;
  try {
    port = await navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF_VID }] });
  } catch (e) {
    return;
  }
  $("reconnect").hidden = true;
  const link = new DeviceLink(port, log);
  try {
    await link.open();
  } catch (e) {
    note($("after"), "bad", "Could not open the port: " + (e.message || e));
    return;
  }
  setBusy(true);
  await installBundle(state.pendingBundle, link);
  state.pendingBundle = null;
  setBusy(false);
}

// While busy every control is off; refresh() decides what is on again.
function setBusy(busy) {
  state.busy = busy;
  refresh();
}

// ---- Start --------------------------------------------------------------------------------------

async function start() {
  renderBoards();
  if (!("serial" in navigator)) {
    $("unsupported").hidden = false;
    $("connect").disabled = true;
  }
  await loadManifest();
  $("connect").addEventListener("click", connect);
  $("disconnect").addEventListener("click", async () => {
    await dropConnection();
    $("device").replaceChildren();
    refresh();
  });
  $("write").addEventListener("click", write);
  $("reconnect").addEventListener("click", reconnect);
  $("custom-file").addEventListener("change", (e) => chooseCustom(e.target.files[0]));
  $("custom-web").addEventListener("change", (e) => chooseCustomWeb(e.target.files[0]));
  for (const input of document.querySelectorAll('input[name="mode"]')) input.addEventListener("change", refresh);
  navigator.serial?.addEventListener("disconnect", (e) => {
    if (state.port && e.target === state.port && !state.busy) {
      log("the device left the USB bus");
      dropConnection().then(refresh);
    }
  });
  refresh();
}

start();
