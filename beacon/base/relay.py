#!/usr/bin/env python3
"""Pincer1 beacon relay: base station on USB → ntfy / Telegram / webhook, and commands back.

Reads the JSON lines the base station prints (base/src/main.cpp), forwards alarms as urgent
notifications, keeps a small state file for replay checks and silence detection, and lets you
send `arm` / `disarm` / `ping` / `ok` to the beacon: over a local HTTP endpoint, from the command
line, or (opt-in) as Telegram commands from your own chat.

Python 3.10+, stdlib + pyserial.

    python3 relay.py                      run (reads the environment below)
    python3 relay.py send disarm          send a command through a running relay
    python3 relay.py status               ask the running relay for its state

Environment:
    RELAY_PORT            serial port of the base (default /dev/ttyUSB0; /dev/ttyACM0 on the Heltec V3)
    RELAY_BAUD            115200
    RELAY_HTTP            127.0.0.1:7410   local control endpoint (POST /cmd {"cmd":"disarm"}, GET /status)
    RELAY_STATE           relay-state.json next to this file
    RELAY_SILENT_AFTER_S  7200: notify once when an armed beacon's heartbeats stop for this long
    RELAY_ALARM_EVERY_S   600: re-notify an ongoing alarm at most this often (sooner if it moved > 200 m)
    NTFY_URL              e.g. https://ntfy.sh/<your-private-topic>   (urgent priority for alarms)
    NTFY_TOKEN            optional bearer token
    TELEGRAM_BOT_TOKEN    bot token; TELEGRAM_CHAT_ID the chat to notify
    TELEGRAM_COMMANDS     1 = poll the bot for /arm /disarm /ping /ok /status from TELEGRAM_CHAT_ID only
    WEBHOOK_URL           POST every event there as JSON ({"event": …, "text": …})

Nothing is sent anywhere unless the matching variables are set. The relay never needs internet
for the beacon to work; it needs it to reach you.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = {
    "port": os.environ.get("RELAY_PORT", "/dev/ttyUSB0"),
    "baud": int(os.environ.get("RELAY_BAUD", "115200")),
    "http": os.environ.get("RELAY_HTTP", "127.0.0.1:7410"),
    "state": os.environ.get("RELAY_STATE", os.path.join(HERE, "relay-state.json")),
    "silent_after": int(os.environ.get("RELAY_SILENT_AFTER_S", "7200")),
    "alarm_every": int(os.environ.get("RELAY_ALARM_EVERY_S", "600")),
    "ntfy": os.environ.get("NTFY_URL", ""),
    "ntfy_token": os.environ.get("NTFY_TOKEN", ""),
    "tg_token": os.environ.get("TELEGRAM_BOT_TOKEN", ""),
    "tg_chat": os.environ.get("TELEGRAM_CHAT_ID", ""),
    "tg_commands": os.environ.get("TELEGRAM_COMMANDS", "0") == "1",
    "webhook": os.environ.get("WEBHOOK_URL", ""),
}
COMMANDS = ("arm", "disarm", "ping", "ok")


def log(msg: str) -> None:
    print(time.strftime("%Y-%m-%d %H:%M:%S ") + msg, flush=True)


# ---------------------------------------------------------------- notifications

def _post(url: str, data: bytes, headers: dict[str, str], timeout: float = 15.0) -> None:
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            r.read()
    except (urllib.error.URLError, OSError) as e:
        log(f"notify: {url.split('/')[2]} failed: {e}")


def notify(title: str, text: str, urgent: bool = False, event: dict | None = None) -> None:
    log(f"notify{' URGENT' if urgent else ''}: {title}: {text}")
    if CFG["ntfy"]:
        headers = {
            "Title": title,
            "Priority": "urgent" if urgent else "default",
            "Tags": "rotating_light" if urgent else "robot",
        }
        if CFG["ntfy_token"]:
            headers["Authorization"] = "Bearer " + CFG["ntfy_token"]
        threading.Thread(target=_post, args=(CFG["ntfy"], text.encode(), headers), daemon=True).start()
    if CFG["tg_token"] and CFG["tg_chat"]:
        body = json.dumps({"chat_id": CFG["tg_chat"], "text": f"{title}\n{text}", "disable_notification": not urgent}).encode()
        threading.Thread(
            target=_post,
            args=(f"https://api.telegram.org/bot{CFG['tg_token']}/sendMessage", body, {"Content-Type": "application/json"}),
            daemon=True,
        ).start()
    if CFG["webhook"]:
        body = json.dumps({"title": title, "text": text, "urgent": urgent, "event": event or {}}).encode()
        threading.Thread(target=_post, args=(CFG["webhook"], body, {"Content-Type": "application/json"}), daemon=True).start()


# ---------------------------------------------------------------- state

class State:
    def __init__(self, path: str):
        self.path = path
        self.lock = threading.Lock()
        self.d: dict = {"last_seq": {}, "last_heartbeat": 0, "last_alarm_notify": 0, "last_pos": None,
                        "beacon_state": "unknown", "silent_notified": False, "last_packet": None}
        try:
            with open(path, encoding="utf-8") as f:
                self.d.update(json.load(f))
        except (OSError, ValueError):
            pass

    def save(self) -> None:
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.d, f, indent=1)
        os.replace(tmp, self.path)


STATE = State(CFG["state"])


def distance_m(a: tuple[float, float], b: tuple[float, float]) -> float:
    r = 6371000.0
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    dp, dl = math.radians(b[0] - a[0]), math.radians(b[1] - a[1])
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(h))


def describe(ev: dict) -> str:
    parts = []
    if ev.get("lat") is not None:
        age = ev.get("fix_age_s")
        parts.append(f"{ev['lat']:.5f},{ev['lon']:.5f}" + (f" (fix {age} s old)" if age else ""))
        parts.append(f"https://www.openstreetmap.org/?mlat={ev['lat']:.6f}&mlon={ev['lon']:.6f}#map=17/{ev['lat']:.6f}/{ev['lon']:.6f}")
    else:
        parts.append("no position yet")
    if ev.get("trig"):
        parts.append("trigger: " + ", ".join(ev["trig"]))
    if ev.get("battery_mv"):
        parts.append(f"battery {ev['battery_mv'] / 1000:.2f} V")
    if ev.get("hdop") is not None:
        parts.append(f"hdop {ev['hdop']}")
    parts.append(f"rssi {ev.get('rssi')} dBm, Find My key #{ev.get('findmy_idx')}")
    return "\n".join(parts)


# ---------------------------------------------------------------- serial side

class Base:
    def __init__(self):
        self.ser = None
        self.lock = threading.Lock()

    def open(self) -> None:
        while self.ser is None:
            try:
                self.ser = serial.Serial(CFG["port"], CFG["baud"], timeout=1)
                log(f"serial: {CFG['port']} open")
            except (serial.SerialException, OSError) as e:
                log(f"serial: {CFG['port']}: {e}; retrying in 5 s")
                time.sleep(5)

    def send(self, cmd: str, repeat_s: int | None = None) -> dict:
        if cmd not in COMMANDS:
            return {"ok": False, "error": "unknown command"}
        line = {"cmd": cmd}
        if repeat_s:
            line["repeat_s"] = int(repeat_s)
        with self.lock:
            if self.ser is None:
                return {"ok": False, "error": "serial not open"}
            self.ser.write((json.dumps(line) + "\n").encode())
        log(f"command → base: {line}")
        return {"ok": True, "sent": line}

    def status_line(self) -> None:
        with self.lock:
            if self.ser:
                self.ser.write(b"status\n")


BASE = Base()


def handle_event(ev: dict) -> None:
    t = ev.get("t")
    now = time.time()
    with STATE.lock:
        STATE.d["last_packet"] = ev
        if t in ("alarm", "heartbeat", "ack"):
            # replay check (the base already does this; belt and braces across base reboots)
            key = ev.get("id", "?")
            last = STATE.d["last_seq"].get(key, 0)
            seq = int(ev.get("seq", 0))
            if seq <= last:
                log(f"drop: seq {seq} ≤ {last} for {key}")
                return
            STATE.d["last_seq"][key] = seq
            STATE.d["beacon_state"] = ev.get("state", "unknown")

        if t == "heartbeat":
            STATE.d["last_heartbeat"] = now
            STATE.d["silent_notified"] = False
            log(f"heartbeat: {describe(ev).replace(chr(10), ' | ')}")
        elif t == "alarm":
            pos = (ev["lat"], ev["lon"]) if ev.get("lat") is not None else None
            moved = pos is not None and STATE.d["last_pos"] is not None and distance_m(pos, tuple(STATE.d["last_pos"])) > 200
            first = STATE.d["last_alarm_notify"] == 0 or now - STATE.d["last_alarm_notify"] > 6 * 3600
            due = now - STATE.d["last_alarm_notify"] >= CFG["alarm_every"]
            if first or moved or due:
                STATE.d["last_alarm_notify"] = now
                notify("Pincer1 ALARM" if first else "Pincer1 alarm update", describe(ev), urgent=True, event=ev)
            else:
                log(f"alarm (not re-notified): {describe(ev).replace(chr(10), ' | ')}")
            if pos is not None:
                STATE.d["last_pos"] = list(pos)
        elif t == "ack":
            STATE.d["last_alarm_notify"] = 0 if ev.get("state") != "alarm" else STATE.d["last_alarm_notify"]
            notify("Pincer1 beacon", f"acknowledged {ev.get('cmd')}: state {ev.get('state')}, battery {ev.get('battery_mv', 0) / 1000:.2f} V", event=ev)
        elif t == "reject":
            log(f"base rejected a packet: {ev}")
        elif t in ("sent", "sending", "gave_up", "stopped", "base", "error"):
            log(f"base: {ev}")
            if t == "gave_up":
                notify("Pincer1 beacon", f"command {ev.get('cmd')} not acknowledged (gave up after {ev.get('tries')} tries)")
        STATE.save()


def serial_loop() -> None:
    BASE.open()
    while True:
        try:
            raw = BASE.ser.readline()
        except (serial.SerialException, OSError) as e:
            log(f"serial: {e}; reopening")
            with BASE.lock:
                BASE.ser = None
            BASE.open()
            continue
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if not line:
            continue
        if not line.startswith("{"):
            log(f"base: {line}")
            continue
        try:
            ev = json.loads(line)
        except ValueError:
            log(f"base (bad json): {line}")
            continue
        handle_event(ev)


def silence_watch() -> None:
    while True:
        time.sleep(60)
        with STATE.lock:
            armed = STATE.d.get("beacon_state") == "armed"
            last = STATE.d.get("last_heartbeat", 0)
            quiet = last and time.time() - last > CFG["silent_after"]
            if armed and quiet and not STATE.d.get("silent_notified"):
                STATE.d["silent_notified"] = True
                STATE.save()
                notify("Pincer1 beacon silent", f"no heartbeat for {int((time.time() - last) / 60)} min while armed (out of range, cell dead, or base down)")


# ---------------------------------------------------------------- control: HTTP + CLI + Telegram

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # quiet
        pass

    def _json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/status":
            with STATE.lock:
                self._json(200, {"beacon_state": STATE.d.get("beacon_state"), "last_heartbeat": STATE.d.get("last_heartbeat"),
                                 "last_packet": STATE.d.get("last_packet"), "last_pos": STATE.d.get("last_pos")})
        else:
            self._json(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/cmd":
            return self._json(404, {"error": "not found"})
        n = int(self.headers.get("Content-Length", "0"))
        try:
            req = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self._json(400, {"error": "bad json"})
        res = BASE.send(str(req.get("cmd", "")), req.get("repeat_s"))
        self._json(200 if res.get("ok") else 400, res)


def http_loop() -> None:
    host, _, port = CFG["http"].rpartition(":")
    srv = HTTPServer((host or "127.0.0.1", int(port)), Handler)
    log(f"control: http://{CFG['http']}  (POST /cmd, GET /status)")
    srv.serve_forever()


def telegram_loop() -> None:
    """Poll the bot for commands from the configured chat only. Opt-in (TELEGRAM_COMMANDS=1)."""
    offset = 0
    api = f"https://api.telegram.org/bot{CFG['tg_token']}"
    log("telegram: command polling on")
    while True:
        try:
            with urllib.request.urlopen(f"{api}/getUpdates?timeout=30&offset={offset}", timeout=40) as r:
                upd = json.load(r)
        except (urllib.error.URLError, OSError, ValueError) as e:
            log(f"telegram: {e}")
            time.sleep(10)
            continue
        for u in upd.get("result", []):
            offset = u["update_id"] + 1
            msg = u.get("message") or {}
            if str(msg.get("chat", {}).get("id")) != str(CFG["tg_chat"]):
                continue  # someone else's chat: ignored
            text = (msg.get("text") or "").strip().lstrip("/").split("@")[0].lower()
            if text in COMMANDS:
                res = BASE.send(text)
                notify("Pincer1 relay", f"{text}: {'queued for the beacon' if res.get('ok') else res.get('error')}")
            elif text == "status":
                with STATE.lock:
                    lp = STATE.d.get("last_packet") or {}
                notify("Pincer1 relay", f"beacon {STATE.d.get('beacon_state')}, last packet: {lp.get('t')} at {lp.get('ms')} ms, " + describe(lp) if lp else "no packet yet")


# ---------------------------------------------------------------- entry

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode")
    s = sub.add_parser("send", help="send a command through the running relay")
    s.add_argument("cmd", choices=COMMANDS)
    s.add_argument("--repeat-s", type=int, default=None, help="keep sending this long (base default 1200)")
    sub.add_parser("status", help="print the running relay's state")
    args = ap.parse_args()

    if args.mode in ("send", "status"):
        url = f"http://{CFG['http']}/" + ("cmd" if args.mode == "send" else "status")
        try:
            if args.mode == "send":
                body = json.dumps({"cmd": args.cmd, "repeat_s": args.repeat_s}).encode()
                req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"}, method="POST")
            else:
                req = urllib.request.Request(url)
            with urllib.request.urlopen(req, timeout=5) as r:
                print(r.read().decode())
            return 0
        except (urllib.error.URLError, OSError) as e:
            print(f"relay not reachable at {url}: {e}", file=sys.stderr)
            return 1

    if serial is None:
        print("pyserial is required: pip install pyserial", file=sys.stderr)
        return 2
    if sys.version_info < (3, 10):
        print("Python 3.10+ required", file=sys.stderr)
        return 2
    log(f"pincer1 relay starting; notifications: ntfy={'on' if CFG['ntfy'] else 'off'} telegram={'on' if CFG['tg_token'] and CFG['tg_chat'] else 'off'} webhook={'on' if CFG['webhook'] else 'off'}")
    threading.Thread(target=http_loop, daemon=True).start()
    threading.Thread(target=silence_watch, daemon=True).start()
    if CFG["tg_commands"] and CFG["tg_token"] and CFG["tg_chat"]:
        threading.Thread(target=telegram_loop, daemon=True).start()
    serial_loop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
