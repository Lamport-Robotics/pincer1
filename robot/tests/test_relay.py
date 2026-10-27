"""The beacon relay (beacon/base/relay.py): command endpoint auth and the second replay check."""

import importlib.util
import json
import os
import sys
import threading
import urllib.error
import urllib.request
from http.server import HTTPServer
from pathlib import Path

import pytest

RELAY = Path(__file__).resolve().parents[2] / "beacon" / "base" / "relay.py"


@pytest.fixture
def relay(tmp_path, monkeypatch):  # noqa: ANN001, ANN201
    monkeypatch.setenv("RELAY_STATE", str(tmp_path / "state.json"))
    monkeypatch.setenv("RELAY_TOKEN", "t0k3n")
    monkeypatch.delenv("NTFY_URL", raising=False)
    monkeypatch.delenv("TELEGRAM_BOT_TOKEN", raising=False)
    monkeypatch.delenv("WEBHOOK_URL", raising=False)
    spec = importlib.util.spec_from_file_location("pincer_relay_under_test", RELAY)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)

    class FakeSerial:
        def __init__(self) -> None:
            self.lines: list[bytes] = []

        def write(self, b: bytes) -> None:
            self.lines.append(b)

    mod.BASE.ser = FakeSerial()
    yield mod
    sys.modules.pop(spec.name, None)


@pytest.fixture
def server(relay):  # noqa: ANN001, ANN201
    srv = HTTPServer(("127.0.0.1", 0), relay.Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    yield f"http://127.0.0.1:{srv.server_port}"
    srv.shutdown()


def _post(url: str, body: dict, token: str | None) -> tuple[int, dict]:
    headers = {"Content-Type": "application/json"}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(
        url + "/cmd", data=json.dumps(body).encode(), headers=headers, method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.load(e)


def test_cmd_needs_the_bearer_token(relay, server) -> None:  # noqa: ANN001
    assert _post(server, {"cmd": "disarm"}, None)[0] == 401
    assert _post(server, {"cmd": "disarm"}, "wrong")[0] == 401
    assert _post(server, {"cmd": "disarm"}, "t0k3n ")[0] == 200  # trailing blank tolerated
    assert relay.BASE.ser.lines == [b'{"cmd": "disarm"}\n']
    code, res = _post(server, {"cmd": "selfdestruct"}, "t0k3n")
    assert code == 400 and res["error"] == "unknown command"
    assert _post(server, {"cmd": "ping", "repeat_s": -5}, "t0k3n")[0] == 400
    assert _post(server, {"cmd": "ping", "repeat_s": True}, "t0k3n")[0] == 400
    assert _post(server, ["cmd"], "t0k3n")[0] == 400
    assert _post(server, {"cmd": "ping", "repeat_s": 60}, "t0k3n")[0] == 200
    assert relay.BASE.ser.lines[-1] == b'{"cmd": "ping", "repeat_s": 60}\n'
    # status needs nothing
    with urllib.request.urlopen(server + "/status", timeout=5) as r:
        assert r.status == 200


def test_no_token_means_no_command_endpoint(relay, server) -> None:  # noqa: ANN001
    relay.CFG["token"] = ""
    code, res = _post(server, {"cmd": "disarm"}, "anything")
    assert code == 403 and "RELAY_TOKEN" in res["error"]
    assert relay.BASE.ser.lines == []


def test_cli_send_refuses_without_token(relay, monkeypatch, capsys) -> None:  # noqa: ANN001
    relay.CFG["token"] = ""
    monkeypatch.setattr(sys, "argv", ["relay.py", "send", "disarm"])
    assert relay.main() == 2
    assert "RELAY_TOKEN" in capsys.readouterr().err


def test_replayed_lines_are_dropped_across_restarts(relay, tmp_path) -> None:  # noqa: ANN001
    hb = {"t": "heartbeat", "id": "1011121314151617", "seq": 5, "state": "armed"}
    relay.handle_event(hb)
    assert relay.STATE.d["last_seq"]["1011121314151617"] == 5
    first = relay.STATE.d["last_heartbeat"]
    relay.STATE.d["last_heartbeat"] = 0
    relay.handle_event(dict(hb))  # same seq again: dropped, nothing updated
    assert relay.STATE.d["last_heartbeat"] == 0
    relay.handle_event(dict(hb, seq=4))  # older
    assert relay.STATE.d["last_heartbeat"] == 0
    relay.handle_event(dict(hb, seq="x"))  # garbage seq
    assert relay.STATE.d["last_heartbeat"] == 0
    relay.handle_event(dict(hb, seq=6))
    assert relay.STATE.d["last_heartbeat"] >= first
    # the counter survives in the state file, so a restarted relay still drops seq 6
    saved = json.loads((tmp_path / "state.json").read_text())
    assert saved["last_seq"]["1011121314151617"] == 6
    fresh = relay.State(str(tmp_path / "state.json"))
    assert fresh.d["last_seq"]["1011121314151617"] == 6
    assert os.path.exists(tmp_path / "state.json")
