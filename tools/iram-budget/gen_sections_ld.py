#!/usr/bin/env python3
"""Regenerate the ESP32-C3 sections.ld of pioarduino's prebuilt Arduino libraries with some linker-only Kconfig options
changed (exp/iram-budget: code moved out of IRAM, which on the C3 is carved from the heap's SRAM). It runs ESP-IDF's
own ldgen (fragment grammar, generation model, template filling) at the IDF commit the libraries were built from,
against the prebuilt libraries themselves: nothing is recompiled, so only options that change linker fragments alone
may be flipped (options.txt says which, and why each is linker-only).

    tools/iram-budget/fetch_idf.sh <idf dir>          # the pinned IDF's fragments, templates and ldgen
    python3 -m venv <venv> && <venv>/bin/pip install pyparsing==3.3.3
    <venv>/bin/python tools/iram-budget/gen_sections_ld.py --idf <idf dir> --check
    <venv>/bin/python tools/iram-budget/gen_sections_ld.py --idf <idf dir>     # writes ld/esp32c3-iram-budget/

--check regenerates with no option changed and requires the platform's own sections.ld, byte for byte: the proof that
the inputs (IDF commit, fragment selection, libraries, template preprocessing) are the ones the libraries were built
with. The output records the sha256 of that platform file; scripts/iram_budget.py refuses to link when it differs.
kconfiglib is not needed: fragment conditions are evaluated against the sdkconfig file itself (an undefined symbol is
n, as kconfiglib evaluates it with warnings off).
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile


def read_sdkconfig(path):
    values = {}
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        m = re.match(r"^CONFIG_([A-Za-z0-9_]+)=(.*)$", line)
        if m:
            v = m.group(2)
            values[m.group(1)] = v[1:-1] if v.startswith('"') else v
            continue
        m = re.match(r"^# CONFIG_([A-Za-z0-9_]+) is not set$", line)
        if m:
            values[m.group(1)] = "n"
    return values


class Expr:
    """Kconfig expressions as fragments use them: symbols, y/n, strings, numbers, = != < <= > >=, !, &&, ||, ()."""

    TOKEN = re.compile(r'\s*(\|\||&&|!=|<=|>=|=|<|>|!|\(|\)|"[^"]*"|[A-Za-z0-9_]+)')

    def __init__(self, values):
        self.values = values

    def evaluate(self, text):
        self.tokens = []
        pos = 0
        text = text.strip()
        while pos < len(text):
            m = self.TOKEN.match(text, pos)
            if not m:
                raise ValueError("cannot parse %r" % text)
            self.tokens.append(m.group(1))
            pos = m.end()
        self.i = 0
        result = self.parse_or()
        if self.i != len(self.tokens):
            raise ValueError("trailing tokens in %r" % text)
        return result

    def peek(self):
        return self.tokens[self.i] if self.i < len(self.tokens) else None

    def take(self):
        t = self.tokens[self.i]
        self.i += 1
        return t

    def parse_or(self):
        v = self.parse_and()
        while self.peek() == "||":
            self.take()
            v = max(v, self.parse_and())
        return v

    def parse_and(self):
        v = self.parse_not()
        while self.peek() == "&&":
            self.take()
            v = min(v, self.parse_not())
        return v

    def parse_not(self):
        if self.peek() == "!":
            self.take()
            return 2 - self.parse_not()
        return self.parse_cmp()

    def value(self, token):
        if token.startswith('"'):
            return token[1:-1]
        if token in ("y", "n", "m"):
            return token
        if re.match(r"^-?[0-9]+$|^0x[0-9a-fA-F]+$", token):
            return token
        return self.values.get(token, "n")

    def parse_cmp(self):
        if self.peek() == "(":
            self.take()
            v = self.parse_or()
            assert self.take() == ")"
            return v
        left = self.value(self.take())
        op = self.peek()
        if op in ("=", "!=", "<", "<=", ">", ">="):
            self.take()
            right = self.value(self.take())
            if op in ("=", "!="):
                equal = left == right
                return 2 if equal == (op == "=") else 0
            try:  # as kconfiglib: numbers compare as numbers, anything else as strings
                a, b = int(left, 0), int(right, 0)
            except ValueError:
                a, b = left, right
            return 2 if {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op] else 0
        return 2 if left == "y" else 0


NOT_FOR_C3 = {  # IDF v5.5.5: chosen by the components' CMakeLists for other targets or options
    "bt/controller/linker_esp32c2.lf", "bt/controller/linker_esp_ble_controller.lf", "freertos/linker_smp.lf",
    "newlib/src/picolibc/libc.lf",
}


class SDKConfig:
    def __init__(self, values):
        self.expr = Expr(values)

    def evaluate_expression(self, expression):
        return self.expr.evaluate(expression) == 2


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    packages = os.path.expanduser("~/.platformio/packages")
    ap.add_argument("--idf", required=True, help="fetch_idf.sh's checkout")
    ap.add_argument("--libs", default=os.path.join(packages, "framework-arduinoespressif32-libs", "esp32c3"))
    ap.add_argument("--toolchain", default=os.path.join(packages, "toolchain-riscv32-esp", "bin"))
    ap.add_argument("--options", default=os.path.join(here, "options.txt"))
    ap.add_argument("--out", default=os.path.join(repo, "ld", "esp32c3-iram-budget", "sections.ld"))
    ap.add_argument("--keep", default=os.path.join(here, "keep_in_iram.txt"),
                    help="fragment entries to drop, so the functions keep the archive's IRAM placement")
    ap.add_argument("--check", action="store_true", help="no option changed: must reproduce the platform's file")
    args = ap.parse_args()

    sys.path.insert(0, os.path.join(args.idf, "tools", "ldgen"))
    from ldgen.entity import EntityDB
    from ldgen.fragments import parse_fragment_file
    from ldgen.generation import Generation
    from ldgen.linker_script import LinkerScript
    from io import StringIO

    values = read_sdkconfig(os.path.join(args.libs, "sdkconfig"))
    overrides = {} if args.check else read_sdkconfig(args.options)
    for k, v in overrides.items():
        if k not in values:
            sys.exit("option CONFIG_%s is not in the prebuilt sdkconfig" % k)
        values[k] = v
    sdkconfig = SDKConfig(values)

    # The template, preprocessed as IDF's ld.cmake does, with the prebuilt sdkconfig.h (the options flipped here are
    # linker-only, so the template does not read them).
    ld_dir = os.path.join(args.idf, "components", "esp_system", "ld")
    with tempfile.TemporaryDirectory() as tmp:
        template = os.path.join(tmp, "sections.ld.in")
        cc = os.path.join(args.toolchain, "riscv32-esp-elf-gcc")
        text = subprocess.run([cc, "-C", "-P", "-x", "c", "-E", "-I", os.path.join(args.libs, "dio_qspi", "include"),
                               "-I", ld_dir, os.path.join(ld_dir, "esp32c3", "sections.ld.in")],
                              check=True, capture_output=True, text=True).stdout
        with open(template, "w", encoding="utf-8") as f:
            f.write(text.replace("\\n", "\n"))  # as ld.cmake does

        # The build's libraries: lib/, the DIO flash mode's libspi_flash.a (this firmware's flash mode) and ld/'s blobs.
        libs = sorted(os.path.join(args.libs, d, f) for d in ("lib", "dio_qspi", "ld")
                      for f in os.listdir(os.path.join(args.libs, d)) if f.endswith(".a"))
        components = {os.path.basename(l)[3:-2] for l in libs}
        objdump = os.path.join(args.toolchain, "riscv32-esp-elf-objdump")
        entities = EntityDB()
        for lib in libs:
            dump = StringIO(subprocess.check_output([objdump, "-h", lib], env=dict(os.environ, LC_ALL="C")).decode())
            dump.name = lib
            entities.add_sections_info(dump)

        fragments = []
        for root, _, files in os.walk(os.path.join(args.idf, "components")):
            for f in files:
                if not f.endswith(".lf"):
                    continue
                component = os.path.relpath(root, os.path.join(args.idf, "components")).split(os.sep)[0]
                if component not in components:
                    continue
                rel = os.path.relpath(os.path.join(root, f), os.path.join(args.idf, "components"))
                # The components' CMakeLists choose these per target and option; for the C3 (not SMP, no SPIRAM
                # workaround, newlib, TWAI without FD) they are:
                if rel in NOT_FOR_C3:
                    continue
                if rel.startswith("bt/") and values.get("BT_ENABLED") != "y" and rel != "bt/linker_common.lf":
                    continue
                path = os.path.join(root, f)
                fragments.append(path)
        fragments.sort()

        # The keep list: `<fragment> <object>:<function>` lines whose `(default)` (flash) entry is dropped from a copy
        # of the fragment, so the function falls back to its archive's rule (for libfreertos.a, FREERTOS_IN_IRAM's
        # `* (noflash_text)`). Each entry must be found exactly once.
        keep = {}
        if not args.check:
            for line in open(args.keep, encoding="utf-8"):
                line = line.split("#", 1)[0].strip()
                if line:
                    fragment, entity = line.split()
                    keep.setdefault(fragment, []).append(entity)
        components_dir = os.path.join(args.idf, "components")
        for fragment, kept in keep.items():
            source = os.path.join(components_dir, fragment)
            if source not in fragments:
                sys.exit("keep list: %s is not a fragment of this build" % fragment)
            lines = open(source, encoding="utf-8").read().split("\n")
            for entity in kept:
                hits = [i for i, l in enumerate(lines) if re.match(r"^\s+%s \(default\)\s*$" % re.escape(entity), l)]
                if len(hits) != 1:
                    sys.exit("keep list: %s matches %d lines of %s" % (entity, len(hits), fragment))
                lines[hits[0]] = re.sub(r"\S.*$", "# kept in IRAM (keep_in_iram.txt): " + entity, lines[hits[0]])
            copy = os.path.join(tmp, fragment.replace(os.sep, "_"))
            with open(copy, "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
            fragments[fragments.index(source)] = copy

        generation = Generation(False, None)
        for f in fragments:
            generation.add_fragments_from_file(parse_fragment_file(f, sdkconfig))
        rules = generation.generate(entities, sdkconfig.evaluate_expression("SOC_MEM_NON_CONTIGUOUS_SRAM"))
        script = LinkerScript(open(template))
        script.fill(rules)
        out = StringIO()
        script.write(out)
    text = re.sub(r"^/\* Generated from: .* \*/$", "/* Generated from: esp_system/ld/sections.ld.in */",
                  out.getvalue(), count=1, flags=re.M)
    original_path = os.path.join(args.libs, "ld", "sections.ld")
    original = open(original_path, encoding="utf-8").read()
    if args.check:
        expected = re.sub(r"^/\* Generated from: .* \*/$", "/* Generated from: esp_system/ld/sections.ld.in */",
                          original, count=1, flags=re.M)
        if text != expected:
            sys.exit("--check: the regenerated script differs from %s" % original_path)
        print("--check: %d fragments, %d libraries reproduce %s" % (len(fragments), len(libs), original_path))
        return
    digest = hashlib.sha256(original.encode("utf-8")).hexdigest()
    header = ("/* exp/iram-budget: generated by tools/iram-budget/gen_sections_ld.py; do not edit. */\n"
              "/* source-sha256: %s (framework-arduinoespressif32-libs/esp32c3/ld/sections.ld) */\n"
              "/* options: %s */\n"
              "/* kept in IRAM: %s */\n" % (digest, ", ".join("CONFIG_%s=%s" % kv for kv in sorted(overrides.items())),
                                            ", ".join(e for es in keep.values() for e in es)))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(header + text)
    print("%d fragments, %d libraries -> %s" % (len(fragments), len(libs), args.out), file=sys.stderr)


if __name__ == "__main__":
    main()
