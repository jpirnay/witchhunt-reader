"""The web bundle's identity must not depend on the firmware build.

Builds the bundle with scripts/build_html.py in a scratch copy of the pieces it reads and checks
web_bundle::PAGES_ID and DIGEST in the generated header:
  - another CROSSPOINT_VERSION, even for a page that carries %%VERSION%%: identical;
  - CRLF line endings in a bundled text file: identical;
  - an edited page, or another WEB_API_VERSION: PAGES_ID differs;
  - a build with terser available differs in DIGEST at most, never in PAGES_ID.
Needs `pip install zopfli`. Usage: python test/web/bundle_identity_test.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join("src", "network", "html", "WebBundle.generated.h")


def make_project(dest):
    os.makedirs(dest)
    shutil.copy(os.path.join(ROOT, "platformio.ini"), dest)
    os.makedirs(os.path.join(dest, "scripts"))
    shutil.copy(os.path.join(ROOT, "scripts", "build_html.py"), os.path.join(dest, "scripts"))
    shutil.copytree(os.path.join(ROOT, "src", "network", "html"), os.path.join(dest, "src", "network", "html"))
    for sub in ("dist", "js"):
        shutil.copytree(os.path.join(ROOT, "tools", "ftwasm", sub), os.path.join(dest, "tools", "ftwasm", sub))


def build(project, version):
    env = dict(os.environ, CROSSPOINT_NAME="Test", CROSSPOINT_VERSION=version, PYTHONUTF8="1")
    env.pop("PIOENV", None)
    r = subprocess.run(
        [sys.executable, os.path.join("scripts", "build_html.py")],
        cwd=project, env=env, capture_output=True, text=True,
    )
    if r.returncode != 0:
        raise AssertionError(f"build_html.py failed:\n{r.stdout}\n{r.stderr}")
    with open(os.path.join(project, HEADER), encoding="utf-8") as f:
        header = f.read()

    def array(name):
        body = re.search(rf"{name}\[32\] = \{{(.*?)\}};", header, re.S).group(1)
        return "".join(re.findall(r"0x([0-9a-f]{2})", body))

    return array("PAGES_ID"), array("DIGEST")


def edit(path, fn, binary=False):
    """Rewrite a file with fn, and make it newer than any output cached from the old text."""
    kw = {} if binary else {"encoding": "utf-8", "newline": ""}
    with open(path, "rb" if binary else "r", **kw) as f:
        data = f.read()
    with open(path, "wb" if binary else "w", **kw) as f:
        f.write(fn(data))
    now = os.path.getmtime(path) + 5
    os.utime(path, (now, now))


def main():
    failures = []

    def check(ok, what):
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "p")
        make_project(p)
        page = os.path.join(p, "src", "network", "html", "HomePage.html")
        # A page that carries the version placeholder must ship it unchanged.
        edit(page, lambda s: s.replace("</body>", "<span id=\"v\">%%VERSION%%</span></body>", 1))

        a = build(p, "2.40-dev+one")
        b = build(p, "9.99-rc.7+other")
        check(a == b, "another CROSSPOINT_VERSION: PAGES_ID and DIGEST identical (page with %%VERSION%%)")

        bundle = [f for f in os.listdir(os.path.join(p, ".pio", "web")) if f.startswith("web-")]
        with open(os.path.join(p, ".pio", "web", bundle[0]), "rb") as f:
            blob = f.read()
        check(b"9.99-rc.7" not in blob, "the version is not in the bundle bytes")

        # The bundle is gzip members back to back; the placeholder is in a page, unsubstituted.
        members, rest = [], blob
        while rest:
            z = zlib.decompressobj(31)
            members.append(z.decompress(rest))
            rest = z.unused_data
        check(any(b"%%VERSION%%" in m for m in members), "%%VERSION%% ships unchanged in a bundled page")

        # CRLF in a bundled text file (a Windows checkout) changes nothing.
        mjs = os.path.join(p, "tools", "ftwasm", "js", "kern-reduce.mjs")
        edit(mjs, lambda d: d.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"), binary=True)
        check(build(p, "2.40-dev+one") == a, "CRLF line endings in a bundled file: identical")

        # A changed page changes the identity.
        edit(page, lambda s: s.replace("</body>", "<!-- x --><p>changed</p></body>", 1))
        c = build(p, "2.40-dev+one")
        check(c[0] != a[0] and c[1] != a[1], "an edited page changes PAGES_ID and DIGEST")

        # A changed WEB_API_VERSION changes PAGES_ID, not the bytes.
        script = os.path.join(p, "scripts", "build_html.py")
        edit(page, lambda s: s.replace("<p>changed</p>", "", 1).replace("<!-- x -->", "", 1))
        base = build(p, "2.40-dev+one")
        check(base == a, "reverting the page restores the identity")
        edit(script, lambda s: re.sub(r"WEB_API_VERSION = (\d+)", lambda m: f"WEB_API_VERSION = {int(m.group(1)) + 1}", s, count=1))
        d = build(p, "2.40-dev+one")
        check(d[0] != a[0] and d[1] == a[1], "another WEB_API_VERSION changes PAGES_ID only")

        # Terser changes how the pages are compressed, not what they are.
        node_modules = os.path.join(ROOT, "node_modules", "terser")
        if os.path.isdir(node_modules) and shutil.which("node"):
            shutil.copytree(os.path.join(ROOT, "node_modules"), os.path.join(p, "node_modules"))
            edit(script, lambda s: re.sub(r"WEB_API_VERSION = (\d+)", lambda m: f"WEB_API_VERSION = {int(m.group(1)) - 1}", s, count=1))
            for root, _, files in os.walk(os.path.join(p, "src")):
                for f in files:
                    if f.endswith(".generated.gz"):
                        os.remove(os.path.join(root, f))  # cached without terser
            t = build(p, "2.40-dev+one")
            check(t[1] != a[1], "with terser installed the bytes differ ...")
            check(t[0] == a[0], "... and PAGES_ID is identical")
        else:
            print("skip terser comparison (node_modules/terser not installed)")

    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()
