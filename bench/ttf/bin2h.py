"""THROWAWAY (TTF step 0): embed a font file in the bench image as a C array."""
import pathlib
import sys

src, dst, name = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
data = src.read_bytes()
lines = [f"// Generated from {src.name} by bench/ttf/bin2h.py. Do not commit.",
         "#pragma once", "#include <cstdint>",
         f"alignas(4) static const uint8_t {name}[] = {{"]
for i in range(0, len(data), 24):
    lines.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
lines.append("};")
lines.append(f"static constexpr uint32_t {name}Len = {len(data)};")
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text("\n".join(lines) + "\n", encoding="ascii")
print(f"{dst}: {len(data)} bytes")
