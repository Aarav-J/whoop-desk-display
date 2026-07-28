#!/usr/bin/env python3
"""WHOOP API capability verification for the desk-display project.

Proves, against your real account:
  1. OAuth authorization-code flow (localhost redirect, state check)
  2. Refresh-token issuance + rotation (offline scope)
  3. Every endpoint/field the display needs:
       recovery  -> recovery_score, hrv_rmssd_milli, resting_heart_rate, spo2, skin_temp
       cycle     -> strain, average_heart_rate, max_heart_rate, kilojoule
       sleep     -> sleep_performance_percentage, stage durations, sleep debt, efficiency
       workout   -> strain, avg/max HR, zone durations
       profile   -> user_id, name
       body      -> max_heart_rate (for HR-alert threshold)

Usage:
    WHOOP_CLIENT_ID=... WHOOP_CLIENT_SECRET=... python3 verify_whoop.py

Tokens are cached in .whoop_tokens.json so re-runs skip the browser flow.
"""

import http.server
import json
import os
import secrets
import sys
import threading
import urllib.parse
import urllib.request
import webbrowser
from datetime import datetime, timezone

AUTH_URL = "https://api.prod.whoop.com/oauth/oauth2/auth"
TOKEN_URL = "https://api.prod.whoop.com/oauth/oauth2/token"
API_BASE = "https://api.prod.whoop.com/developer/v2"
REDIRECT_URI = "http://localhost:8787/callback"
CALLBACK_PORT = 8787
SCOPES = "read:recovery read:cycles read:sleep read:workout read:profile read:body_measurement offline"
TOKEN_CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".whoop_tokens.json")

CLIENT_ID = os.environ.get("WHOOP_CLIENT_ID", "")
CLIENT_SECRET = os.environ.get("WHOOP_CLIENT_SECRET", "")

PASS, FAIL, WARN = "PASS", "FAIL", "WARN"
results = []


def report(status, name, detail=""):
    results.append((status, name, detail))
    print(f"  [{status}] {name}" + (f" — {detail}" if detail else ""))


def http_json(method, url, data=None, headers=None, form=False):
    body = None
    hdrs = dict(headers or {})
    hdrs.setdefault("User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36")
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


def oauth_flow():
    """Run the browser consent flow, capture the code on localhost."""
    state = secrets.token_urlsafe(12)  # >= 8 chars per WHOOP docs
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
            self.wfile.write(b"<h2>WHOOP auth captured - return to the terminal.</h2>")

        def log_message(self, *a):
            pass

    server = http.server.HTTPServer(("127.0.0.1", CALLBACK_PORT), Handler)
    server.timeout = 180
    print(f"\nOpening browser for WHOOP consent...\nIf it didn't open, visit:\n  {auth_full}\n")
    webbrowser.open(auth_full)
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()
    thread.join(timeout=180)
    server.server_close()

    if "error" in captured:
        report(FAIL, "OAuth consent", f"error={captured.get('error')} {captured.get('error_description','')}")
        sys.exit(1)
    if captured.get("state") != state:
        report(FAIL, "OAuth state check", f"expected {state!r}, got {captured.get('state')!r}")
        sys.exit(1)
    report(PASS, "OAuth consent + state check")

    code = captured.get("code")
    if not code:
        report(FAIL, "Authorization code", "no code in redirect")
        sys.exit(1)

    status, tokens = http_json("POST", TOKEN_URL, form=True, data={
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT_URI,
        "client_id": CLIENT_ID,
        "client_secret": CLIENT_SECRET,
    })
    if status != 200:
        report(FAIL, "Code exchange", f"HTTP {status}: {tokens}")
        sys.exit(1)
    report(PASS, "Authorization code exchange", f"expires_in={tokens.get('expires_in')}s")
    return tokens


def save_tokens(tokens):
    tokens["_obtained_at"] = datetime.now(timezone.utc).isoformat()
    with open(TOKEN_CACHE, "w") as f:
        json.dump(tokens, f, indent=2)


def load_tokens():
    try:
        with open(TOKEN_CACHE) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def refresh(tokens):
    status, new = http_json("POST", TOKEN_URL, form=True, data={
        "grant_type": "refresh_token",
        "refresh_token": tokens["refresh_token"],
        "client_id": CLIENT_ID,
        "client_secret": CLIENT_SECRET,
        "scope": "offline",
    })
    return status, new


def api_get(path, token):
    status, body = http_json("GET", API_BASE + path,
                             headers={"Authorization": f"Bearer {token}"})
    return status, body


def field(obj, dotted):
    cur = obj
    for part in dotted.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def check_fields(name, obj, fields):
    missing = [f for f in fields if field(obj, f) is None]
    if missing:
        report(WARN, name, f"missing fields: {', '.join(missing)}")
    else:
        report(PASS, name)


def main():
    print("=" * 64)
    print("WHOOP API capability verification")
    print("=" * 64)

    if not CLIENT_ID or not CLIENT_SECRET:
        print("\nSet WHOOP_CLIENT_ID and WHOOP_CLIENT_SECRET first:\n")
        print("  export WHOOP_CLIENT_ID=<from developer-dashboard.whoop.com>")
        print("  export WHOOP_CLIENT_SECRET=<same>\n")
        sys.exit(2)

    # --- 1. Tokens: cache -> refresh -> full OAuth -------------------------
    print("\n-- Auth ------------------------------------------------------------")
    tokens = load_tokens()
    if tokens:
        status, new = refresh(tokens)
        if status == 200 and "access_token" in new:
            tokens = new
            report(PASS, "Refresh token rotation", "cached token refreshed")
        else:
            report(WARN, "Cached token refresh failed; redoing OAuth", f"HTTP {status}")
            tokens = None
    if not tokens:
        tokens = oauth_flow()

    if "refresh_token" not in tokens:
        report(FAIL, "Refresh token issued", "missing — is the 'offline' scope granted on the app?")
    else:
        report(PASS, "Refresh token issued (offline scope)")
    save_tokens(tokens)

    # Prove rotation semantics: a second refresh succeeds, and per WHOOP docs
    # the new access/refresh pair supersedes the old one. We reuse the returned
    # pair immediately, which is what the production poller will do.
    old_refresh = tokens.get("refresh_token")
    if old_refresh:
        status, new = refresh(tokens)
        if status == 200 and new.get("refresh_token") and new["refresh_token"] != old_refresh:
            report(PASS, "Refresh token rotation verified", "new token differs")
            tokens = new
            save_tokens(tokens)
            status2, body2 = refresh({"refresh_token": old_refresh})
            if status2 != 200:
                report(PASS, "Old refresh token invalidated", f"HTTP {status2}")
            else:
                report(WARN, "Old refresh token still valid", "WHOOP did not invalidate it")
        else:
            report(WARN, "Rotation check skipped/failed", f"HTTP {status}: {new}")

    access = tokens["access_token"]

    # --- 2. Profile & body measurement --------------------------------------
    print("\n-- Identity ----------------------------------------------------------")
    status, profile = api_get("/user/profile/basic", access)
    if status == 200:
        report(PASS, "GET /user/profile/basic",
               f"user_id={profile.get('user_id')} name={profile.get('first_name')}")
    else:
        report(FAIL, "GET /user/profile/basic", f"HTTP {status}: {profile}")

    status, body_m = api_get("/user/measurement/body", access)
    max_hr = None
    if status == 200:
        max_hr = body_m.get("max_heart_rate")
        check_fields("GET /user/measurement", body_m,
                     ["height_meter", "weight_kilogram", "max_heart_rate"])
        if max_hr:
            print(f"        -> max_heart_rate={max_hr} (HR alert threshold source)")
    else:
        report(FAIL, "GET /user/measurement", f"HTTP {status}: {body_m}")

    # --- 3. Recovery ---------------------------------------------------------
    print("\n-- Recovery ------------------------------------------------------------")
    status, rec = api_get("/recovery?limit=3", access)
    records = rec.get("records", []) if status == 200 else []
    if records:
        latest = records[0]
        check_fields("GET /recovery (latest)", latest, [
            "score_state", "score.recovery_score", "score.hrv_rmssd_milli",
            "score.resting_heart_rate", "score.spo2_percentage",
            "score.skin_temp_celsius", "score.user_calibrating",
        ])
        s = latest.get("score", {})
        print(f"        -> score={s.get('recovery_score')}%  hrv={s.get('hrv_rmssd_milli')}ms  "
              f"rhr={s.get('resting_heart_rate')}bpm  state={latest.get('score_state')}")
    else:
        report(WARN, "GET /recovery", f"no records (HTTP {status}) — wearable synced recently?")

    # --- 4. Cycles (strain) --------------------------------------------------
    print("\n-- Cycles (strain) -----------------------------------------------------")
    status, cyc = api_get("/cycle?limit=3", access)
    records = cyc.get("records", []) if status == 200 else []
    if records:
        latest = records[0]
        check_fields("GET /cycle (latest)", latest, [
            "score_state", "score.strain", "score.average_heart_rate",
            "score.max_heart_rate", "score.kilojoule",
        ])
        s = latest.get("score", {})
        strain = s.get("strain")
        if strain is not None:
            print(f"        -> strain={strain:.1f}  avg_hr={s.get('average_heart_rate')}  "
                  f"max_hr={s.get('max_heart_rate')}  kJ={s.get('kilojoule', 0):.0f}")
    else:
        report(WARN, "GET /cycle", f"no records (HTTP {status})")

    # --- 5. Sleep --------------------------------------------------------------
    print("\n-- Sleep ----------------------------------------------------------------")
    status, slp = api_get("/activity/sleep?limit=3", access)
    records = slp.get("records", []) if status == 200 else []
    if records:
        latest = records[0]
        check_fields("GET /activity/sleep (latest)", latest, [
            "nap", "score_state", "score.stage_summary.total_in_bed_time_milli",
            "score.stage_summary.total_light_sleep_time_milli",
            "score.stage_summary.total_slow_wave_sleep_time_milli",
            "score.stage_summary.total_rem_sleep_time_milli",
            "score.stage_summary.total_awake_time_milli",
            "score.sleep_needed.need_from_sleep_debt_milli",
            "score.sleep_performance_percentage",
            "score.sleep_efficiency_percentage",
            "score.respiratory_rate",
        ])
        s = latest.get("score", {})
        perf = s.get("sleep_performance_percentage")
        debt_ms = field(latest, "score.sleep_needed.need_from_sleep_debt_milli") or 0
        print(f"        -> perf={perf}%  debt={debt_ms/60000:.0f}min  "
              f"efficiency={s.get('sleep_efficiency_percentage')}%")
    else:
        report(WARN, "GET /activity/sleep", f"no records (HTTP {status})")

    # --- 6. Workouts -----------------------------------------------------------
    print("\n-- Workouts ---------------------------------------------------------------")
    status, wk = api_get("/activity/workout?limit=3", access)
    records = wk.get("records", []) if status == 200 else []
    if records:
        latest = records[0]
        check_fields("GET /activity/workout (latest)", latest, [
            "sport_name", "score_state", "score.strain",
            "score.average_heart_rate", "score.max_heart_rate",
            "score.zone_durations.zone_five_milli",
        ])
        s = latest.get("score", {})
        print(f"        -> {latest.get('sport_name')}: strain={s.get('strain')}  "
              f"avg_hr={s.get('average_heart_rate')}  max_hr={s.get('max_heart_rate')}")
    else:
        report(WARN, "GET /activity/workout", f"no records (HTTP {status}) — log a workout to test this")

    # --- 7. Steps check (expected: NOT available) ------------------------------
    print("\n-- Known gaps -------------------------------------------------------------")
    blob = json.dumps([rec, cyc, slp, wk, body_m]).lower()
    if "step" in blob:
        report(WARN, "Steps", "'step' string found in payloads — inspect manually")
    else:
        report(PASS, "Steps confirmed absent from API", "get steps from another source or drop the feature")

    # --- Summary -------------------------------------------------------------
    print("\n" + "=" * 64)
    npass = sum(1 for r in results if r[0] == PASS)
    nwarn = sum(1 for r in results if r[0] == WARN)
    nfail = sum(1 for r in results if r[0] == FAIL)
    print(f"RESULT: {npass} pass, {nwarn} warn, {nfail} fail")
    if max_hr:
        print(f"HR alert threshold reference: 90% of max_hr = {int(max_hr * 0.9)} bpm")
    print("=" * 64)
    sys.exit(1 if nfail else 0)


if __name__ == "__main__":
    main()
