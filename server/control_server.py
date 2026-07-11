#!/usr/bin/env python3
"""Control panel for the CYD usage server: status, logs, enable/disable.

Serves server.html plus a small JSON API on 127.0.0.1:8788. Bound to
localhost ONLY because /api/enable and /api/disable shell out to launchctl —
never expose this port to the LAN.
"""

import json
import os
import subprocess
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8788
JOB_LABEL = "com.eunite.cydusage"
PLIST_PATH = os.path.expanduser("~/Library/LaunchAgents/%s.plist" % JOB_LABEL)
HTML_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "server.html")
USAGE_URL = "http://127.0.0.1:8787/api/usage"
LOG_OUT = "/tmp/cydusage.log"
LOG_ERR = "/tmp/cydusage.err"
LOG_TAIL_LINES = 40


def run(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        return out.returncode, out.stdout, out.stderr
    except Exception as exc:
        return -1, "", str(exc)


def launchd_status():
    code, out, _ = run(["/bin/launchctl", "print", "gui/%d/%s" % (os.getuid(), JOB_LABEL)])
    if code != 0:
        return {"loaded": False, "pid": None, "state": "not loaded", "uptime": None}
    pid = None
    state = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("pid = "):
            try:
                pid = int(line.split("=", 1)[1])
            except ValueError:
                pass
        elif line.startswith("state = "):
            state = line.split("=", 1)[1].strip()
    uptime = None
    if pid:
        code, out, _ = run(["/bin/ps", "-o", "etime=", "-p", str(pid)])
        if code == 0:
            uptime = out.strip() or None
    return {"loaded": True, "pid": pid, "state": state or "unknown", "uptime": uptime}


def probe_endpoint():
    t0 = time.time()
    try:
        with urllib.request.urlopen(USAGE_URL, timeout=3) as resp:
            doc = json.loads(resp.read())
        return {
            "up": True,
            "latency_ms": int((time.time() - t0) * 1000),
            "generated_at": doc.get("generated_at"),
            "limits_ok": doc.get("limits") is not None,
            "clients": doc.get("clients") or [],
            "today": doc.get("today"),
            "week": doc.get("week"),
            "active_now": doc.get("active_now"),
        }
    except Exception as exc:
        return {"up": False, "error": str(exc)}


def tail_file(path, n=LOG_TAIL_LINES):
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            if size > 8192:
                f.seek(size - 8192)
            data = f.read()
        lines = data.decode("utf-8", errors="replace").splitlines()
        return lines[-n:]
    except OSError:
        return []


def do_enable():
    if not os.path.exists(PLIST_PATH):
        return False, "plist not found at %s — see README Part 1" % PLIST_PATH
    code, out, err = run(["/bin/launchctl", "bootstrap", "gui/%d" % os.getuid(), PLIST_PATH])
    if code == 0:
        return True, "server enabled (launchd job loaded)"
    # Already loaded is not a failure worth alarming over.
    if "already bootstrapped" in err.lower() or "service already loaded" in err.lower():
        return True, "server was already enabled"
    return False, (err or out or "launchctl bootstrap failed").strip()


def do_disable():
    code, out, err = run(["/bin/launchctl", "bootout", "gui/%d/%s" % (os.getuid(), JOB_LABEL)])
    if code == 0:
        return True, "server disabled (launchd job unloaded)"
    if "no such process" in err.lower() or code == 3:
        return True, "server was already disabled"
    return False, (err or out or "launchctl bootout failed").strip()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, doc, status=200):
        self._send(status, json.dumps(doc).encode("utf-8"), "application/json")

    def do_GET(self):
        if self.path in ("/", "/index.html", "/server.html"):
            try:
                with open(HTML_PATH, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            except OSError:
                self._send(404, b"server.html not found", "text/plain")
            return
        if self.path == "/api/status":
            self._send_json({
                "now": time.time(),
                "launchd": launchd_status(),
                "endpoint": probe_endpoint(),
                "logs": {"out": tail_file(LOG_OUT), "err": tail_file(LOG_ERR)},
            })
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if self.path == "/api/enable":
            ok, detail = do_enable()
        elif self.path == "/api/disable":
            ok, detail = do_disable()
        else:
            self._send(404, b"not found", "text/plain")
            return
        self._send_json({"ok": ok, "detail": detail})


class Server(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    server = Server(("127.0.0.1", PORT), Handler)
    print("Control panel on http://127.0.0.1:%d/ (localhost only)" % PORT)
    server.serve_forever()


if __name__ == "__main__":
    main()
