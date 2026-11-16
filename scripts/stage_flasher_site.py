#!/usr/bin/env python3
"""
Stage the web flasher's GitHub Pages site: the page (flasher/), the images of the releases it offers
and a releases.json that tells the page what it has. deploy_flasher.yml runs it with --github; run it
with --assets to try the page with one build's images (serve the output folder on 127.0.0.1).

The images are served from the site itself because GitHub's release downloads send no
Access-Control-Allow-Origin header, so a page cannot fetch them. A Pages deploy replaces the whole
site, so --github downloads every release it offers again.

The releases offered (select_releases): the last FULL_RELEASES full releases, to go back to when a
newer one misbehaves, and the pre-releases published after the newest of them (at most
PRE_RELEASES), to try one before its release. The page selects the newest full release and lists
pre-releases only when asked to.

Per release and board the entry lists the full-install image and the app image (either may be
missing: releases before 2.51 have no full image, and the page then offers the app update only), the
web bundle, the otadata image the "update only" path writes with the app (boot_app0.bin, taken from
the full images, which all carry the same one), and the partition table the full images carry,
which is what the page calls "ours". A release without full images gets the table of partitions.csv
as it was at its tag.

Usage:
  stage_flasher_site.py --out <site dir> --github <owner/repo>
  stage_flasher_site.py --out <site dir> --assets <dir of *.bin> --tag <tag> [--name <title>] [--url <release page>]
                        [--prerelease]
"""

import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import full_install_image  # noqa: E402  (shared partition-table parser)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Board ids as flasher/lib.js names them, and their release assets (src/network/FirmwareBoardTag.h).
BOARDS = {
    "x3x4": ("firmware.bin", "full-install-x3x4.bin"),
    "x4pro": ("firmware-x4pro.bin", "full-install-x4pro.bin"),
    "lilygo": ("firmware-lilygo.bin", "full-install-lilygo.bin"),
    "murphy": ("firmware-murphy.bin", "full-install-murphy.bin"),
    "picco": ("firmware-picco.bin", "full-install-picco.bin"),
}
BOARD_ASSETS = {name for pair in BOARDS.values() for name in pair}
OTADATA = (0xE000, 0x10000)
TABLE = (0x8000, 0x8C00)

FULL_RELEASES = 5
PRE_RELEASES = 3
# GitHub Pages refuses a site larger than 1 GB. A release with full images is about 60 MB.
SITE_LIMIT = 1_000_000_000


def describe(path, published):
    with open(path, "rb") as f:
        data = f.read()
    return {"file": published, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def table_entries(rows):
    return [
        {"label": e[0], "type": e[1], "subtype": e[2], "offset": e[3], "size": e[4], "flags": e[5]}
        for e in rows
    ]


def release_dir(tag):
    """The site folder of a release: its tag, with anything but letters, digits, '.', '-' and '_' replaced."""
    return "releases/" + re.sub(r"[^A-Za-z0-9._-]", "_", tag)


def has_board_image(release):
    return any(asset["name"] in BOARD_ASSETS for asset in release.get("assets", []))


def select_releases(releases, full=FULL_RELEASES, pre=PRE_RELEASES):
    """The releases the page offers, newest first, from GitHub's release list (the REST API's objects):
    the pre-releases published after the newest full release (at most `pre`), then the last `full`
    full releases. Drafts and releases without a board image are left out."""
    published = [r for r in releases if not r.get("draft") and r.get("published_at") and has_board_image(r)]
    published.sort(key=lambda r: r["published_at"], reverse=True)  # ISO 8601 UTC sorts as text
    fulls = [r for r in published if not r.get("prerelease")][:full]
    newest_full = fulls[0]["published_at"] if fulls else ""
    pres = [r for r in published if r.get("prerelease") and r["published_at"] > newest_full][:pre]
    return pres + fulls


def stage_release(assets, out, info, csv_path):
    """Copies one release's images from `assets` to its folder under `out` and returns its entry for
    releases.json. `info` holds tag, name, url, prerelease and published; `csv_path()` gives the
    partitions.csv of the release, asked for only when it has no full image to take the table from."""
    folder = release_dir(info["tag"])
    target = os.path.join(out, folder)
    os.makedirs(target)
    entry = dict(info, boards={}, otadata=None, table=None, web=None)
    otadata = None
    for board, (app_name, full_name) in BOARDS.items():
        images = {"app": None, "full": None}
        app = os.path.join(assets, app_name)
        if os.path.isfile(app):
            shutil.copy2(app, os.path.join(target, app_name))
            images["app"] = describe(app, folder + "/" + app_name)
        full = os.path.join(assets, full_name)
        if os.path.isfile(full):
            shutil.copy2(full, os.path.join(target, full_name))
            images["full"] = describe(full, folder + "/" + full_name)
            with open(full, "rb") as f:
                data = f.read()
            slice_ = data[OTADATA[0]:OTADATA[1]]
            if otadata is not None and slice_ != otadata:
                sys.exit("stage_flasher_site: %s: %s carries another otadata than the other full images"
                         % (info["tag"], full_name))
            otadata = slice_
            table = table_entries(full_install_image.parse_table(data[TABLE[0]:TABLE[1]]))
            if entry["table"] is not None and table != entry["table"]:
                sys.exit("stage_flasher_site: %s: %s carries another partition table than the other full images"
                         % (info["tag"], full_name))
            entry["table"] = table
        entry["boards"][board] = images
        print("%-12s %-7s app: %-22s full: %s" % (info["tag"], board, images["app"] and app_name,
                                                  images["full"] and full_name))

    if entry["table"] is None:
        entry["table"] = table_entries(full_install_image.parse_csv(csv_path()))

    if otadata is not None:
        path = os.path.join(target, "boot_app0.bin")
        with open(path, "wb") as f:
            f.write(otadata)
        entry["otadata"] = describe(path, folder + "/boot_app0.bin")

    bundles = sorted(glob.glob(os.path.join(assets, "web-*.bin")))
    if len(bundles) > 1:
        sys.exit("stage_flasher_site: %s: more than one web bundle: %s" % (info["tag"], ", ".join(bundles)))
    if bundles:
        name = os.path.basename(bundles[0])
        shutil.copy2(bundles[0], os.path.join(target, name))
        entry["web"] = dict(describe(bundles[0], folder + "/" + name), name=name)
    return entry


def copy_page(out):
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    page_dir = os.path.join(ROOT, "flasher")
    for name in sorted(os.listdir(page_dir)):
        src = os.path.join(page_dir, name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(out, name))


def write_index(out, entries):
    folders = [release_dir(e["tag"]) for e in entries]
    if len(set(folders)) != len(folders):
        sys.exit("stage_flasher_site: two tags share a site folder: %s" % ", ".join(folders))
    total = sum(os.path.getsize(os.path.join(d, f)) for d, _, files in os.walk(out) for f in files)
    if total > SITE_LIMIT:
        sys.exit("stage_flasher_site: the site is %d bytes, more than GitHub Pages takes; offer fewer releases"
                 % total)
    index = {
        "staged": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "releases": entries,
    }
    with open(os.path.join(out, "releases.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=2)
        f.write("\n")
    print("staged %d release(s) in %s, %.1f MB: %s" % (len(entries), out, total / 1e6,
                                                       ", ".join(e["tag"] + (" (pre)" if e["prerelease"] else "")
                                                                 for e in entries)))


def gh(*args):
    return subprocess.run(["gh", *args], check=True, stdout=subprocess.PIPE).stdout


def stage_github(repo, out):
    releases = json.loads(gh("api", "repos/%s/releases?per_page=100" % repo))
    chosen = select_releases(releases)
    if not chosen:
        sys.exit("stage_flasher_site: %s has no published release with a board image" % repo)
    entries = []
    with tempfile.TemporaryDirectory() as work:
        for i, release in enumerate(chosen):
            tag = release["tag_name"]
            assets = os.path.join(work, str(i))
            os.makedirs(assets)
            args = ["release", "download", tag, "--repo", repo, "--dir", assets]
            for pattern in ("firmware*.bin", "full-install-*.bin", "web-*.bin"):
                args += ["--pattern", pattern]
            gh(*args)

            def csv_path(tag=tag, assets=assets):
                path = os.path.join(assets, "partitions.csv")
                with open(path, "wb") as f:
                    f.write(gh("api", "-H", "Accept: application/vnd.github.raw+json",
                               "repos/%s/contents/partitions.csv?ref=%s" % (repo, urllib.parse.quote(tag, safe=""))))
                return path

            info = {
                "tag": tag,
                "name": release.get("name") or tag,
                "url": release.get("html_url", ""),
                "prerelease": bool(release.get("prerelease")),
                "published": release["published_at"],
            }
            entries.append(stage_release(assets, out, info, csv_path))
            shutil.rmtree(assets)
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--github", metavar="OWNER/REPO", help="stage the releases this repository offers")
    source.add_argument("--assets", help="stage one release from this folder of *.bin")
    parser.add_argument("--tag", help="with --assets: the release's tag")
    parser.add_argument("--name", default="")
    parser.add_argument("--url", default="")
    parser.add_argument("--prerelease", action="store_true")
    args = parser.parse_args()
    if args.assets and not args.tag:
        parser.error("--assets needs --tag")

    copy_page(args.out)
    if args.github:
        entries = stage_github(args.github, args.out)
    else:
        info = {
            "tag": args.tag,
            "name": args.name or args.tag,
            "url": args.url,
            "prerelease": args.prerelease,
            "published": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        entries = [stage_release(args.assets, args.out, info, lambda: os.path.join(ROOT, "partitions.csv"))]
    write_index(args.out, entries)


if __name__ == "__main__":
    main()
