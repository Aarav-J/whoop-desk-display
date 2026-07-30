#!/usr/bin/env python3
"""WHOOP API backend proxy for the ESP32 desk display.

Aggregates recovery, strain, sleep, and body data from the WHOOP Developer API
into a single flat JSON snapshot the ESP32 consumes via HTTP GET.

Usage:
    WHOOP_CLIENT_ID=... WHOOP_CLIENT_SECRET=... python3 whoop_backend.py

The server shares the token cache with verify_whoop.py (.whoop_tokens.json).
If no cached tokens exist, it launches the browser OAuth flow on first request.

Endpoints:
    GET /api/snapshot   →  flat aggregated JSON (cached 60 s)
    GET /health         →  liveness probe
"""

import http.server
import json
import os
import secrets
import sys
import threading
import time
import urllib.parse
import urllib.request
import webbrowser
from datetime import datetime, timezone


# ── .env loader (no dependencies) ──────────────────────────────────────────
def _load_dotenv(path=None):
    if path is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, val = line.partition("=")
                key = key.strip()
                val = val.strip().strip("\"'")
                if key and key not in os.environ:
                    os.environ[key] = val
    except OSError:
        pass  # .env is optional

_load_dotenv()

# ── Config ──────────────────────────────────────────────────────────────────
AUTH_URL   = "https://api.prod.whoop.com/oauth/oauth2/auth"
TOKEN_URL  = "https://api.prod.whoop.com/oauth/oauth2/token"
API_BASE   = "https://api.prod.whoop.com/developer/v2"
REDIRECT_URI = "http://localhost:8787/callback"
CALLBACK_PORT = 8787
SCOPES     = ("read:recovery read:cycles read:sleep read:workout "
              "read:profile read:body_measurement offline")

TOKEN_CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           ".whoop_tokens.json")

CLIENT_ID     = os.environ.get("WHOOP_CLIENT_ID", "")
CLIENT_SECRET = os.environ.get("WHOOP_CLIENT_SECRET", "")
BIND_ADDR = "0.0.0.0"
PORT          = int(os.environ.get("WHOOP_BACKEND_PORT", "8080"))
CACHE_SECS    = int(os.environ.get("WHOOP_CACHE_SECS", "60"))
_cache = None              # (snapshot_dict, fetched_at, expires_at)
_cache_lock = threading.Lock()
_auth_lock  = threading.Lock()
_refresh_running = False   # prevent concurrent background refreshes
_access_token = None       # current bearer token
_token_expiry = 0.0        # unix timestamp when it expires

# ── HTTP helpers (reused from verify_whoop.py) ──────────────────────────────
def _http_json(method, url, data=None, headers=None, form=False):
    body = None
    hdrs = dict(headers or {})
    hdrs.setdefault("User-Agent",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36")
    if data is not None:
        if form:
            body = urllib.parse.urlencode(data).encode()
            hdrs["Content-Type"] = "application/x-www-form-urlencoded"
        else:
            body = json.dumps(data).encode()
            hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        raw = e.read().decode()
        try:
            return e.code, json.loads(raw)
        except json.JSONDecodeError:
            return e.code, {"raw": raw}
    except Exception as e:
        return 0, {"error": str(e)}


def _api_get(path, token):
    return _http_json("GET", API_BASE + path,
                      headers={"Authorization": f"Bearer {token}"})


def _field(obj, dotted):
    cur = obj
    for part in dotted.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


# ── Token management ────────────────────────────────────────────────────────
def _load_tokens():
    try:
        with open(TOKEN_CACHE) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def _save_tokens(tokens):
    tokens["_obtained_at"] = datetime.now(timezone.utc).isoformat()
    with open(TOKEN_CACHE, "w") as f:
        json.dump(tokens, f, indent=2)


def _refresh(tokens):
    status, new = _http_json("POST", TOKEN_URL, form=True, data={
        "grant_type": "refresh_token",
        "refresh_token": tokens["refresh_token"],
        "client_id": CLIENT_ID,
        "client_secret": CLIENT_SECRET,
        "scope": "offline",
    })
    return status, new


def _oauth_flow():
    """Browser consent flow — blocks until the redirect lands."""
    if not CLIENT_ID or not CLIENT_SECRET:
        raise RuntimeError(
            f"WHOOP_CLIENT_ID={'set' if CLIENT_ID else 'MISSING'}  "
            f"WHOOP_CLIENT_SECRET={'set' if CLIENT_SECRET else 'MISSING'}  "
            f"— check your .env file")

    state = secrets.token_urlsafe(12)
    params = urllib.parse.urlencode({
        "client_id": CLIENT_ID,
        "redirect_uri": REDIRECT_URI,
        "response_type": "code",
        "scope": SCOPES,
        "state": state,
    })
    auth_full = f"{AUTH_URL}?{params}"
    captured = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            q = urllib.parse.urlparse(self.path).query
            captured.update(urllib.parse.parse_qsl(q))
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                b"<h2>WHOOP auth captured &mdash; you can close this tab.</h2>")

        def log_message(self, *a):
            pass

    server = http.server.HTTPServer(("127.0.0.1", CALLBACK_PORT), Handler)
    server.timeout = 180
    print(f"Opening browser for WHOOP consent…\nIf it didn't open, visit:\n  {auth_full}\n")
    webbrowser.open(auth_full)
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()
    thread.join(timeout=180)
    server.server_close()

    if "error" in captured:
        raise RuntimeError(
            f"OAuth error: {captured.get('error')} — {captured.get('error_description', '')}")
    if captured.get("state") != state:
        raise RuntimeError(f"OAuth state mismatch: {captured.get('state')}")
    code = captured.get("code")
    if not code:
        raise RuntimeError("No authorization code in redirect")

    status, tokens = _http_json("POST", TOKEN_URL, form=True, data={
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT_URI,
        "client_id": CLIENT_ID,
        "client_secret": CLIENT_SECRET,
    })
    if status != 200:
        raise RuntimeError(f"Code exchange failed: HTTP {status} {tokens}")
    print(f"OAuth complete — tokens cached. Refresh tokens rotate automatically"
          f" (no re-login needed). Access token TTL: {tokens.get('expires_in', '?')}s")
    return tokens


def _get_valid_access_token():
    """Return a valid access token, refreshing only when within 5 min of expiry.
    With the 'offline' scope, refresh tokens rotate automatically — you never
    need to re-authenticate through the browser."""
    global _access_token, _token_expiry
    with _auth_lock:
        now = time.time()
        if _access_token and now < _token_expiry - 300:
            return _access_token  # still valid with buffer

        # Token missing or expiring soon — try refresh
        tokens = _load_tokens()
        if tokens and tokens.get("refresh_token"):
            status, new = _refresh(tokens)
            if status == 200 and "access_token" in new:
                _access_token = new["access_token"]
                _token_expiry = now + new.get("expires_in", 3600)
                _save_tokens(new)
                return _access_token
            print(f"[auth] Refresh failed (HTTP {status}) — re-authorizing…")

        # No cached tokens or refresh failed — full OAuth
        tokens = _oauth_flow()
        _access_token = tokens["access_token"]
        _token_expiry = now + tokens.get("expires_in", 3600)
        _save_tokens(tokens)
        return _access_token

# ── Data aggregation ────────────────────────────────────────────────────────
def _fetch_snapshot():
    """Hit WHOOP endpoints and return the flat snapshot dict for the ESP32."""
    token = _get_valid_access_token()

    snap = {}

    # -- Recovery -------------------------------------------------------------
    status, data = _api_get("/recovery?limit=1", token)
    if status == 200 and data.get("records"):
        r = data["records"][0]
        s = r.get("score", {})
        snap["recovery"] = s.get("recovery_score", 0)
        snap["hrv"]      = s.get("hrv_rmssd_milli", 0.0)
        snap["rhr"]      = s.get("resting_heart_rate", 0)
        snap["spo2"]     = int(s.get("spo2_percentage", 0) or 0)
    else:
        print(f"[warn] /recovery failed: HTTP {status}")

    # -- Cycle (strain) -------------------------------------------------------
    status, data = _api_get("/cycle?limit=1", token)
    if status == 200 and data.get("records"):
        s = data["records"][0].get("score", {})
        snap["strain"] = s.get("strain", 0.0)
        snap["avg_hr"] = s.get("average_heart_rate", 0)
    else:
        print(f"[warn] /cycle failed: HTTP {status}")

    # -- Sleep ----------------------------------------------------------------
    status, data = _api_get("/activity/sleep?limit=1", token)
    if status == 200 and data.get("records"):
        s = data["records"][0].get("score", {})
        snap["sleep_perf"] = int(s.get("sleep_performance_percentage", 0) or 0)
        debt_ms = _field(data["records"][0],
                         "score.sleep_needed.need_from_sleep_debt_milli") or 0
        snap["sleep_debt_min"] = int(debt_ms / 60000)
    else:
        print(f"[warn] /activity/sleep failed: HTTP {status}")

    # -- Body (max HR) --------------------------------------------------------
    status, data = _api_get("/user/measurement/body", token)
    if status == 200:
        snap["max_hr"] = data.get("max_heart_rate", 0)
    else:
        print(f"[warn] /user/measurement/body failed: HTTP {status}")

    return snap


def _refresh_cache_async():
    """Background thread: fetch fresh snapshot and update cache."""
    global _cache, _refresh_running
    try:
        snap = _fetch_snapshot()
        with _cache_lock:
            _cache = (snap, time.time(), time.time() + CACHE_SECS)
        print(f"[cache] refreshed — recovery={snap.get('recovery')}%  "
              f"strain={snap.get('strain')}  sleep={snap.get('sleep_perf')}%")
    except Exception as e:
        print(f"[cache] background refresh failed: {e}")
    finally:
        _refresh_running = False


def get_snapshot():
    """Return cached snapshot. Stale-while-revalidate: return immediately
    if we have any data, refresh in background if stale."""
    global _cache, _refresh_running
    now = time.time()
    with _cache_lock:
        if _cache is not None:
            snap, fetched_at, expires_at = _cache
            if now < expires_at:
                return snap  # fresh cache hit
            # Stale — return old data, trigger background refresh
            if not _refresh_running:
                _refresh_running = True
                threading.Thread(target=_refresh_cache_async, daemon=True).start()
            return snap

    # Cold cache (first request ever) — must block, but only once
    snap = _fetch_snapshot()
    with _cache_lock:
        _cache = (snap, time.time(), time.time() + CACHE_SECS)
    return snap


# ── HTTP server ─────────────────────────────────────────────────────────────
class BackendHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path

        if path == "/api/snapshot":
            try:
                snap = get_snapshot()
                body = json.dumps(snap).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", f"max-age={CACHE_SECS}")
                self.end_headers()
                self.wfile.write(body)
            except Exception as e:
                msg = json.dumps({"error": str(e)}).encode()
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(msg)))
                self.end_headers()
                self.wfile.write(msg)
                print(f"[error] GET /api/snapshot → {e}")

        elif path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"ok\n")

        elif path == "/auth":
            global _access_token, _token_expiry
            now = time.time()
            status = {
                "token_set": bool(_access_token),
                "expires_in_s": max(0, int(_token_expiry - now)),
                "auto_refresh": True,
                "reauth_needed": False,
            }
            body = json.dumps(status).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, fmt, *args):
        print(f"[{self.client_address[0]}] {args[0]}")


def main():
    if not CLIENT_ID or not CLIENT_SECRET:
        print("Set WHOOP_CLIENT_ID and WHOOP_CLIENT_SECRET first:\n")
        print("  export WHOOP_CLIENT_ID=<from developer-dashboard.whoop.com>")
        print("  export WHOOP_CLIENT_SECRET=<same>\n")
        sys.exit(2)

    # Prime the cache on startup so the first request is fast
    print("Priming snapshot cache…")
    try:
        snap = get_snapshot()
        print(f"  recovery={snap.get('recovery')}%  strain={snap.get('strain')}  "
              f"sleep={snap.get('sleep_perf')}%  hrv={snap.get('hrv')}ms")
    except Exception as e:
        print(f"[warn] Initial fetch failed (will retry on request): {e}")

    server = http.server.HTTPServer((BIND_ADDR, PORT), BackendHandler)
    print(f"\nBackend listening on http://{BIND_ADDR}:{PORT}")
    print(f"  ESP32 endpoint:  http://<this-machine-ip>:{PORT}/api/snapshot")
    print(f"  Health check:    http://{BIND_ADDR}:{PORT}/health")
    print(f"  Cache TTL:       {CACHE_SECS}s")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
