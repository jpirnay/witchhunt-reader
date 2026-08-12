#!/usr/bin/env python3
"""KOSync server conformance harness for the CrossPoint/witchhunt KOReader sync client.

Why this exists
---------------
lib/KOReaderSync/KOReaderSyncClient.cpp is built on esp_http_client, so it cannot be
compiled by the host test toolchain in test/ -- there is no way to unit-test it natively.
What that client really encodes is a set of *claims about how real KOSync servers behave*
("a successful PUT answers 200 or 202", "no stored progress means 404"). Those claims are
what broke against BookLore/grimmory, a Spring service that answers with the idiomatic
codes (crosspoint-reader issue #2876).

This harness talks to a live KOSync server with exactly the wire format the firmware uses,
then runs each response through a Python mirror of the firmware's status -> Error mappings.
A failure means the firmware would mis-handle that server.

Keep the mappings below in sync with KOReaderSyncClient.cpp. They are deliberately a
transcription, not an abstraction.

Account safety
--------------
Creating accounts on a public sync server is the one thing here that can get you banned,
so nothing in the default run creates or attempts to create an account:

  * default suite          -- auth + progress only. Writes progress under a fixed, namespaced
                              test document id that no real book can collide with.
  * --check-register-existing -- sends ONE POST /users/create for the account you already own,
                              to verify the server's "already exists" signal. Creates nothing,
                              but it is still a create *attempt*, so it is opt-in.
  * --allow-create-user    -- actually creates an account. Requires an explicitly supplied
                              username/password (never auto-generated, so re-runs reuse the
                              same account instead of multiplying them), prints a warning, and
                              asks for typed confirmation unless --yes is passed. Hard-capped
                              at one create request per invocation.

Every run is additionally bounded by a total request budget and a delay between requests.

Usage
-----
  python scripts/kosync_conformance.py --server https://sync.koreader.rocks --user NAME --password PASS
  python scripts/kosync_conformance.py --config scripts/kosync_conformance.local.json
  python scripts/kosync_conformance.py --dry-run

Credentials may also come from the environment (KOSYNC_URL, KOSYNC_USER, KOSYNC_PASSWORD) or
from a JSON config file with the keys {"server", "user", "password"}. Anything matching
*.local* is gitignored, so scripts/kosync_conformance.local.json is a safe place to keep them.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request

# Mirrors KOReaderSyncClient.cpp: DEVICE_NAME / DEVICE_ID and the Accept header.
DEVICE_NAME = "CrossPoint"
DEVICE_ID = "crosspoint-reader"
ACCEPT_HEADER = "application/vnd.koreader.v1+json"

# Fixed, namespaced document id for progress tests. Derived from a constant string rather
# than from a real file so a test run can never overwrite progress for a book you own.
TEST_DOCUMENT = hashlib.md5(b"crosspoint-reader kosync conformance harness").hexdigest()

DEFAULT_REQUEST_BUDGET = 20
DEFAULT_REQUEST_DELAY_S = 0.5


# --------------------------------------------------------------------------------------
# Transcription of the firmware's status -> Error mappings.
# Keep in step with lib/KOReaderSync/KOReaderSyncClient.cpp.
# --------------------------------------------------------------------------------------


def _skip_bom_and_whitespace(body: str) -> str:
    return body.lstrip("﻿").lstrip()


def map_register(status: int, body: str) -> str:
    if 300 <= status < 400:
        return "REDIRECT_ERROR"
    if status == 200:
        return "USER_EXISTS"  # some servers answer 200 when the user already exists
    if 200 <= status < 300:
        return "OK"
    if status == 402:
        return "USER_EXISTS" if "already" in body.lower() else "REGISTRATION_DISABLED"
    if status == 409:
        return "USER_EXISTS"
    return "SERVER_ERROR"


def map_authenticate(status: int, body: str) -> str:
    if 300 <= status < 400:
        return "REDIRECT_ERROR"
    if 200 <= status < 300:
        # 204/205 are legitimately bodiless; every other 2xx must carry a JSON object.
        if status not in (204, 205) and _skip_bom_and_whitespace(body)[:1] != "{":
            return "INVALID_RESPONSE"
        return "OK"
    if status == 401:
        return "AUTH_FAILED"
    return "SERVER_ERROR"


def map_get_progress(status: int, body: str) -> str:
    if 300 <= status < 400:
        return "REDIRECT_ERROR"
    if 200 <= status < 300:
        if not body:
            return "NOT_FOUND"
        try:
            doc = json.loads(body)
        except ValueError:
            return "JSON_ERROR"
        if not isinstance(doc, dict) or doc.get("progress") is None:
            return "NOT_FOUND"
        return "OK"
    if status == 401:
        return "AUTH_FAILED"
    if status == 404:
        return "NOT_FOUND"
    return "SERVER_ERROR"


def map_update_progress(status: int, body: str) -> str:
    if 300 <= status < 400:
        return "REDIRECT_ERROR"
    if 200 <= status < 300:
        first = _skip_bom_and_whitespace(body)[:1]
        if first and first != "{":
            return "INVALID_RESPONSE"
        return "OK"
    if status == 401:
        return "AUTH_FAILED"
    return "SERVER_ERROR"


# --------------------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------------------


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """The firmware reports any 3xx as REDIRECT_ERROR, so observe them instead of following."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Client:
    def __init__(self, server: str, user: str, md5_password: str, args) -> None:
        self.server = server.rstrip("/")
        self.user = user
        self.md5_password = md5_password
        self.dry_run = args.dry_run
        self.delay = args.request_delay
        self.budget = args.request_budget
        self.spent = 0
        self.opener = urllib.request.build_opener(_NoRedirect)
        self._last = 0.0

    def request(self, method: str, path: str, body: dict | None = None, auth: bool = True,
                auth_key: str | None = None) -> tuple[int, str]:
        url = f"{self.server}{path}"
        if self.dry_run:
            print(f"  [dry-run] {method} {url}")
            return (0, "")
        if self.spent >= self.budget:
            raise RuntimeError(f"request budget of {self.budget} exhausted; raise --request-budget if intended")
        self.spent += 1

        elapsed = time.monotonic() - self._last
        if elapsed < self.delay:
            time.sleep(self.delay - elapsed)

        payload = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=payload, method=method)
        req.add_header("Accept", ACCEPT_HEADER)
        if payload is not None:
            req.add_header("Content-Type", "application/json")
        if auth:
            req.add_header("x-auth-user", self.user)
            req.add_header("x-auth-key", auth_key if auth_key is not None else self.md5_password)

        try:
            with self.opener.open(req, timeout=20) as resp:
                text = resp.read().decode("utf-8", "replace")
                status = resp.status
        except urllib.error.HTTPError as e:
            text = e.read().decode("utf-8", "replace")
            status = e.code
        except urllib.error.URLError as e:
            raise RuntimeError(f"{method} {url} failed at the transport layer: {e.reason}") from e
        finally:
            self._last = time.monotonic()
        return (status, text)


# --------------------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------------------

RESULTS: list[tuple[str, bool, str]] = []


def record(name: str, ok: bool, detail: str) -> None:
    RESULTS.append((name, ok, detail))
    mark = "PASS" if ok else "FAIL"
    print(f"  [{mark}] {name}: {detail}")


def check_auth_valid(c: Client) -> None:
    status, body = c.request("GET", "/users/auth")
    if c.dry_run:
        return
    mapped = map_authenticate(status, body)
    record("auth/valid-credentials", mapped == "OK", f"HTTP {status} -> {mapped} (want OK)")


def check_auth_invalid(c: Client) -> None:
    status, body = c.request("GET", "/users/auth", auth_key="0" * 32)
    if c.dry_run:
        return
    mapped = map_authenticate(status, body)
    record("auth/rejects-bad-key", mapped == "AUTH_FAILED", f"HTTP {status} -> {mapped} (want AUTH_FAILED)")


def check_progress_unknown(c: Client) -> None:
    """A never-written document must land on the graceful no-progress path, whatever shape the
    server uses to say so (404, 204, or 2xx with an empty body / empty object)."""
    unknown = hashlib.md5(b"crosspoint-reader conformance never-written document").hexdigest()
    status, body = c.request("GET", f"/syncs/progress/{unknown}")
    if c.dry_run:
        return
    mapped = map_get_progress(status, body)
    record("progress/unknown-document", mapped == "NOT_FOUND", f"HTTP {status} -> {mapped} (want NOT_FOUND)")


def check_progress_put(c: Client, xpath: str, percentage: float) -> None:
    """The regression that motivated PR #2945: Spring services answer a PUT with 201/204."""
    body = {
        "document": TEST_DOCUMENT,
        "progress": xpath,
        "percentage": percentage,
        "device": DEVICE_NAME,
        "device_id": DEVICE_ID,
    }
    status, text = c.request("PUT", "/syncs/progress", body=body)
    if c.dry_run:
        return
    mapped = map_update_progress(status, text)
    record("progress/upload-accepted", mapped == "OK", f"HTTP {status} -> {mapped} (want OK)")


def check_progress_roundtrip(c: Client, xpath: str, percentage: float) -> None:
    status, body = c.request("GET", f"/syncs/progress/{TEST_DOCUMENT}")
    if c.dry_run:
        return
    mapped = map_get_progress(status, body)
    if mapped != "OK":
        record("progress/round-trip", False, f"HTTP {status} -> {mapped} (want OK)")
        return
    doc = json.loads(body)
    got_xpath = doc.get("progress")
    got_pct = doc.get("percentage")
    ok = got_xpath == xpath and got_pct is not None and abs(float(got_pct) - percentage) < 1e-4
    record("progress/round-trip", ok, f"stored progress={got_xpath!r} percentage={got_pct!r}")


def check_register_existing(c: Client) -> None:
    """Verify the server's 'already exists' signal WITHOUT creating anything: the account in the
    config already exists, so this create attempt must be refused."""
    body = {"username": c.user, "password": c.md5_password}
    status, text = c.request("POST", "/users/create", body=body, auth=False)
    if c.dry_run:
        return
    mapped = map_register(status, text)
    record("register/existing-user-refused", mapped == "USER_EXISTS", f"HTTP {status} -> {mapped} (want USER_EXISTS)")


def check_register_create(c: Client, username: str, password: str) -> None:
    body = {"username": username, "password": hashlib.md5(password.encode()).hexdigest()}
    status, text = c.request("POST", "/users/create", body=body, auth=False)
    if c.dry_run:
        return
    mapped = map_register(status, text)
    # USER_EXISTS is a pass too: it means a previous run already made this account, which is
    # exactly the reuse behaviour the fixed username is there to produce.
    ok = mapped in ("OK", "USER_EXISTS")
    record("register/create-account", ok, f"HTTP {status} -> {mapped} (want OK, or USER_EXISTS on re-run)")


# --------------------------------------------------------------------------------------
# Offline self-test: pins the mapping table above without a network or an account.
# This is the part that can run in CI. Every row is a status/body shape a real KOSync
# implementation is known to produce; the expectations encode the post-#2945 behaviour.
# --------------------------------------------------------------------------------------

SELF_TEST_CASES: list[tuple[str, int, str, str]] = [
    # authenticate
    ("authenticate", 200, '{"username":"u"}', "OK"),
    ("authenticate", 200, "", "INVALID_RESPONSE"),           # bodiless 200 stays suspicious
    ("authenticate", 200, "<html>portal</html>", "INVALID_RESPONSE"),
    ("authenticate", 204, "", "OK"),                          # Spring-style, legitimately bodiless
    ("authenticate", 201, '{"ok":1}', "OK"),
    ("authenticate", 302, "", "REDIRECT_ERROR"),
    ("authenticate", 401, "", "AUTH_FAILED"),
    ("authenticate", 500, "", "SERVER_ERROR"),
    # getProgress
    ("get_progress", 200, '{"progress":"/body/x","percentage":0.5}', "OK"),
    ("get_progress", 200, "{}", "NOT_FOUND"),                 # reference server's no-progress shape
    ("get_progress", 200, "", "NOT_FOUND"),
    ("get_progress", 204, "", "NOT_FOUND"),                   # Spring-style no-progress shape
    ("get_progress", 200, "not json", "JSON_ERROR"),
    ("get_progress", 404, "", "NOT_FOUND"),
    ("get_progress", 401, "", "AUTH_FAILED"),
    ("get_progress", 500, "", "SERVER_ERROR"),
    # updateProgress -- the #2876 regression
    ("update_progress", 200, '{"document":"d"}', "OK"),
    ("update_progress", 202, "", "OK"),
    ("update_progress", 201, "", "OK"),
    ("update_progress", 204, "", "OK"),
    ("update_progress", 200, "<html>portal</html>", "INVALID_RESPONSE"),
    ("update_progress", 401, "", "AUTH_FAILED"),
    ("update_progress", 500, "", "SERVER_ERROR"),
    # registerUser
    ("register", 201, "", "OK"),
    ("register", 204, "", "OK"),
    ("register", 200, "", "USER_EXISTS"),                     # servers that answer 200 for existing
    ("register", 402, '{"message":"Username is already registered."}', "USER_EXISTS"),
    ("register", 402, '{"message":"Registration is disabled."}', "REGISTRATION_DISABLED"),
    ("register", 409, "", "USER_EXISTS"),
    ("register", 500, "", "SERVER_ERROR"),
]

_MAPPERS = {
    "authenticate": map_authenticate,
    "get_progress": map_get_progress,
    "update_progress": map_update_progress,
    "register": map_register,
}


def run_self_test() -> int:
    failures = 0
    for mapper_name, status, body, expected in SELF_TEST_CASES:
        got = _MAPPERS[mapper_name](status, body)
        if got != expected:
            failures += 1
            print(f"  [FAIL] {mapper_name}({status}, {body!r}) -> {got}, want {expected}")
    total = len(SELF_TEST_CASES)
    print(f"{total - failures}/{total} mapping cases passed")
    if failures:
        print("The Python mirror and lib/KOReaderSync/KOReaderSyncClient.cpp have drifted apart.")
    return 1 if failures else 0


# --------------------------------------------------------------------------------------


def load_settings(args) -> tuple[str, str, str]:
    server, user, password = args.server, args.user, args.password
    if args.config:
        with open(args.config, encoding="utf-8") as fh:
            cfg = json.load(fh)
        server = server or cfg.get("server")
        user = user or cfg.get("user")
        password = password or cfg.get("password")
    server = server or os.environ.get("KOSYNC_URL")
    user = user or os.environ.get("KOSYNC_USER")
    password = password or os.environ.get("KOSYNC_PASSWORD")
    return (server, user, password)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--server", help="Base URL, e.g. https://sync.koreader.rocks")
    p.add_argument("--user")
    p.add_argument("--password", help="Plain password; the MD5 is computed here, as the firmware does")
    p.add_argument("--config", help="JSON file with {server, user, password}")
    p.add_argument("--dry-run", action="store_true", help="Print the request plan and send nothing")
    p.add_argument("--self-test", action="store_true",
                   help="Run the offline mapping table only. No network, no account, CI-safe.")
    p.add_argument("--request-budget", type=int, default=DEFAULT_REQUEST_BUDGET)
    p.add_argument("--request-delay", type=float, default=DEFAULT_REQUEST_DELAY_S,
                   help="Minimum seconds between requests, so a public server is never hammered")
    p.add_argument("--check-register-existing", action="store_true",
                   help="Send one create attempt for the account you already own to check the "
                        "'already exists' signal. Creates nothing, but is still a create attempt.")
    p.add_argument("--allow-create-user", action="store_true",
                   help="Actually create an account. Requires --new-username and --new-password.")
    p.add_argument("--new-username", help="Fixed username for --allow-create-user. Reuse it across runs.")
    p.add_argument("--new-password", help="Password for --allow-create-user")
    p.add_argument("--yes", action="store_true", help="Skip the typed confirmation for --allow-create-user")
    args = p.parse_args()

    if args.self_test:
        return run_self_test()

    # Validate the account-creating path before anything is printed or sent.
    if args.allow_create_user and not (args.new_username and args.new_password):
        p.error("--allow-create-user requires --new-username and --new-password. They are never "
                "auto-generated, so that repeated runs reuse one account instead of creating many.")

    server, user, password = load_settings(args)
    if args.dry_run:
        server = server or "https://sync.example.invalid"
        user = user or "<user>"
        password = password or "<password>"
    if not (server and user and password):
        p.error("server, user and password are required (flags, --config, or KOSYNC_* env vars)")

    md5_password = hashlib.md5(password.encode()).hexdigest()
    client = Client(server, user, md5_password, args)

    print(f"KOSync conformance: {server}")
    print(f"  user            : {user}")
    print(f"  test document   : {TEST_DOCUMENT}")
    print(f"  request budget  : {args.request_budget} (delay {args.request_delay}s)")
    if args.dry_run:
        print("  MODE            : dry run, no requests will be sent")
    print()

    if args.allow_create_user:
        print("!! --allow-create-user will create a real account on this server.")
        print(f"!! server={server} username={args.new_username}")
        print("!! Reuse the same username on every run. Creating many accounts can get you banned.")
        if not args.yes and not args.dry_run:
            if input("!! Type 'create' to continue: ").strip() != "create":
                print("Aborted.")
                return 2
        print()

    xpath = "/body/DocFragment[1]/body/p[1]/text().0"
    percentage = 0.4242

    try:
        print("auth")
        check_auth_valid(client)
        check_auth_invalid(client)

        print("progress")
        check_progress_unknown(client)
        check_progress_put(client, xpath, percentage)
        check_progress_roundtrip(client, xpath, percentage)

        if args.check_register_existing:
            print("register")
            check_register_existing(client)

        if args.allow_create_user:
            print("register (account creation)")
            check_register_create(client, args.new_username, args.new_password)
    except RuntimeError as e:
        print(f"\nAborted: {e}", file=sys.stderr)
        return 2

    if args.dry_run:
        print("\nDry run complete; no requests were sent.")
        return 0

    failed = [name for name, ok, _ in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} checks passed ({client.spent} requests sent)")
    if failed:
        print("Failed: " + ", ".join(failed))
        print("A failure here means KOReaderSyncClient.cpp would mis-handle this server.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
