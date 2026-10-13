"""THROWAWAY (TTF step 0): MMU pages an app image maps on the ESP32-C3.

128 pages of 64 KB are shared by the app's code (IROM, 0x42xxxxxx) and its rodata (DROM,
0x3Cxxxxxx), and the two buses index the SAME table: page i of IROM and page i of DROM are one
entry. So the image takes the UNION of the page indices its segments touch, not their sum. (The
sum was the first version of this script; on the bench image it predicted 94 free pages where the
device reported 100, and on the reader image it predicted more pages than the chip has.)"""
import subprocess
import sys

readelf, elf = sys.argv[1], sys.argv[2]
out = subprocess.run([readelf, "-lW", elf], capture_output=True, text=True, check=True).stdout
used = set()
for line in out.splitlines():
    parts = line.split()
    if not parts or parts[0] != "LOAD":
        continue
    va, memsz = int(parts[2], 16), int(parts[5], 16)
    for base in (0x3C000000, 0x42000000):
        if base <= va < base + 0x800000:
            first, last = (va - base) // 0x10000, (va - base + memsz - 1) // 0x10000
            used.update(range(first, last + 1))
            print(f"  segment 0x{va:08x} size {memsz:>9} -> pages {first}..{last}")
print(f"{elf}: {len(used)} pages mapped by the image, {128 - len(used)} free for runtime mappings")
