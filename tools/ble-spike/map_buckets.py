"""Group map deltas by component, loaded sections only. usage: map_buckets.py BASE.map SPIKE.map"""
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from map_attrib import parse  # noqa: E402

SECTIONS = {".iram0.text": "iram", ".dram0.data": "data", ".dram0.bss": "bss", ".flash.text": "text",
            ".flash.rodata": "rodata"}


def bucket(owner):
    if owner in ("libbtdm_app.a", "libbtbb.a", "libbt.a"):
        return "controller+IDF bt glue (prebuilt)"
    if owner.startswith("src/NimBLE"):
        return "NimBLE-Arduino C++ wrapper"
    if owner in ("src/BleKeyboardHost.cpp.o", "src/HidKeymap.cpp.o"):
        return "SDK BleKeyboardHost"
    if owner == "src/BleSpike.cpp":
        return "spike harness"
    if owner.startswith("src/") and owner.endswith(".c"):
        return "NimBLE host (C)"
    return "other (" + owner + ")"


base, _ = parse(sys.argv[1])
spike, _ = parse(sys.argv[2])
table = defaultdict(lambda: defaultdict(int))
others = defaultdict(int)
for key in set(base) | set(spike):
    sec, owner = key
    if sec not in SECTIONS:
        continue
    delta = spike.get(key, 0) - base.get(key, 0)
    name = bucket(owner)
    if name.startswith("other"):
        others[(SECTIONS[sec], owner)] += delta
        name = "other"
    table[name][SECTIONS[sec]] += delta
cols = ["iram", "data", "bss", "text", "rodata"]
print(f"{'component':38s}" + "".join(f"{c:>9s}" for c in cols) + f"{'flash*':>9s}")
for name in sorted(table, key=lambda n: -sum(table[n].values())):
    row = table[name]
    flash = row["iram"] + row["data"] + row["text"] + row["rodata"]
    print(f"{name:38s}" + "".join(f"{row[c]:+9d}" for c in cols) + f"{flash:+9d}")
print("\n(* flash = iram + data + text + rodata: what the image stores)")
print("\nlargest 'other' entries:")
for (sec, owner), d in sorted(others.items(), key=lambda kv: -abs(kv[1]))[:12]:
    print(f"  {sec:7s} {owner:50s} {d:+7d}")
