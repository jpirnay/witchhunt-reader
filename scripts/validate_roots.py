#!/usr/bin/env python3
"""
Validate that a list of HTTPS URLs verifies against the curated CrossPoint root
set (lib/SecureNet/include/CrossPointRoots.pem) — the same anchors the firmware
loads into wolfSSL. Catches "a host chains to a root we don't ship" BEFORE it
fails on-device.

Usage:
  python scripts/validate_roots.py                 # check built-in first-party hosts
  python scripts/validate_roots.py --urls hosts.txt  # + one URL/host per line
  python scripts/validate_roots.py --auto-append     # stage missing roots (review required)

Exit code is non-zero if any host FAILs verification (SKIPPED does not fail).

Requires the `openssl` CLI on PATH.
"""

import argparse
import os
import re
import subprocess
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOTS_PEM = os.path.join(PROJECT_DIR, "lib", "SecureNet", "include", "CrossPointRoots.pem")
STAGING_PEM = os.path.join(PROJECT_DIR, "scripts", "discovered-roots.pem")

# First-party hosts the firmware contacts. Keep in sync with the trust map.
BUILTIN_HOSTS = [
    "github.com:443",
    "api.github.com:443",
    "raw.githubusercontent.com:443",
    "objects.githubusercontent.com:443",
    "sync.koreader.rocks:443",
    "api.open-meteo.com:443",
    "geocoding-api.open-meteo.com:443",
    "api.ipify.org:443",
    "timeapi.io:443",
    # Public OPDS catalogs commonly used with this reader (Let's Encrypt-served).
    "standardebooks.org:443",
    "catalog.feedbooks.com:443",
    # Project Gutenberg's OPDS (SSL.com-served); m. redirects to www., where the books are too.
    "m.gutenberg.org:443",
    "www.gutenberg.org:443",
]


def split_host_port(entry):
    entry = entry.strip()
    if entry.startswith("http://"):
        return None, None  # plain http: no TLS to check
    if entry.startswith("https://"):
        entry = entry[len("https://"):]
        entry = entry.split("/", 1)[0]
    if ":" in entry:
        host, port = entry.rsplit(":", 1)
        return host, port
    return entry, "443"


def run_openssl(args, stdin_data=None, timeout=20):
    try:
        return subprocess.run(
            ["openssl"] + args,
            input=stdin_data,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except FileNotFoundError:
        sys.exit("error: openssl CLI not found on PATH")
    except subprocess.TimeoutExpired:
        return None


def check_host(host, port):
    """Return (status, detail). status in {PASS, FAIL, SKIPPED}."""
    res = run_openssl(
        ["s_client", "-connect", "{}:{}".format(host, port), "-servername", host,
         "-CAfile", ROOTS_PEM, "-verify_return_error"],
        stdin_data="",
    )
    if res is None:
        return "SKIPPED", "connect/TLS timeout (no resolve or no TLS)"
    out = res.stdout + res.stderr
    if "Verify return code: 0 (ok)" in out:
        return "PASS", ""
    if "no peer certificate available" in out or "Connection refused" in out or "gethostbyname" in out:
        return "SKIPPED", "no TLS / does not resolve"
    m = re.search(r"Verify return code: (\d+ \([^)]+\))", out)
    return "FAIL", m.group(1) if m else "verification failed"


def fetch_missing_root(host, port):
    """Grab the deepest issuer of the served chain (best-effort) for staging."""
    res = run_openssl(
        ["s_client", "-connect", "{}:{}".format(host, port), "-servername", host, "-showcerts"],
        stdin_data="",
    )
    if res is None:
        return None
    pems = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", res.stdout, re.S)
    if not pems:
        return None
    top = pems[-1]  # deepest cert the server sent (often an intermediate, not the true root)
    info = run_openssl(["x509", "-noout", "-subject", "-enddate", "-fingerprint", "-sha256"], stdin_data=top)
    return top, (info.stdout if info else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--urls", help="file with one URL/host per line")
    ap.add_argument("--auto-append", action="store_true",
                    help="stage missing roots to scripts/discovered-roots.pem for MANUAL review")
    args = ap.parse_args()

    if not os.path.exists(ROOTS_PEM):
        sys.exit("error: {} not found".format(ROOTS_PEM))

    entries = list(BUILTIN_HOSTS)
    if args.urls:
        with open(args.urls, "r", encoding="utf-8") as f:
            entries += [ln for ln in (l.strip() for l in f) if ln and not ln.startswith("#")]

    any_fail = False
    staged = []
    print("Validating {} host(s) against {}\n".format(len(entries), os.path.relpath(ROOTS_PEM, PROJECT_DIR)))
    for entry in entries:
        host, port = split_host_port(entry)
        if host is None:
            print("  SKIPPED  {}  (plain http)".format(entry))
            continue
        status, detail = check_host(host, port)
        line = "  {:8} {}:{}".format(status, host, port)
        if detail:
            line += "  [{}]".format(detail)
        print(line)
        if status == "FAIL":
            any_fail = True
            if args.auto_append:
                got = fetch_missing_root(host, port)
                if got:
                    staged.append((host, got[0], got[1]))

    if staged:
        with open(STAGING_PEM, "a", encoding="utf-8") as f:
            for host, pem, info in staged:
                f.write("# staged from {} — REVIEW FINGERPRINT before merging into CrossPointRoots.pem\n".format(host))
                f.write("# {}\n".format(info.replace("\n", " ").strip()))
                f.write(pem + "\n")
        print("\n*** WARNING ***")
        print("Staged {} candidate root(s) to {}".format(len(staged), os.path.relpath(STAGING_PEM, PROJECT_DIR)))
        print("A root captured from a live probe can be MITM-injected. Verify each")
        print("fingerprint against an authoritative source (the CA's published root)")
        print("BEFORE copying it into CrossPointRoots.pem and regenerating the header.")

    print("\n{}".format("FAIL: some hosts are not covered by the curated roots" if any_fail
                        else "OK: all resolvable hosts verify against the curated roots"))
    sys.exit(1 if any_fail else 0)


if __name__ == "__main__":
    main()
