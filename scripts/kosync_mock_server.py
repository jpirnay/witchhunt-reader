#!/usr/bin/env python3
"""An offline KOSync server that can impersonate the dialects the firmware has to cope with.

Real KOSync implementations disagree about status codes. The reference server
(sync.koreader.rocks) answers 200 almost everywhere; Spring-based services such as
BookLore/grimmory use the idiomatic codes and answer a PUT with 201/204, which used to land
in SERVER_ERROR and made every push fail after a successful pull (crosspoint-reader #2876).
Reproducing that needed an account on somebody else's server. This removes that need.

Two ways to use it:

1. Point the device at it. This is the valuable one -- it exercises the real
   lib/KOReaderSync client, esp_http_client and all, against a server dialect of your
   choosing, over plain HTTP with no TLS handshake and no account anywhere:

     python scripts/kosync_mock_server.py --dialect spring --host 0.0.0.0 --port 8080

   Then on the device, Settings -> System -> KOReader Sync -> Sync Server URL:
   http://<your-lan-ip>:8080, with any username/password (see --user/--password below).
   Every request is logged, so you can watch exactly what the firmware sends.

2. Let scripts/kosync_conformance.py drive it. That spawns one instance per dialect and
   asserts the client's status -> Error mapping comes out right for each.

Endpoints mirror the KOSync API the firmware speaks:

  POST /users/create            body {username, password}   (password is already an MD5)
  GET  /users/auth              headers x-auth-user, x-auth-key
  GET  /syncs/progress/:document
  PUT  /syncs/progress          body {document, progress, percentage, device, device_id,
                                      metadata?, position?}
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------------------------------
# Dialects. Each entry says what status/body this flavour of server answers with.
# ---------------------------------------------------------------------------------------

DIALECTS: dict[str, dict] = {
    # sync.koreader.rocks and the original Lua kosync. 200 for nearly everything;
    # "no progress for this document" is 200 with an empty JSON object, not 404.
    "reference": {
        "description": "Reference kosync (sync.koreader.rocks): 200 everywhere, {} for no progress",
        "auth_ok": (200, '{"username": "%(user)s"}'),
        "create_ok": (201, '{"username": "%(user)s"}'),
        "create_exists": (402, '{"code": 2002, "message": "Username is already registered."}'),
        "progress_missing": (200, "{}"),
        "progress_found": (200, None),
        "put_ok": (200, '{"document": "%(document)s", "timestamp": %(timestamp)d}'),
    },
    # BookLore / grimmory: a Spring service using the idiomatic codes. This is the dialect
    # that broke the client before the 2xx widening.
    "spring": {
        "description": "Spring-based KOSync (BookLore/grimmory): 204/201, the #2876 dialect",
        "auth_ok": (204, ""),
        "create_ok": (201, ""),
        "create_exists": (409, ""),
        "progress_missing": (204, ""),
        "progress_found": (200, None),
        "put_ok": (201, ""),
    },
    # A server that answers every successful call with a bare 204. Legal, and the most
    # aggressive thing a 2xx-tolerant client has to survive.
    "bodiless": {
        "description": "Every success is a bodiless 204",
        "auth_ok": (204, ""),
        "create_ok": (204, ""),
        "create_exists": (409, ""),
        "progress_missing": (204, ""),
        "progress_found": (200, None),
        "put_ok": (204, ""),
    },
    # Registration switched off server-side. The reference server signals this with the
    # same 402 it uses for "already exists", distinguished only by the message body.
    "registration_disabled": {
        "description": "Reference dialect, but /users/create is disabled (402 + message)",
        "auth_ok": (200, '{"username": "%(user)s"}'),
        "create_ok": (402, '{"code": 2005, "message": "Registration is disabled."}'),
        "create_exists": (402, '{"code": 2005, "message": "Registration is disabled."}'),
        "progress_missing": (200, "{}"),
        "progress_found": (200, None),
        "put_ok": (200, "{}"),
    },
    # A captive portal or misconfigured reverse proxy: HTTP 200, but the body is HTML.
    # The client must refuse this rather than treat it as success.
    "portal": {
        "description": "Captive portal: 200 with HTML for everything",
        "auth_ok": (200, "<html><body>Sign in to the network</body></html>"),
        "create_ok": (200, "<html><body>Sign in to the network</body></html>"),
        "create_exists": (200, "<html><body>Sign in to the network</body></html>"),
        "progress_missing": (200, "<html><body>Sign in to the network</body></html>"),
        "progress_found": (200, "<html><body>Sign in to the network</body></html>"),
        "put_ok": (200, "<html><body>Sign in to the network</body></html>"),
    },
    # A proxy that redirects everything, e.g. http -> https upgrade the client won't follow.
    "redirect": {
        "description": "Everything answers 302",
        "auth_ok": (302, ""),
        "create_ok": (302, ""),
        "create_exists": (302, ""),
        "progress_missing": (302, ""),
        "progress_found": (302, ""),
        "put_ok": (302, ""),
    },
}

PROGRESS_PATH = re.compile(r"^/syncs/progress/([0-9a-fA-F]+)$")


class MockState:
    """Accounts and stored progress, shared across handler threads."""

    def __init__(self, dialect: str, user: str, auth_key: str, verbose: bool = True,
                 accept_any_key: bool = False) -> None:
        self.rules = DIALECTS[dialect]
        self.dialect = dialect
        self.users: dict[str, str] = {user: auth_key} if user else {}
        self.progress: dict[str, dict] = {}
        self.verbose = verbose
        # When set, any x-auth-key is accepted for a known user. Convenient when pointing a
        # device at the mock and you do not want to compute the password MD5 by hand.
        self.accept_any_key = accept_any_key
        self.lock = threading.Lock()
        self.request_log: list[tuple[str, str, int]] = []


def make_handler(state: MockState):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        # -- plumbing ----------------------------------------------------------------
        def log_message(self, fmt, *a):  # noqa: A003 - BaseHTTPRequestHandler hook
            if state.verbose:
                sys.stderr.write("  %s\n" % (fmt % a))

        def _body(self) -> str:
            length = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(length).decode("utf-8", "replace") if length else ""

        def _respond(self, status: int, body: str = "", content_type: str = "application/json") -> None:
            payload = body.encode()
            self.send_response(status)
            # 204/205 must not carry a body or a Content-Length per RFC 9110.
            if status not in (204, 205) and payload:
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(payload)))
            elif status not in (204, 205):
                self.send_header("Content-Length", "0")
            if status in (301, 302, 303, 307, 308):
                self.send_header("Location", "https://elsewhere.invalid/")
            self.end_headers()
            if status not in (204, 205) and payload:
                self.wfile.write(payload)
            with state.lock:
                state.request_log.append((self.command, self.path, status))

        def _rule(self, name: str, **fmt) -> tuple[int, str]:
            status, body = state.rules[name]
            if body is None:
                return (status, "")
            fmt.setdefault("user", self.headers.get("x-auth-user") or "")
            return (status, body % fmt if "%(" in body else body)

        def _authenticated(self) -> bool:
            user = self.headers.get("x-auth-user")
            key = self.headers.get("x-auth-key")
            if not user:
                return False
            with state.lock:
                if user not in state.users:
                    return False
                return state.accept_any_key or state.users[user] == key

        # -- endpoints ---------------------------------------------------------------
        def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler hook
            if self.path == "/users/auth":
                if state.dialect in ("portal", "redirect"):
                    return self._respond(*self._rule("auth_ok"), content_type="text/html")
                if not self._authenticated():
                    return self._respond(401, '{"message": "Unauthorized"}')
                return self._respond(*self._rule("auth_ok"))

            m = PROGRESS_PATH.match(self.path)
            if m:
                if state.dialect in ("portal", "redirect"):
                    return self._respond(*self._rule("progress_found"), content_type="text/html")
                if not self._authenticated():
                    return self._respond(401, '{"message": "Unauthorized"}')
                document = m.group(1)
                with state.lock:
                    stored = state.progress.get(document)
                if stored is None:
                    return self._respond(*self._rule("progress_missing"))
                return self._respond(state.rules["progress_found"][0], json.dumps(stored))

            return self._respond(404, '{"message": "Not found"}')

        def do_POST(self):  # noqa: N802
            body = self._body()
            if self.path != "/users/create":
                return self._respond(404, '{"message": "Not found"}')
            if state.dialect in ("portal", "redirect"):
                return self._respond(*self._rule("create_ok"), content_type="text/html")
            try:
                payload = json.loads(body) if body else {}
            except ValueError:
                return self._respond(400, '{"message": "Bad JSON"}')
            username = payload.get("username") or ""
            password = payload.get("password") or ""
            if not username or not password:
                return self._respond(400, '{"message": "Missing credentials"}')
            with state.lock:
                exists = username in state.users
            if exists:
                return self._respond(*self._rule("create_exists", user=username))
            status, resp = self._rule("create_ok", user=username)
            # registration_disabled never actually registers anybody.
            if 200 <= status < 300:
                with state.lock:
                    state.users[username] = password
            return self._respond(status, resp)

        def do_PUT(self):  # noqa: N802
            body = self._body()
            if self.path != "/syncs/progress":
                return self._respond(404, '{"message": "Not found"}')
            if state.dialect in ("portal", "redirect"):
                return self._respond(*self._rule("put_ok"), content_type="text/html")
            if not self._authenticated():
                return self._respond(401, '{"message": "Unauthorized"}')
            try:
                payload = json.loads(body) if body else {}
            except ValueError:
                return self._respond(400, '{"message": "Bad JSON"}')
            document = payload.get("document")
            if not document:
                return self._respond(400, '{"message": "Missing document"}')
            record = {
                "document": document,
                "progress": payload.get("progress", ""),
                "percentage": payload.get("percentage", 0.0),
                "device": payload.get("device", ""),
                "device_id": payload.get("device_id", ""),
                "timestamp": int(time.time()),
            }
            # Echo back the optional extensions so a device round-trip shows what it sent.
            for optional in ("metadata", "position"):
                if optional in payload:
                    record[optional] = payload[optional]
            with state.lock:
                state.progress[document] = record
            return self._respond(*self._rule("put_ok", document=document, timestamp=record["timestamp"]))

    return Handler


def serve(dialect: str, host: str, port: int, user: str, auth_key: str,
          verbose: bool = True) -> tuple[ThreadingHTTPServer, MockState]:
    """Start the mock in a background thread. Returns (server, state); caller shuts it down.

    Pass port 0 to have the OS assign one; read it back from server.server_address[1].
    """
    state = MockState(dialect, user, auth_key, verbose)
    httpd = ThreadingHTTPServer((host, port), make_handler(state))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return (httpd, state)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dialect", choices=sorted(DIALECTS), default="reference")
    p.add_argument("--host", default="127.0.0.1",
                   help="Use 0.0.0.0 to let a device on the LAN reach it (default: %(default)s)")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--user", default="crosspoint", help="Pre-registered username (default: %(default)s)")
    p.add_argument("--auth-key", default="",
                   help="Expected x-auth-key, i.e. the MD5 of the password. Empty accepts any key "
                        "for that user, which is usually what you want when testing a device.")
    p.add_argument("--list-dialects", action="store_true")
    args = p.parse_args()

    if args.list_dialects:
        for name in sorted(DIALECTS):
            print(f"{name:22} {DIALECTS[name]['description']}")
        return 0

    state = MockState(args.dialect, args.user, args.auth_key, accept_any_key=not args.auth_key)
    httpd = ThreadingHTTPServer((args.host, args.port), make_handler(state))
    print(f"KOSync mock server on http://{args.host}:{args.port}")
    print(f"  dialect : {args.dialect} -- {DIALECTS[args.dialect]['description']}")
    print(f"  account : {args.user} ({'any key accepted' if not args.auth_key else 'fixed key'})")
    print("  point the device's Sync Server URL here; Ctrl-C to stop\n")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
