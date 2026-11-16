"""The web flasher's site offers the right releases, each with the images and table it carries.

scripts/stage_flasher_site.py stages the site deploy_flasher.yml publishes. Checks:
  * select_releases: the last five full releases, newest first, and ahead of them only the
    pre-releases published after the newest full release (at most three); drafts and releases
    without a board image are left out;
  * stage_release: each release in its own folder, every image listed with its size and SHA-256; the
    otadata and partition table taken from the full images, or the table from the release's
    partitions.csv when it has none; two full images with different tables stop the staging;
  * releases.json lists the releases as the page reads them, with paths the page can fetch;
  * --assets (one release) is refused inside GitHub Actions, where only a re-run of an older
    release's deploy-flasher job uses it, and still stages on a desktop.
Usage: python test/web/flasher_site_test.py
"""

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import full_install_image  # noqa: E402
import stage_flasher_site as site  # noqa: E402

failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


# ---- select_releases ----------------------------------------------------------------------------

def release(tag, day, prerelease=False, draft=False, assets=("firmware.bin",)):
    return {
        "tag_name": tag,
        "prerelease": prerelease,
        "draft": draft,
        "published_at": None if draft else "2026-11-%02dT12:00:00Z" % day,
        "assets": [{"name": a} for a in assets],
    }


listed = [
    release("2.55RC", 20, prerelease=True),
    release("2.54RC", 19, prerelease=True),
    release("2.56RC", 21, prerelease=True),
    release("2.53", 18),
    release("2.53b", 17, assets=("notes.txt",)),  # no board image
    release("2.53c", 17, draft=True),
    release("2.52", 16),
    release("2.51", 15, assets=("full-install-x3x4.bin",)),
    release("2.41RC", 14, prerelease=True),  # older than the newest full release
    release("2.50", 13),
    release("2.40", 12),
    release("2.37", 11),
]
tags = [r["tag_name"] for r in site.select_releases(listed)]
check(tags == ["2.56RC", "2.55RC", "2.54RC", "2.53", "2.52", "2.51", "2.50", "2.40"],
      "select: newer pre-releases first, newest first, then the last five full releases: %s" % tags)
tags = [r["tag_name"] for r in site.select_releases(listed, full=5, pre=2)]
check(tags[:2] == ["2.56RC", "2.55RC"], "select: at most `pre` pre-releases, the newest ones")
tags = [r["tag_name"] for r in site.select_releases([release("2.54RC", 19, prerelease=True)])]
check(tags == ["2.54RC"], "select: without a full release every pre-release counts as newer")

# ---- stage_release ------------------------------------------------------------------------------

PART_MD5_MAGIC = b"\xeb\xeb" + b"\xff" * 14


def table_blob(rows):
    blob = b""
    for label, ptype, subtype, offset, size, flags in rows:
        blob += b"\xaa\x50" + struct.pack("<BBII", ptype, subtype, offset, size)
        blob += label.encode("ascii").ljust(16, b"\0") + struct.pack("<I", flags)
    blob += PART_MD5_MAGIC + hashlib.md5(blob).digest()
    return blob.ljust(site.TABLE[1] - site.TABLE[0], b"\xff")


def full_image(rows, otadata_byte):
    data = bytearray(b"\xff" * 0x20000)
    data[0] = 0xE9
    blob = table_blob(rows)
    data[site.TABLE[0]:site.TABLE[0] + len(blob)] = blob
    data[site.OTADATA[0]:site.OTADATA[1]] = bytes([otadata_byte]) * (site.OTADATA[1] - site.OTADATA[0])
    return bytes(data)


def write(folder, name, data):
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)


ours = full_install_image.parse_csv(os.path.join(ROOT, "partitions.csv"))
other = [("nvs", 1, 2, 0x9000, 0x5000, 0)]
csv_asked = []


def csv_path():
    csv_asked.append(True)
    return os.path.join(ROOT, "partitions.csv")


with tempfile.TemporaryDirectory() as tmp:
    out = os.path.join(tmp, "site")
    site.copy_page(out)
    check(os.path.isfile(os.path.join(out, "index.html")) and os.path.isfile(os.path.join(out, "lib.js")),
          "stage: the page's files are at the site's top")

    # A release with full images for two boards and an app for a third, and its web bundle.
    full = os.path.join(tmp, "full")
    os.makedirs(full)
    write(full, "firmware.bin", b"app-x3x4")
    write(full, "full-install-x3x4.bin", full_image(ours, 0x11))
    write(full, "firmware-x4pro.bin", b"app-x4pro")
    write(full, "full-install-x4pro.bin", full_image(ours, 0x11))
    write(full, "firmware-murphy.bin", b"app-murphy")
    write(full, "web-0123456789ab.bin", b"bundle")
    info = {"tag": "2.53", "name": "2.53 - Test", "url": "u", "prerelease": False, "published": "p"}
    entry = site.stage_release(full, out, info, csv_path)
    x3 = entry["boards"]["x3x4"]
    check(x3["app"]["file"] == "releases/2.53/firmware.bin"
          and x3["full"]["file"] == "releases/2.53/full-install-x3x4.bin",
          "stage: images go to the release's own folder, with paths from the site's top")
    check(x3["app"]["size"] == 8 and x3["app"]["sha256"] == hashlib.sha256(b"app-x3x4").hexdigest(),
          "stage: each image is listed with its size and SHA-256")
    check(entry["boards"]["murphy"]["full"] is None and entry["boards"]["picco"] == {"app": None, "full": None},
          "stage: a missing image is listed as none")
    check(entry["table"] == site.table_entries(ours) and not csv_asked,
          "stage: the table is the full images', and partitions.csv is not asked for")
    with open(os.path.join(out, "releases/2.53/boot_app0.bin"), "rb") as f:
        check(f.read() == b"\x11" * 0x2000 and entry["otadata"]["file"] == "releases/2.53/boot_app0.bin",
              "stage: otadata is the full images' otadata slice")
    check(entry["web"]["name"] == "web-0123456789ab.bin"
          and entry["web"]["file"] == "releases/2.53/web-0123456789ab.bin",
          "stage: the web bundle is listed under its name")
    check(entry["tag"] == "2.53" and entry["name"] == "2.53 - Test" and entry["prerelease"] is False,
          "stage: the release's tag, name and kind come along")

    # An older release: app images only, so the table comes from its partitions.csv.
    old = os.path.join(tmp, "old")
    os.makedirs(old)
    write(old, "firmware.bin", b"old-app")
    info = {"tag": "2.41 RC/1", "name": "x", "url": "", "prerelease": True, "published": "p"}
    entry_old = site.stage_release(old, out, info, csv_path)
    check(entry_old["boards"]["x3x4"]["app"]["file"] == "releases/2.41_RC_1/firmware.bin",
          "stage: a tag with other characters gets a folder of safe ones")
    check(csv_asked and entry_old["table"] == site.table_entries(ours) and entry_old["otadata"] is None,
          "stage: without full images, the table is partitions.csv's and there is no otadata")
    check(entry_old["web"] is None, "stage: a release without a web bundle lists none")

    site.write_index(out, [entry, entry_old])
    with open(os.path.join(out, "releases.json"), encoding="utf-8") as f:
        index = json.load(f)
    check([r["tag"] for r in index["releases"]] == ["2.53", "2.41 RC/1"], "index: releases.json lists them in order")
    files = [img["file"] for r in index["releases"] for b in r["boards"].values() for img in b.values() if img]
    check(all(os.path.isfile(os.path.join(out, p)) for p in files), "index: every listed image is on the site")

    # Two full images with different tables: the release is not staged.
    mixed = os.path.join(tmp, "mixed")
    os.makedirs(mixed)
    write(mixed, "full-install-x3x4.bin", full_image(ours, 0x11))
    write(mixed, "full-install-x4pro.bin", full_image(other, 0x11))
    try:
        site.stage_release(mixed, out, dict(info, tag="mixed"), csv_path)
        check(False, "stage: full images with different tables stop the staging")
    except SystemExit as e:
        check("another partition table" in str(e), "stage: full images with different tables stop the staging")

# ---- --assets in GitHub Actions ----------------------------------------------------------------

SCRIPT = os.path.join(ROOT, "scripts", "stage_flasher_site.py")
with tempfile.TemporaryDirectory() as tmp:
    assets = os.path.join(tmp, "assets")
    os.makedirs(assets)
    write(assets, "firmware.bin", b"app")
    out = os.path.join(tmp, "site")
    args = [sys.executable, SCRIPT, "--assets", assets, "--out", out, "--tag", "2.53"]
    env = dict(os.environ, GITHUB_ACTIONS="true")
    run = subprocess.run(args, env=env, capture_output=True, text=True)
    check(run.returncode != 0 and run.stdout.startswith("::error::") and "Deploy Flasher" in run.stdout
          and not os.path.exists(out),
          "--assets in GitHub Actions: refused before staging, with an error naming Deploy Flasher")
    env = {k: v for k, v in os.environ.items() if k != "GITHUB_ACTIONS"}
    run = subprocess.run(args, env=env, capture_output=True, text=True)
    check(run.returncode == 0 and os.path.isfile(os.path.join(out, "releases.json")),
          "--assets on a desktop: stages the release")

if failures:
    print("%d check(s) failed" % len(failures))
    sys.exit(1)
print("all checks passed")
