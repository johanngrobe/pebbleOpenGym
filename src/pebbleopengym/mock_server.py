"""A stand-in for the openGym scoped-token API, for the emulator.

Implements what the watch app uses, with openGym's rules: GET /api/token/data returns the
document and its revision; PUT needs the exact revision read (409 with the current document
otherwise) and may only write `workouts`, `exWeights` and `customEx` (Workouts: Change).
"""

from __future__ import annotations

import json
import secrets
import time
from datetime import date
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WRITABLE = {"workouts", "exWeights", "customEx"}


def sample_state() -> dict:
    today = date.today()
    push, pull, legs = "r_push", "r_pull", "r_legs"
    return {
        "unit": "kg",
        "lang": "de",
        "restSec": 90,
        "routines": [
            {"id": push, "name": "Push", "emoji": "💪", "ex": [
                {"id": "0025", "sets": 4, "reps": 6, "weight": 0},
                {"id": "0426", "sets": 3, "reps": 10, "weight": 20},
                {"id": "0334", "sets": 3, "reps": 12, "weight": 10, "restSec": 60},
                {"id": "0251", "sets": 3, "reps": 10, "weight": 0, "bodyweight": True},
            ]},
            {"id": pull, "name": "Pull", "emoji": "🏋️", "ex": [
                {"id": "0027", "sets": 4, "reps": 8, "weight": 50},
                {"id": "1323", "sets": 3, "reps": 10, "weight": 45},
                {"id": "0031", "sets": 3, "reps": 12, "weight": 30},
            ]},
            {"id": legs, "name": "Legs", "emoji": "🦵", "ex": [
                {"id": "0043", "sets": 5, "reps": 5, "weight": 70},
                {"id": "0085", "sets": 3, "reps": 8, "weight": 60},
                {"id": "0464", "sets": 3, "sec": 45, "weight": 0, "mode": "time", "bodyweight": True},
            ]},
        ],
        "week": {"1": push, "3": pull, "5": legs, str((today.isoweekday()) % 7): push},
        "dayPlan": {},
        "exWeights": {"0025": {"w": 60, "d": "2026-11-20"}},
        "workouts": [
            {"id": "seed1", "d": "2026-11-20", "start": 1763650000000, "end": 1763653600000,
             "routineId": push, "name": "Push", "prs": [], "vol": 1440,
             "entries": [{"id": "0025", "topW": 60, "sets": [{"w": 60, "r": 6, "done": True}] * 4}]},
        ],
    }


class Store:
    def __init__(self) -> None:
        self.state = sample_state()
        self.rev = 1
        self.wid = secrets.token_hex(8)


def serve(port: int, token: str) -> None:
    store = Store()

    class Handler(BaseHTTPRequestHandler):
        def _send(self, status: int, body: dict) -> None:
            data = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _authorized(self) -> bool:
            if self.headers.get("Authorization") != f"Bearer {token}":
                self._send(401, {"error": "token refused"})
                return False
            return True

        def do_GET(self) -> None:  # noqa: N802
            if self.path == "/api/token/me":
                if self._authorized():
                    self._send(200, {"user": {"id": "u1", "name": "Test"},
                                     "token": {"id": "t_1", "name": "Pebble",
                                               "scopes": {"workouts": "apply", "plans": "view"},
                                               "exp": None}})
                return
            if self.path != "/api/token/data":
                return self._send(404, {"error": "not found"})
            if self._authorized():
                self._send(200, {"state": store.state, "rev": store.rev, "wid": store.wid})

        def do_PUT(self) -> None:  # noqa: N802
            if self.path != "/api/token/data":
                return self._send(404, {"error": "not found"})
            if not self._authorized():
                return
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
            if not isinstance(body.get("baseRev"), int):
                return self._send(400, {"error": "baseRev required", "code": "base-rev"})
            if body["baseRev"] != store.rev:
                return self._send(409, {"error": "revision moved", "state": store.state,
                                        "rev": store.rev, "wid": store.wid})
            state = body.get("state") or {}
            bad = [k for k in state if k not in WRITABLE]
            if bad:
                return self._send(403, {"error": f"not writable: {bad}", "code": "scope"})
            store.state.update(state)
            store.rev += 1
            store.wid = secrets.token_hex(8)
            added = [w for w in state.get("workouts", []) if str(w.get("id", "")).startswith("pbl")]
            print(time.strftime("%H:%M:%S"), f"PUT rev {store.rev}: {len(added)} Pebble workout(s)")
            for w in added:
                print("   ", json.dumps(w, ensure_ascii=False))
            self._send(200, {"ok": True, "rev": store.rev, "wid": store.wid})

    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"mock openGym API on http://localhost:{port}  (token: {token})")
    server.serve_forever()
