#!/usr/bin/env python3
"""
Stage the web flasher's GitHub Pages site: the page (flasher/), one release's images and a
manifest.json that tells the page what it has. release.yml's deploy-flasher job runs it; run it
locally to try the page with a build's images (serve the output folder on 127.0.0.1).

The images are served from the site itself because GitHub's release downloads send no
Access-Control-Allow-Origin header, so a page cannot fetch them.

Per board the manifest lists the full-install image and the app image (either may be missing:
releases before 2.51 have no full image, and the page then offers the app update only), the web
bundle, the otadata image the "update only" path writes with the app (boot_app0.bin, taken from
the full images, which all carry the same one), and the partition table the full images carry,
which is what the page calls "ours".

Usage:
  stage_flasher_site.py --assets <dir of *.bin> --out <site dir> --tag <tag> [--name <title>] [--url <release page>]
"""

import argparse
import datetime
import glob
import hashlib
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import full_install_image  # noqa: E402  (shared partition-table parser)

# Board ids as flasher/lib.js names them, and their release assets (src/network/FirmwareBoardTag.h).
BOARDS = {
    "x3x4": ("firmware.bin", "full-install-x3x4.bin"),
    "x4pro": ("firmware-x4pro.bin", "full-install-x4pro.bin"),
    "lilygo": ("firmware-lilygo.bin", "full-install-lilygo.bin"),
    "murphy": ("firmware-murphy.bin", "full-install-murphy.bin"),
    "picco": ("firmware-picco.bin", "full-install-picco.bin"),
}
OTADATA = (0xE000, 0x10000)
TABLE = (0x8000, 0x8C00)


def describe(path, published):
    with open(path, "rb") as f:
        data = f.read()
    return {"file": published, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--assets", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--name", default="")
    parser.add_argument("--url", default="")
    args = parser.parse_args()

    page_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "flasher")
    images = os.path.join(args.out, "images")
    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    os.makedirs(images)
    for name in sorted(os.listdir(page_dir)):
        src = os.path.join(page_dir, name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(args.out, name))

    manifest = {
        "tag": args.tag,
        "name": args.name or args.tag,
        "url": args.url,
        "staged": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "boards": {},
        "otadata": None,
        "table": None,
        "web": None,
    }
    otadata = None
    for board, (app_name, full_name) in BOARDS.items():
        entry = {"app": None, "full": None}
        app = os.path.join(args.assets, app_name)
        if os.path.isfile(app):
            shutil.copy2(app, os.path.join(images, app_name))
            entry["app"] = describe(app, "images/" + app_name)
        full = os.path.join(args.assets, full_name)
        if os.path.isfile(full):
            shutil.copy2(full, os.path.join(images, full_name))
            entry["full"] = describe(full, "images/" + full_name)
            with open(full, "rb") as f:
                data = f.read()
            slice_ = data[OTADATA[0]:OTADATA[1]]
            if otadata is not None and slice_ != otadata:
                sys.exit("stage_flasher_site: %s carries another otadata than the other full images" % full_name)
            otadata = slice_
            table = [
                {"label": e[0], "type": e[1], "subtype": e[2], "offset": e[3], "size": e[4], "flags": e[5]}
                for e in full_install_image.parse_table(data[TABLE[0]:TABLE[1]])
            ]
            if manifest["table"] is not None and table != manifest["table"]:
                sys.exit("stage_flasher_site: %s carries another partition table than the other full images" % full_name)
            manifest["table"] = table
        manifest["boards"][board] = entry
        print("%-7s app: %-22s full: %s" % (board, entry["app"] and app_name, entry["full"] and full_name))

    if manifest["table"] is None:
        # No full image to take it from (an older release): the table partitions.csv describes.
        csv_path = os.path.join(os.path.dirname(page_dir), "partitions.csv")
        manifest["table"] = [
            {"label": e[0], "type": e[1], "subtype": e[2], "offset": e[3], "size": e[4], "flags": e[5]}
            for e in full_install_image.parse_csv(csv_path)
        ]

    if otadata is not None:
        path = os.path.join(images, "boot_app0.bin")
        with open(path, "wb") as f:
            f.write(otadata)
        manifest["otadata"] = describe(path, "images/boot_app0.bin")

    bundles = sorted(glob.glob(os.path.join(args.assets, "web-*.bin")))
    if len(bundles) > 1:
        sys.exit("stage_flasher_site: more than one web bundle: %s" % ", ".join(bundles))
    if bundles:
        name = os.path.basename(bundles[0])
        shutil.copy2(bundles[0], os.path.join(images, name))
        manifest["web"] = dict(describe(bundles[0], "images/" + name), name=name)

    with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("staged %s in %s (table: %s, web: %s)" % (args.tag, args.out, "yes" if manifest["table"] else "no",
                                                    manifest["web"]["name"] if manifest["web"] else "none"))


if __name__ == "__main__":
    main()
