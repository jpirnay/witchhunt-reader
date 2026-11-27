"""Sum a GNU ld map file's input sections by (output section, archive), and diff two maps.

usage: map_attrib.py BASE.map SPIKE.map
"""
import re
import sys
from collections import defaultdict

OUT_SECTIONS = (".iram0.text", ".iram0.vectors", ".iram0.data", ".iram0.bss", ".dram0.data", ".dram0.bss",
                ".flash.text", ".flash.rodata", ".flash.appdesc", ".noinit", ".rtc.text", ".rtc.data",
                ".rtc.bss", ".flash.init_array", ".flash_rodata_dummy", ".iram0.text_end", ".dram0.dummy")

out_re = re.compile(r"^(\.[\w.]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
out_name_only = re.compile(r"^(\.[\w.]+)\s*$")
in_full = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$")
in_name_only = re.compile(r"^ (\S+)\s*$")
in_cont = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$")


def owner(path):
    path = path.strip().replace("\\", "/")
    m = re.search(r"([^/]+\.a)\(", path)
    if m:
        lib = m.group(1)
        # PlatformIO lib archives are named libXXX.a inside .pio/build/<env>/libNNN/
        dm = re.search(r"/(lib[0-9a-f]+)/([^/]+\.a)\(", path)
        return dm.group(2) if dm else lib
    if "/src/" in path or path.endswith(".o"):
        m = re.search(r"/src/(.+?)\.o$", path)
        return "src/" + (m.group(1) if m else path.rsplit("/", 1)[-1])
    return path.rsplit("/", 1)[-1]


def parse(path):
    sizes = defaultdict(int)
    totals = {}
    current = None
    pending_out = None
    pending_in = None
    in_map = False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not in_map:
                if line.startswith("Linker script and memory map"):
                    in_map = True
                continue
            if pending_out:
                m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", line)
                if m:
                    totals[pending_out] = int(m.group(2), 16)
                pending_out = None
                continue
            m = out_re.match(line)
            if m:
                current = m.group(1)
                totals[current] = int(m.group(3), 16)
                continue
            m = out_name_only.match(line)
            if m:
                current = m.group(1)
                pending_out = current
                continue
            if current is None:
                continue
            if pending_in:
                m = in_cont.match(line)
                pending_in = None
                if m and int(m.group(1), 16) != 0:
                    sizes[(current, owner(m.group(3)))] += int(m.group(2), 16)
                continue
            m = in_full.match(line)
            if m:
                if m.group(1).startswith("*") or int(m.group(2), 16) == 0:
                    continue
                sizes[(current, owner(m.group(4)))] += int(m.group(3), 16)
                continue
            m = in_name_only.match(line)
            if m and not m.group(1).startswith("*"):
                pending_in = m.group(1)
    return sizes, totals


def main():
    base, base_tot = parse(sys.argv[1])
    spike, spike_tot = parse(sys.argv[2])
    print("== output section totals (base -> spike, delta) ==")
    for sec in sorted(set(base_tot) | set(spike_tot)):
        b, s = base_tot.get(sec, 0), spike_tot.get(sec, 0)
        if b or s:
            print(f"{sec:24s} {b:9d} -> {s:9d}  {s - b:+8d}")
    print("\n== per (section, owner) deltas, |delta| >= 64 ==")
    rows = []
    for key in set(base) | set(spike):
        d = spike.get(key, 0) - base.get(key, 0)
        if abs(d) >= 64:
            rows.append((key[0], -abs(d), key[1], d))
    for sec, _, own, d in sorted(rows):
        print(f"{sec:24s} {own:48s} {d:+8d}")
    print("\n== per owner, all sections (spike - base) ==")
    per = defaultdict(int)
    for key in set(base) | set(spike):
        per[key[1]] += spike.get(key, 0) - base.get(key, 0)
    for own, d in sorted(per.items(), key=lambda kv: -abs(kv[1])):
        if abs(d) >= 256:
            print(f"{own:48s} {d:+8d}")


if __name__ == "__main__":
    main()
