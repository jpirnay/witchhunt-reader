#!/usr/bin/env python3
"""KOSync conformance checks for the CrossPoint/witchhunt KOReader sync client.

lib/KOReaderSync/KOReaderSyncClient.cpp is built on esp_http_client, so the host CMake
toolchain in test/ cannot compile it and there is no native unit test for any of it. What
that client really encodes is a set of claims about how KOSync servers behave -- "a
successful PUT answers 200 or 202", "no stored progress means 404" -- and those claims are
what broke against Spring-based implementations (crosspoint-reader issue #2876).

This runs entirely offline. scripts/kosync_mock_server.py impersonates each server dialect
on localhost; every response goes through a Python mirror of the client's four
status -> Error mappings, and each dialect declares the Error it must produce.

  python scripts/kosync_conformance.py              # every dialect against the mock
  python scripts/kosync_conformance.py --self-test  # mapping table only, no sockets
  python scripts/kosync_conformance.py --dialect spring --verbose

No credentials, no accounts, no third-party server is contacted.

An important limitation: the mappings below are a hand transcription of the C++, so a green
run proves the intended table is self-consistent, not that KOReaderSyncClient.cpp matches
it. Change both together. To exercise the actual C++ client, run the mock with
--host 0.0.0.0 and point a device's Sync Server URL at it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import urllib.error
import urllib.request

from kosync_mock_server import DIALECTS, serve

# Mirrors KOReaderSyncClient.cpp: DEVICE_NAME / DEVICE_ID and the Accept header.
DEVICE_NAME = "CrossPoint"
DEVICE_ID = "crosspoint-reader"
ACCEPT_HEADER = "application/vnd.koreader.v1+json"

TEST_USER = "crosspoint"
TEST_PASSWORD = "conformance"
TEST_KEY = hashlib.md5(TEST_PASSWORD.encode()).hexdigest()
TEST_DOCUMENT = hashlib.md5(b"crosspoint-reader kosync conformance harness").hexdigest()
UNKNOWN_DOCUMENT = hashlib.md5(b"crosspoint-reader conformance never-written document").hexdigest()


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


_MAPPERS = {
    "authenticate": map_authenticate,
    "get_progress": map_get_progress,
    "update_progress": map_update_progress,
    "register": map_register,
}


# --------------------------------------------------------------------------------------
# Offline mapping table. No sockets: pins the intended behaviour on its own.
# --------------------------------------------------------------------------------------

SELF_TEST_CASES: list[tuple[str, int, str, str]] = [
    ("authenticate", 200, '{"username":"u"}', "OK"),
    ("authenticate", 200, "", "INVALID_RESPONSE"),            # bodiless 200 stays suspicious
    ("authenticate", 200, "<html>portal</html>", "INVALID_RESPONSE"),
    ("authenticate", 204, "", "OK"),                          # Spring-style, legitimately bodiless
    ("authenticate", 201, '{"ok":1}', "OK"),
    ("authenticate", 302, "", "REDIRECT_ERROR"),
    ("authenticate", 401, "", "AUTH_FAILED"),
    ("authenticate", 500, "", "SERVER_ERROR"),
    ("get_progress", 200, '{"progress":"/body/x","percentage":0.5}', "OK"),
    ("get_progress", 200, "{}", "NOT_FOUND"),                 # reference server's no-progress shape
    ("get_progress", 200, "", "NOT_FOUND"),
    ("get_progress", 204, "", "NOT_FOUND"),                   # Spring-style no-progress shape
    ("get_progress", 200, "not json", "JSON_ERROR"),
    ("get_progress", 404, "", "NOT_FOUND"),
    ("get_progress", 401, "", "AUTH_FAILED"),
    ("get_progress", 500, "", "SERVER_ERROR"),
    ("update_progress", 200, '{"document":"d"}', "OK"),
    ("update_progress", 202, "", "OK"),
    ("update_progress", 201, "", "OK"),                       # the #2876 regression
    ("update_progress", 204, "", "OK"),                       # the #2876 regression
    ("update_progress", 200, "<html>portal</html>", "INVALID_RESPONSE"),
    ("update_progress", 401, "", "AUTH_FAILED"),
    ("update_progress", 500, "", "SERVER_ERROR"),
    ("register", 201, "", "OK"),
    ("register", 204, "", "OK"),
    ("register", 200, "", "USER_EXISTS"),                     # servers that answer 200 for existing
    ("register", 402, '{"message":"Username is already registered."}', "USER_EXISTS"),
    ("register", 402, '{"message":"Registration is disabled."}', "REGISTRATION_DISABLED"),
    ("register", 409, "", "USER_EXISTS"),
    ("register", 500, "", "SERVER_ERROR"),
]


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
# What each dialect must produce. This is the actual contract under test: a Spring server
# has to come out as OK everywhere the reference server does.
# --------------------------------------------------------------------------------------

EXPECTATIONS: dict[str, dict[str, str]] = {
    "reference": {
        "auth/valid-credentials": "OK",
        "auth/rejects-bad-key": "AUTH_FAILED",
        "progress/unknown-document": "NOT_FOUND",
        "progress/upload-accepted": "OK",
        "progress/round-trip": "OK",
        "register/new-account": "OK",
        "register/existing-account": "USER_EXISTS",
    },
    "spring": {
        "auth/valid-credentials": "OK",
        "auth/rejects-bad-key": "AUTH_FAILED",
        "progress/unknown-document": "NOT_FOUND",
        "progress/upload-accepted": "OK",
        "progress/round-trip": "OK",
        "register/new-account": "OK",
        "register/existing-account": "USER_EXISTS",
    },
    "bodiless": {
        "auth/valid-credentials": "OK",
        "auth/rejects-bad-key": "AUTH_FAILED",
        "progress/unknown-document": "NOT_FOUND",
        "progress/upload-accepted": "OK",
        "progress/round-trip": "OK",
        "register/new-account": "OK",
        "register/existing-account": "USER_EXISTS",
    },
    "registration_disabled": {
        "auth/valid-credentials": "OK",
        "auth/rejects-bad-key": "AUTH_FAILED",
        "progress/unknown-document": "NOT_FOUND",
        "progress/upload-accepted": "OK",
        "progress/round-trip": "OK",
        "register/new-account": "REGISTRATION_DISABLED",
        "register/existing-account": "REGISTRATION_DISABLED",
    },
    "portal": {
        "auth/valid-credentials": "INVALID_RESPONSE",
        "auth/rejects-bad-key": "INVALID_RESPONSE",  # portal answers before auth is considered
        "progress/unknown-document": "JSON_ERROR",
        "progress/upload-accepted": "INVALID_RESPONSE",
        "progress/round-trip": "JSON_ERROR",
        "register/new-account": "INVALID_RESPONSE",
        "register/existing-account": "INVALID_RESPONSE",
    },
    "redirect": {
        "auth/valid-credentials": "REDIRECT_ERROR",
        "auth/rejects-bad-key": "REDIRECT_ERROR",
        "progress/unknown-document": "REDIRECT_ERROR",
        "progress/upload-accepted": "REDIRECT_ERROR",
        "progress/round-trip": "REDIRECT_ERROR",
        "register/new-account": "REDIRECT_ERROR",
        "register/existing-account": "REDIRECT_ERROR",
    },
}

# The portal dialect answers HTML with 200 for POST /users/create too, which the register
# mapping reads as USER_EXISTS (status 200) rather than INVALID_RESPONSE -- registerUser has
# no body-shape guard, because it needs the body to tell 402 cases apart. Record the real
# behaviour rather than an aspiration.
EXPECTATIONS["portal"]["register/new-account"] = "USER_EXISTS"
EXPECTATIONS["portal"]["register/existing-account"] = "USER_EXISTS"


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """The firmware reports any 3xx as REDIRECT_ERROR, so observe them instead of following."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Client:
    def __init__(self, base: str) -> None:
        self.base = base.rstrip("/")
        self.opener = urllib.request.build_opener(_NoRedirect)

    def request(self, method: str, path: str, body: dict | None = None, auth: bool = True,
                user: str = TEST_USER, key: str = TEST_KEY) -> tuple[int, str]:
        payload = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"{self.base}{path}", data=payload, method=method)
        req.add_header("Accept", ACCEPT_HEADER)
        if payload is not None:
            req.add_header("Content-Type", "application/json")
        if auth:
            req.add_header("x-auth-user", user)
            req.add_header("x-auth-key", key)
        try:
            with self.opener.open(req, timeout=10) as resp:
                return (resp.status, resp.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as e:
            return (e.code, e.read().decode("utf-8", "replace"))
        except urllib.error.URLError as e:
            raise RuntimeError(f"{method} {path} failed at the transport layer: {e.reason}") from e


def run_dialect(dialect: str, verbose: bool) -> list[tuple[str, bool, str]]:
    httpd, _state = serve(dialect, "127.0.0.1", 0, TEST_USER, TEST_KEY, verbose=verbose)
    port = httpd.server_address[1]
    c = Client(f"http://127.0.0.1:{port}")
    want = EXPECTATIONS[dialect]
    results: list[tuple[str, bool, str]] = []

    def check(name: str, mapper: str, status: int, body: str, extra: str = "") -> None:
        got = _MAPPERS[mapper](status, body)
        expected = want[name]
        detail = f"HTTP {status} -> {got} (want {expected})"
        results.append((name, got == expected, detail + (f" {extra}" if extra else "")))

    xpath = "/body/DocFragment[1]/body/p[1]/text().0"
    percentage = 0.4242

    try:
        status, body = c.request("GET", "/users/auth")
        check("auth/valid-credentials", "authenticate", status, body)

        status, body = c.request("GET", "/users/auth", key="0" * 32)
        check("auth/rejects-bad-key", "authenticate", status, body)

        status, body = c.request("GET", f"/syncs/progress/{UNKNOWN_DOCUMENT}")
        check("progress/unknown-document", "get_progress", status, body)

        status, body = c.request("PUT", "/syncs/progress", body={
            "document": TEST_DOCUMENT,
            "progress": xpath,
            "percentage": percentage,
            "device": DEVICE_NAME,
            "device_id": DEVICE_ID,
        })
        check("progress/upload-accepted", "update_progress", status, body)

        status, body = c.request("GET", f"/syncs/progress/{TEST_DOCUMENT}")
        extra = ""
        if _MAPPERS["get_progress"](status, body) == "OK":
            doc = json.loads(body)
            ok = doc.get("progress") == xpath and abs(float(doc.get("percentage", -1)) - percentage) < 1e-4
            extra = "stored value matches" if ok else f"STORED VALUE MISMATCH: {body}"
            if not ok:
                results.append(("progress/round-trip-payload", False, extra))
        check("progress/round-trip", "get_progress", status, body, extra)

        status, body = c.request("POST", "/users/create", auth=False,
                                 body={"username": "freshaccount", "password": TEST_KEY})
        check("register/new-account", "register", status, body)

        status, body = c.request("POST", "/users/create", auth=False,
                                 body={"username": TEST_USER, "password": TEST_KEY})
        check("register/existing-account", "register", status, body)
    finally:
        httpd.shutdown()
        httpd.server_close()

    return results


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true", help="Mapping table only; opens no sockets")
    p.add_argument("--dialect", choices=sorted(DIALECTS), help="Run one dialect instead of all")
    p.add_argument("--verbose", action="store_true", help="Log every request the mock server serves")
    args = p.parse_args()

    if args.self_test:
        return run_self_test()

    print("Offline mapping table")
    if run_self_test() != 0:
        return 1
    print()

    dialects = [args.dialect] if args.dialect else sorted(EXPECTATIONS)
    total = 0
    failed: list[str] = []
    for dialect in dialects:
        print(f"{dialect} -- {DIALECTS[dialect]['description']}")
        try:
            results = run_dialect(dialect, args.verbose)
        except RuntimeError as e:
            print(f"  [FAIL] {e}")
            failed.append(f"{dialect}/transport")
            continue
        for name, ok, detail in results:
            total += 1
            print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
            if not ok:
                failed.append(f"{dialect}/{name}")
        print()

    print(f"{total - len(failed)}/{total} dialect checks passed")
    if failed:
        print("Failed: " + ", ".join(failed))
        print("A failure means KOReaderSyncClient.cpp would mis-handle that server dialect.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
