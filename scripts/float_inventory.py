"""Lists the firmware's own functions that do floating-point maths on the C3, from a built ELF.

The C3 has no FPU, so every float or double operation is a call into a libgcc soft-float helper (__mulsf3,
__divdf3, ...) or a libm routine. This disassembles the ELF, finds every call to one of them, and attributes
it to the calling function and its source file: what runs on the device, with nothing from comments, names
or compiled-out code. Used to plan and check the integer-maths work (docs/design/integer-math.md).

    python scripts/float_inventory.py .pio/build/default/firmware.elf [--all] [--helpers]

--all also lists callers outside src/, lib/ and freeink-sdk/ (newlib, ArduinoJson, libstdc++, ESP-IDF), which
keep most helpers in the image whatever our code does. --helpers lists each linked helper with its size and
whether only our code calls it (those leave the image once our code stops).
"""

import argparse
import collections
import glob
import os
import re
import subprocess
import sys

SOFT_FLOAT = re.compile(
    r"^__(?:(?:add|sub|mul|div|neg|eq|ne|ge|gt|le|lt|unord|cmp)(?:sf|df|tf)\d"
    r"|(?:fix|fixuns)(?:sf|df)(?:si|di)"
    r"|float(?:un)?(?:si|di)(?:sf|df)"
    r"|extendsfdf2|truncdfsf2|trunctfdf2|extenddftf2)$"
)
LIBM = re.compile(
    r"^(?:sqrt|cbrt|hypot|pow|exp|exp2|log|log2|log10|sin|cos|tan|asin|acos|atan|atan2"
    r"|floor|ceil|round|lround|lrint|rint|trunc|fmod|ldexp|frexp|modf|scalbn)f?$"
    r"|^(?:strtof|strtod|_strtod_r|atof|_dtoa_r)$"
)
FUNCTION = re.compile(r"^([0-9a-f]{8}) <(.+)>:$")
REFERENCE = re.compile(r"<([^>+]+)>")


def is_float(symbol):
    return bool(SOFT_FLOAT.match(symbol) or LIBM.match(symbol))


def tool(name):
    found = glob.glob(os.path.expanduser(f"~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-{name}"))
    if not found:
        sys.exit(f"float_inventory: no riscv32-esp-elf-{name} in ~/.platformio/packages/toolchain-riscv32-esp")
    return found[0]


def float_calls(elf):
    """{function: Counter(helper -> calls)} and {function: start address}."""
    calls = collections.defaultdict(collections.Counter)
    starts = {}
    current = None
    proc = subprocess.Popen([tool("objdump"), "-d", "--no-show-raw-insn", elf], stdout=subprocess.PIPE, text=True)
    for line in proc.stdout:
        m = FUNCTION.match(line)
        if m:
            current = m.group(2)
            starts[current] = m.group(1)
            continue
        if current is None or ":\t" not in line:
            continue
        for ref in REFERENCE.finditer(line):
            symbol = ref.group(1)
            if symbol != current and is_float(symbol):
                calls[current][symbol] += 1
    proc.wait()
    return calls, starts


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf")
    parser.add_argument("--all", action="store_true", help="also list third-party callers")
    parser.add_argument("--helpers", action="store_true", help="list the linked helpers")
    args = parser.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    calls, starts = float_calls(args.elf)
    names = list(calls)
    files = subprocess.run(
        [tool("addr2line"), "-e", args.elf] + [starts[n] for n in names], capture_output=True, text=True
    ).stdout.splitlines()
    demangled = subprocess.run(
        [tool("c++filt")], input="\n".join(names), capture_output=True, text=True
    ).stdout.splitlines()

    by_file = collections.defaultdict(list)
    helper_users = collections.defaultdict(set)
    for name, file, pretty in zip(names, files, demangled):
        path = file.split(":")[0]
        rel = os.path.relpath(path, root) if path.startswith(root + "/") else path
        ours = rel.startswith(("src/", "lib/", "freeink-sdk/"))
        for helper in calls[name]:
            helper_users[helper].add("ours" if ours else "other")
        if ours or args.all:
            by_file[rel].append((sum(calls[name].values()), re.sub(r"\(.*", "", pretty)))

    total = 0
    for rel, functions in sorted(by_file.items(), key=lambda kv: -sum(n for n, _ in kv[1])):
        count = sum(n for n, _ in functions)
        total += count
        print(f"{count:5}  {rel}")
        for n, fn in sorted(functions, reverse=True):
            print(f"       {n:4}  {fn}")
    print(f"{total} call sites in {sum(len(f) for f in by_file.values())} functions")

    if args.helpers:
        sizes = collections.Counter()
        out = subprocess.run([tool("nm"), "-S", args.elf], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 4 and is_float(parts[3]):
                sizes[parts[3]] += int(parts[1], 16)
        only_ours = 0
        for helper, size in sizes.most_common():
            users = helper_users.get(helper, set())
            note = "only ours" if users == {"ours"} else ("" if users else "called by other helpers")
            only_ours += size if users == {"ours"} else 0
            print(f"{size:6}  {helper:16} {note}")
        print(f"{sum(sizes.values())} bytes of helpers, {only_ours} of them called only by our code")


if __name__ == "__main__":
    main()
