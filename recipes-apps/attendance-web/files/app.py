#!/usr/bin/env python3
import argparse
import base64
import csv
import html
import json
import os
import secrets
import socket
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

DEFAULTS = {
    "ATTENDANCE_WEB_USER": "admin",
    "ATTENDANCE_WEB_PASSWORD": "",
    "ATTENDANCE_WEB_HOST": "0.0.0.0",
    "ATTENDANCE_WEB_PORT": "8080",
    "ATTENDANCE_CSV": "/var/lib/face-ai/attendance.csv",
    "ATTENDANCE_PERF_CSV": "/var/lib/face-ai/performance.csv",
}

def load_config(path):
    cfg = dict(DEFAULTS)
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            cfg[key.strip()] = value.strip().strip('"').strip("'")
    return cfg

def read_csv(path, limit=None):
    try:
        with open(path, "r", newline="", encoding="utf-8-sig") as f:
            reader = csv.DictReader(f)
            rows = list(reader)
            if limit:
                rows = rows[-limit:]
            return reader.fieldnames or [], rows, None
    except FileNotFoundError:
        return [], [], "File not found"
    except (OSError, UnicodeError, csv.Error) as e:
        return [], [], str(e)

def pick(row, names):
    lowered = {str(k).strip().lower(): v for k, v in row.items() if k is not None}
    for name in names:
        if name in lowered and lowered[name] not in (None, ""):
            return str(lowered[name])
    return ""

def esc(value):
    return html.escape(str(value if value is not None else ""), quote=True)

def process_running(fragment):
    try:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                raw = open("/proc/" + entry + "/cmdline", "rb").read()
                cmd = raw.replace(b"\0", b" ").decode("utf-8", "ignore")
                if fragment in cmd and "attendance-web" not in cmd:
                    return True
            except (OSError, PermissionError):
                pass
    except OSError:
        pass
    return False

def socket_status(path="/run/pir-monitor/events.sock"):
    return os.path.exists(path)

def status_items():
    return [
        ("face-ai process", process_running("face-ai")),
        ("attendance-app process", process_running("attendance-app")),
        ("PIR monitor process", process_running("pir-monitor")),
        ("PIR event socket", socket_status()),
    ]

CSS = """
:root{color-scheme:light;--ink:#17212b;--muted:#5e6b75;--line:#d9e0e5;--panel:#fff;--bg:#f3f6f8;--accent:#1d5f8a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}
header{background:#17364b;color:white;padding:18px 24px;display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap}
header h1{font-size:20px;margin:0}header span{font-size:13px;opacity:.85}
main{max-width:1200px;margin:24px auto;padding:0 16px}.nav{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:18px}
a.btn,button{background:var(--accent);color:white;text-decoration:none;border:0;border-radius:5px;padding:9px 13px;font:inherit;cursor:pointer}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:18px;margin-bottom:18px;overflow:auto}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}
.metric{border:1px solid var(--line);border-radius:7px;padding:14px}.metric strong{display:block;font-size:17px}.muted{color:var(--muted);font-size:13px}
.ok{color:#176b43}.bad{color:#a12626}h2{font-size:18px;margin:0 0 12px}table{border-collapse:collapse;width:100%;font-size:14px}
th,td{text-align:left;padding:9px 10px;border-bottom:1px solid var(--line);vertical-align:top;white-space:nowrap}
th{background:#f6f8f9}input{padding:9px;border:1px solid var(--line);border-radius:5px;max-width:100%}
footer{color:var(--muted);font-size:12px;margin:20px 0}
"""

class Handler(BaseHTTPRequestHandler):
    server_version = "AttendanceWeb/1.0"

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.address_string(), fmt % args), flush=True)

    def authorized(self):
        header = self.headers.get("Authorization", "")
        if not header.startswith("Basic "):
            return False
        try:
            decoded = base64.b64decode(header[6:], validate=True).decode("utf-8")
            user, password = decoded.split(":", 1)
        except Exception:
            return False
        cfg = self.server.cfg
        return (secrets.compare_digest(user, cfg["ATTENDANCE_WEB_USER"]) and
                secrets.compare_digest(password, cfg["ATTENDANCE_WEB_PASSWORD"]))

    def send_bytes(self, data, content_type="text/html; charset=utf-8", status=200, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def page(self, title, body, status=200):
        payload = ("<!doctype html><html lang='en'><head><meta charset='utf-8'>"
                   "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<title>" + esc(title) + "</title><style>" + CSS + "</style></head><body>"
                   "<header><h1>Face Attendance · Administration</h1><span>Read-only Web 1</span></header>"
                   "<main><nav class='nav'><a class='btn' href='/'>Dashboard</a>"
                   "<a class='btn' href='/employees'>Employees</a>"
                   "<a class='btn' href='/attendance'>Attendance</a>"
                   "<a class='btn' href='/export.csv'>Export CSV</a></nav>"
                   + body + "<footer>Read-only interface. Employee names are inferred from attendance records until the face registry format is integrated.</footer></main></body></html>")
        self.send_bytes(payload.encode("utf-8"), status=status)

    def do_GET(self):
        if not self.authorized():
            self.send_bytes(b"Authentication required", "text/plain; charset=utf-8", 401,
                            {"WWW-Authenticate": 'Basic realm="Attendance administration", charset="UTF-8"'})
            return
        route = urlparse(self.path).path
        if route == "/":
            self.dashboard()
        elif route == "/employees":
            self.employees()
        elif route == "/attendance":
            self.attendance()
        elif route == "/export.csv":
            self.export_csv()
        elif route == "/health":
            self.send_bytes(json.dumps({"ok": True, "time": datetime.utcnow().isoformat() + "Z"}).encode(),
                            "application/json; charset=utf-8")
        else:
            self.page("Not found", "<section class='panel'><h2>404 · Not found</h2></section>", 404)

    def dashboard(self):
        cfg = self.server.cfg
        fields, rows, error = read_csv(cfg["ATTENDANCE_CSV"])
        _, perf, perf_error = read_csv(cfg["ATTENDANCE_PERF_CSV"], 10)
        names = set()
        for row in rows:
            name = pick(row, ("name", "employee", "person", "identity", "label", "employee_name"))
            if name:
                names.add(name)
        cards = [
            ("Attendance rows", str(len(rows)), error or "Source: " + cfg["ATTENDANCE_CSV"]),
            ("Names in attendance", str(len(names)), "Distinct names in CSV"),
            ("Recent performance rows", str(len(perf)), perf_error or "Source: performance.csv"),
        ]
        body = "<section class='grid'>"
        for title, value, sub in cards:
            body += "<div class='metric'><span class='muted'>" + esc(title) + "</span><strong>" + esc(value) + "</strong><span class='muted'>" + esc(sub) + "</span></div>"
        body += "</section><section class='panel'><h2>Service status</h2><div class='grid'>"
        for label, ok in status_items():
            body += "<div class='metric'><strong class='" + ("ok" if ok else "bad") + "'>" + ("RUNNING" if ok else "NOT DETECTED") + "</strong><span>" + esc(label) + "</span></div>"
        body += "</div><p class='muted'>Process/socket checks are best-effort indicators, not full health checks. Camera connectivity is not independently verified by this page.</p></section>"
        body += "<section class='panel'><h2>Recent attendance</h2>" + table_html(fields, rows[-10:]) + "</section>"
        self.page("Dashboard", body)

    def employees(self):
        _, rows, error = read_csv(self.server.cfg["ATTENDANCE_CSV"])
        names = {}
        for row in rows:
            name = pick(row, ("name", "employee", "person", "identity", "label", "employee_name"))
            if name:
                names[name] = names.get(name, 0) + 1
        body = "<section class='panel'><h2>Employees seen in attendance data</h2>"
        if error:
            body += "<p class='bad'>Attendance CSV unavailable: " + esc(error) + "</p>"
        body += "<p class='muted'>This is not yet the complete face-registration database; it lists unique names found in attendance.csv.</p>"
        body += "<table><thead><tr><th>Name</th><th>Attendance records</th></tr></thead><tbody>"
        for name in sorted(names, key=str.casefold):
            body += "<tr><td>" + esc(name) + "</td><td>" + str(names[name]) + "</td></tr>"
        body += "</tbody></table></section>"
        self.page("Employees", body)

    def attendance(self):
        fields, rows, error = read_csv(self.server.cfg["ATTENDANCE_CSV"], 500)
        body = "<section class='panel'><h2>Attendance history</h2><p class='muted'>Showing up to the latest 500 rows.</p>"
        if error:
            body += "<p class='bad'>CSV unavailable: " + esc(error) + "</p>"
        body += table_html(fields, rows) + "</section>"
        self.page("Attendance", body)

    def export_csv(self):
        path = self.server.cfg["ATTENDANCE_CSV"]
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            self.page("Export unavailable", "<section class='panel'><h2>Cannot read CSV</h2><p>" + esc(e) + "</p></section>", 503)
            return
        self.send_bytes(data, "text/csv; charset=utf-8", 200,
                        {"Content-Disposition": 'attachment; filename="attendance.csv"'})

def table_html(fields, rows):
    if not fields:
        return "<p class='muted'>No CSV header/data available, or the file does not exist.</p>"
    out = "<table><thead><tr>" + "".join("<th>" + esc(f) + "</th>" for f in fields) + "</tr></thead><tbody>"
    for row in rows:
        out += "<tr>" + "".join("<td>" + esc(row.get(f, "")) + "</td>" for f in fields) + "</tr>"
    out += "</tbody></table>"
    return out

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="/etc/attendance-web.conf")
    args = parser.parse_args()
    cfg = load_config(args.config)
    if not cfg.get("ATTENDANCE_WEB_PASSWORD"):
        raise SystemExit("ERROR: ATTENDANCE_WEB_PASSWORD is empty; configure /etc/attendance-web.conf")
    host = cfg.get("ATTENDANCE_WEB_HOST", "0.0.0.0")
    port = int(cfg.get("ATTENDANCE_WEB_PORT", "8080"))
    server = ThreadingHTTPServer((host, port), Handler)
    server.cfg = cfg
    print("attendance-web listening on %s:%d" % (host, port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
