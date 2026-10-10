import asyncio
import json
from collections.abc import AsyncIterator, Awaitable, Callable
from typing import Any

import pytest
from websockets.asyncio.client import ClientConnection, connect

from pincer_robot.config import FeatureFlags
from pincer_robot.nav import FindResult, NavResult
from pincer_robot.presence.fusion import PresenceState
from pincer_robot.service import RobotService, report_item_text, wake_urgency


class FakeHandlers:
    def __init__(self, passphrase_ok: bool = True) -> None:
        self.spoken: list[str] = []
        self.urgencies: list[float] = []
        self.attention_calls = 0
        self.passphrase_ok = passphrase_ok
        self.say_delay = 0.0
        self.chat_calls: list[tuple[str, dict[str, Any]]] = []
        self.chat_error = ""
        # feature hooks
        self.settings_calls: list[tuple[FeatureFlags, list[str]]] = []
        self.enroll_ok = True
        self.network_calls: list[tuple[bool, str, float]] = []
        self.approach_calls = 0
        self.visible = True
        self.find_result: FindResult | None = None
        self.find_calls: list[float] = []
        self.goto_calls: list[str] = []
        self.named: list[tuple[str, float | None, Any]] = []
        self.map_calls: list[tuple[str, str | None]] = []
        self.dock_calls: list[str] = []
        self.dock_state = "idle"
        self.leave_calls: list[str] = []
        self.leave_result = False

    async def say(self, text: str, urgency: float = 0.0) -> None:
        self.spoken.append(text)
        self.urgencies.append(urgency)
        if self.say_delay:
            await asyncio.sleep(self.say_delay)

    async def apply_settings(self, flags: FeatureFlags, changed: list[str]) -> None:
        self.settings_calls.append((FeatureFlags(**vars(flags)), list(changed)))

    async def enroll_room(self) -> bool:
        return self.enroll_ok

    async def network_override(self, on: bool, reason: str, for_secs: float) -> bool:
        self.network_calls.append((on, reason, for_secs))
        return True

    async def wake_approach(self) -> None:
        self.approach_calls += 1

    def owner_visible(self) -> bool:
        return self.visible

    async def find_owner(self, max_secs: float) -> FindResult | None:
        self.find_calls.append(max_secs)
        if self.find_result is not None:
            self.visible = self.find_result.found
        return self.find_result

    async def goto(self, room: str) -> NavResult:
        self.goto_calls.append(room)
        return NavResult(room == "bedroom", "arrived" if room == "bedroom" else "unknown_room", 4.2)

    async def name_room(self, name: str, radius: float | None, polygon: Any) -> dict[str, Any]:
        self.named.append((name, radius, polygon))
        return {"name": name, "x": 1.0, "y": 2.0, "rooms": [name]}

    async def map_action(self, action: str, name: str | None) -> dict[str, Any]:
        self.map_calls.append((action, name))
        return {"ok": True, "map": action == "save", "rooms": []}

    async def dock_action(self, action: str) -> dict[str, Any]:
        self.dock_calls.append(action)
        if action == "go" and self.dock_state == "off":
            return {"ok": False, "reason": "dock_off", "state": "off"}
        return {"ok": True, "state": self.dock_state, "enabled": self.dock_state != "off"}

    async def leave_dock_for(self, reason: str) -> bool:
        self.leave_calls.append(reason)
        return self.leave_result

    async def ask_passphrase(self) -> bool:
        return self.passphrase_ok

    async def attention(self) -> None:
        self.attention_calls += 1

    async def chat(self, text: str, context: dict[str, Any]) -> str:
        self.chat_calls.append((text, dict(context)))
        if self.chat_error:
            raise RuntimeError(self.chat_error)
        return f"re: {text}"


def _state(**kw: Any) -> PresenceState:
    base = dict(
        person=False, owner=False, voice_ok=False, duress=False, moved=False,
        lidar_conf=0.0, face_conf=0.0, ts=1000.0,
    )  # fmt: skip
    base.update(kw)
    return PresenceState(**base)


async def _recv(ws: ClientConnection, timeout: float = 2.0) -> dict[str, Any]:
    raw = await asyncio.wait_for(ws.recv(), timeout)
    text = raw.decode() if isinstance(raw, bytes) else raw
    return json.loads(text)


async def _recv_type(ws: ClientConnection, t: str, timeout: float = 2.0) -> dict[str, Any]:
    deadline = asyncio.get_running_loop().time() + timeout
    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            raise TimeoutError(f"no {t} message")
        msg = await _recv(ws, remaining)
        if msg.get("t") == t:
            return msg


def run(coro: Callable[[], Awaitable[None]]) -> None:
    asyncio.run(coro())


async def _server(
    handlers: FakeHandlers, heartbeat_s: float = 2.0, flags: FeatureFlags | None = None
) -> AsyncIterator[tuple[RobotService, str]]:
    svc = RobotService(
        handlers, port=0, heartbeat_s=heartbeat_s, report_pause_s=0.01, flags=flags,
        wake_pause_s=0.1,
    )  # fmt: skip
    await svc.start()
    try:
        yield svc, f"ws://127.0.0.1:{svc.bound_port}"
    finally:
        await svc.stop()


def test_presence_sent_on_connect_and_on_change() -> None:
    async def body() -> None:
        async for svc, url in _server(FakeHandlers()):
            svc.publish(_state(person=True, lidar_conf=0.91))
            async with connect(url) as ws:
                msg = await _recv(ws)
                assert msg["t"] == "presence"
                assert msg["person"] is True and msg["owner"] is False
                assert msg["confidence"] == {"lidar": 0.91, "face": 0.0}
                assert set(msg) == {
                    "t", "person", "owner", "voice_ok", "duress", "moved", "confidence", "ts"
                }  # fmt: skip
                svc.publish(_state(person=True, owner=True, face_conf=0.88))
                msg = await _recv(ws)
                assert msg["owner"] is True and msg["confidence"]["face"] == 0.88
                # Confidence-only change is not pushed immediately...
                svc.publish(_state(person=True, owner=True, face_conf=0.5))
                with pytest.raises(TimeoutError):
                    await _recv(ws, 0.3)

    run(body)


def test_heartbeat_every_interval() -> None:
    async def body() -> None:
        async for svc, url in _server(FakeHandlers(), heartbeat_s=0.2):
            svc.publish(_state())
            async with connect(url) as ws:
                await _recv(ws)
                t0 = asyncio.get_running_loop().time()
                a = await _recv(ws, 1.0)
                b = await _recv(ws, 1.0)
                assert a["t"] == b["t"] == "presence"
                assert asyncio.get_running_loop().time() - t0 < 0.9

    run(body)


def test_say_speaks_and_acks() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for _svc, url in _server(h):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "say", "id": "x1", "text": "0.5 SOL out."}) + "\n")
                msg = await _recv_type(ws, "said")
                assert msg == {"t": "said", "id": "x1"}
                assert h.spoken == ["0.5 SOL out."]

    run(body)


def test_ask_passphrase_replies_ok_and_not_ok() -> None:
    async def body() -> None:
        for ok in (True, False):
            async for _svc, url in _server(FakeHandlers(passphrase_ok=ok)):
                async with connect(url) as ws:
                    await ws.send(json.dumps({"t": "ask_passphrase", "id": "x2"}))
                    msg = await _recv_type(ws, "passphrase")
                    assert msg == {"t": "passphrase", "id": "x2", "ok": ok}

    run(body)


def test_attention_and_morning_report() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for _svc, url in _server(h):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "attention"}))
                items = ["SOL is up 3 percent.", {"title": "Agent", "text": "Closed two trades."}]
                await ws.send(json.dumps({"t": "morning_report", "id": "r1", "items": items}))
                msg = await _recv_type(ws, "done")
                assert msg["id"] == "r1"
                assert h.attention_calls == 1
                assert h.spoken == [
                    "Good morning. 2 items.",
                    "SOL is up 3 percent.",
                    "Agent. Closed two trades.",
                ]

    run(body)


def test_brief_is_phrased_by_the_brain_or_read_plainly() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for _svc, url in _server(h):
            async with connect(url) as ws:
                lines = [
                    "Good morning.",
                    "SOL is at $187.12 up 4.5 percent on the day.",
                    "My battery is at 18 percent.",
                ]
                await ws.send(
                    json.dumps(
                        {
                            "t": "brief",
                            "id": "b1",
                            "facts": {"notes": ["decrypt.co unreachable"]},
                            "lines": lines,
                        }
                    )
                )
                msg = await _recv_type(ws, "done")
                assert msg["id"] == "b1"
                # the brain got the facts and the rules; its number-preserving reply was spoken once
                assert len(h.chat_calls) == 1
                prompt = h.chat_calls[0][0]
                assert "SOL is at $187.12" in prompt and "decrypt.co unreachable" in prompt
                assert "Do not add facts" in prompt
                assert h.spoken == [f"re: {prompt}"]
                # no brain: the plain lines are read one by one
                h.spoken.clear()
                h.chat_error = "no_brain"
                await ws.send(json.dumps({"t": "brief", "id": "b2", "lines": lines}))
                await _recv_type(ws, "done")
                assert h.spoken == lines

    run(body)


def test_brief_rejects_a_rewrite_that_drops_numbers() -> None:
    from pincer_robot.service import numbers_preserved

    lines = ["BTC is at $112,346 down 2.3 percent on the day.", "1 signed payment is waiting."]
    assert numbers_preserved(
        lines, "Bitcoin sits at 112346 dollars, off 2.3 percent; 1 payment waits."
    )
    assert not numbers_preserved(lines, "Bitcoin is down a bit and a payment is waiting.")


def test_speech_is_serialised_and_order_kept() -> None:
    async def body() -> None:
        h = FakeHandlers()
        h.say_delay = 0.05
        async for _svc, url in _server(h):
            async with connect(url) as ws:
                for i in range(3):
                    await ws.send(json.dumps({"t": "say", "id": f"s{i}", "text": f"line {i}"}))
                ids = [(await _recv_type(ws, "said"))["id"] for _ in range(3)]
                assert ids == ["s0", "s1", "s2"]
                assert h.spoken == ["line 0", "line 1", "line 2"]

    run(body)


def test_unknown_type_and_garbage_are_tolerated() -> None:
    async def body() -> None:
        async for _svc, url in _server(FakeHandlers()):
            async with connect(url) as ws:
                await ws.send("not json\n")
                await ws.send(json.dumps({"t": "dance", "id": "d1"}))
                msg = await _recv_type(ws, "error")
                assert msg["id"] == "d1" and msg["error"].startswith("unknown_type")

    run(body)


def test_daemon_reconnect_and_multiline_frames() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for svc, url in _server(h):
            svc.publish(_state(person=True))
            async with connect(url) as ws:
                await _recv(ws)
            # Disconnect mid-speech must not break the service.
            h.say_delay = 0.1
            async with connect(url) as ws:
                await _recv(ws)
                await ws.send(json.dumps({"t": "say", "id": "a", "text": "long"}))
            await asyncio.sleep(0.2)
            h.say_delay = 0.0
            async with connect(url) as ws:
                msg = await _recv(ws)
                assert msg["person"] is True
                two = (
                    json.dumps({"t": "say", "id": "b", "text": "one"})
                    + "\n"
                    + json.dumps({"t": "say", "id": "c", "text": "two"})
                    + "\n"
                )
                await ws.send(two)
                got = {(await _recv_type(ws, "said"))["id"] for _ in range(2)}
                assert got == {"b", "c"}
            assert h.spoken == ["long", "one", "two"]

    run(body)


def test_chat_replies_then_speaks_with_whitelisted_context() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for svc, url in _server(h):
            svc.publish(_state(person=True, owner=True, lidar_conf=0.9))
            async with connect(url) as ws:
                await _recv(ws)
                await ws.send(json.dumps({"t": "chat", "id": "c1", "text": "what did I miss?"}))
                msg = await _recv_type(ws, "chat")
                assert msg == {"t": "chat", "id": "c1", "text": "re: what did I miss?"}
                await asyncio.sleep(0.05)
                assert h.spoken == ["re: what did I miss?"]
                text, ctx = h.chat_calls[0]
                assert text == "what did I miss?"
                assert ctx["owner"] is True and ctx["person"] is True and ctx["duress"] is False
                assert "time" in ctx and "lidar_conf" not in ctx
                # Empty text and a brain failure both come back as errors, nothing spoken.
                await ws.send(json.dumps({"t": "chat", "id": "c2", "text": "  "}))
                assert (await _recv_type(ws, "error"))["error"] == "empty_text"
                h.chat_error = "no_brain: set PINCER_BRAIN=anthropic or local"
                await ws.send(json.dumps({"t": "chat", "id": "c3", "text": "hi"}))
                err = await _recv_type(ws, "error")
                assert err["id"] == "c3" and err["error"].startswith("no_brain")
                assert h.spoken == ["re: what did I miss?"]

    run(body)


def test_report_item_text_shapes() -> None:
    assert report_item_text("plain") == "plain"
    assert report_item_text({"text": "body"}) == "body"
    assert report_item_text({"title": "T", "summary": "S"}) == "T. S"
    assert report_item_text({"title": "only"}) == "only"
    assert report_item_text({"x": 1}) == '{"x": 1}'


# --- owner-optional features -------------------------------------------------------------------


def test_settings_message_updates_flags_applies_and_acks() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for svc, url in _server(h):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "settings", "id": "s1", "airgap": "night",
                                          "wake": True}))  # fmt: skip
                msg = await _recv_type(ws, "settings")
                assert msg == {"t": "settings", "room_lock": False, "two_person": False,
                               "airgap": "night", "wake": True, "dock": False,
                               "mode": "online", "brain": "", "id": "s1"}  # fmt: skip
                assert svc.flags.airgap == "night" and svc.flags.wake is True
                flags, changed = h.settings_calls[-1]
                assert flags.airgap == "night" and changed == ["airgap", "wake"]
                await ws.send(json.dumps({"t": "settings", "airgap": "loose"}))
                err = await _recv_type(ws, "error")
                assert err["error"].startswith("bad_settings")
                assert svc.flags.airgap == "night"

    run(body)


def test_enroll_room_and_network_messages() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for _svc, url in _server(h):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "enroll_room", "id": "r1"}))
                assert await _recv_type(ws, "enroll_room") == {"t": "enroll_room", "id": "r1",
                                                               "ok": True}  # fmt: skip
                h.enroll_ok = False
                await ws.send(json.dumps({"t": "enroll_room", "id": "r2"}))
                assert (await _recv_type(ws, "enroll_room"))["ok"] is False
                await ws.send(json.dumps({"t": "network", "on": True, "reason": "agent_window",
                                          "for_secs": 600}))  # fmt: skip
                await ws.send(json.dumps({"t": "network", "id": "n2", "on": False}))
                msg = await _recv_type(ws, "network")
                assert msg == {"t": "network", "id": "n2", "applied": True}
                assert h.network_calls == [(True, "agent_window", 600.0), (False, "", 0.0)]

    run(body)


def test_wake_off_replies_immediately() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for _svc, url in _server(h):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "wake", "id": "w1", "reason": "PORTFOLIO_DROP",
                                          "lines": ["Wake up."], "max_secs": 5}))  # fmt: skip
                msg = await _recv_type(ws, "wake")
                assert msg == {"t": "wake", "id": "w1", "acked": False, "secs": 0,
                               "reason": "wake_off"}  # fmt: skip
                assert h.spoken == [] and h.approach_calls == 0

    run(body)


def test_wake_acked_by_touch_stops_loop_with_rising_urgency() -> None:
    async def body() -> None:
        h = FakeHandlers()
        h.say_delay = 0.02
        async for _svc, url in _server(h, flags=FeatureFlags(wake=True)):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "wake", "id": "w2", "reason": "PORTFOLIO_DROP",
                                          "lines": ["Wake up.", "SOL fell 20 percent."],
                                          "max_secs": 30}))  # fmt: skip
                for _ in range(100):  # let it repeat a few times (bounded wait)
                    if len(h.spoken) >= 3:
                        break
                    await asyncio.sleep(0.02)
                await ws.send(json.dumps({"t": "touch"}))
                msg = await _recv_type(ws, "wake")
                assert msg["id"] == "w2" and msg["acked"] is True and msg["reason"] == "touch"
                assert msg["secs"] <= 2 and msg["travelled"] is False and msg["room"] is None
                assert h.approach_calls == 1
                assert h.leave_calls == ["wake"]  # the alarm pulls it off the charger first
                assert h.spoken[:2] == ["Wake up.", "SOL fell 20 percent."]
                assert 3 <= len(h.spoken) < 12
                assert h.urgencies[0] == 0.0
                assert h.urgencies == sorted(h.urgencies)  # never gets calmer
                assert h.urgencies[-1] > h.urgencies[0]
                assert h.find_calls == []  # owner was visible: no navigation
            await asyncio.sleep(0.2)
            n = len(h.spoken)
            await asyncio.sleep(0.3)
            assert len(h.spoken) == n  # loop really stopped

    run(body)


def test_wake_times_out_and_passphrase_acks() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for svc, url in _server(h, flags=FeatureFlags(wake=True)):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "wake", "id": "w3", "lines": ["Up."],
                                          "max_secs": 1}))  # fmt: skip
                msg = await _recv_type(ws, "wake", timeout=3.0)
                assert msg["acked"] is False and msg["reason"] == "timeout" and msg["secs"] == 1
                assert len(h.spoken) >= 3 and max(h.urgencies) == 1.0
                h.spoken.clear()
                await ws.send(json.dumps({"t": "wake", "id": "w4", "lines": ["Up."],
                                          "max_secs": 20}))  # fmt: skip
                await asyncio.sleep(0.15)
                svc.ack_wake_threadsafe("passphrase")
                msg = await _recv_type(ws, "wake")
                assert msg["acked"] is True and msg["reason"] == "passphrase"
                # No lines: a default line built from the reason is spoken.
                await ws.send(json.dumps({"t": "wake", "id": "w5", "reason": "FIRE",
                                          "max_secs": 1}))  # fmt: skip
                await _recv_type(ws, "wake", timeout=3.0)
                assert any("FIRE" in line for line in h.spoken)

    run(body)


def test_wake_travels_to_owner_when_not_visible() -> None:
    async def body() -> None:
        h = FakeHandlers()
        h.visible = False
        h.find_result = FindResult(True, "bedroom", True, ["desk", "bedroom"], 12.0)
        async for _svc, url in _server(h, flags=FeatureFlags(wake=True)):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "wake", "id": "w6", "lines": ["Up."],
                                          "max_secs": 10}))  # fmt: skip
                await asyncio.sleep(0.2)
                await ws.send(json.dumps({"t": "touch"}))
                msg = await _recv_type(ws, "wake")
                assert msg["acked"] and msg["travelled"] is True and msg["room"] == "bedroom"
                assert h.find_calls == [6.0]  # 60 % of max_secs for the search
                assert h.approach_calls == 1 and h.spoken
                # Not found / no map: still speaks in place.
                h.visible = False
                h.find_result = None
                await ws.send(json.dumps({"t": "wake", "id": "w7", "lines": ["Up."],
                                          "max_secs": 1}))  # fmt: skip
                msg = await _recv_type(ws, "wake", timeout=3.0)
                assert msg["travelled"] is False and msg["room"] is None and not msg["acked"]

    run(body)


def test_wake_urgency_curve() -> None:
    assert wake_urgency(0, 6) == 0.0
    assert wake_urgency(5, 6) == 1.0
    assert wake_urgency(9, 6) == 1.0
    assert 0 < wake_urgency(2, 6) < wake_urgency(3, 6) < 1
    assert wake_urgency(0, 1) == 0.0 and wake_urgency(1, 1) == 1.0


def test_goto_name_room_and_map_messages() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for _svc, url in _server(h):
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "goto", "id": "g1", "room": "Bedroom"}))
                msg = await _recv_type(ws, "goto")
                assert msg == {"t": "goto", "id": "g1", "room": "bedroom", "ok": True,
                               "reason": "arrived", "secs": 4}  # fmt: skip
                await ws.send(json.dumps({"t": "goto", "id": "g2", "room": "attic"}))
                assert (await _recv_type(ws, "goto"))["reason"] == "unknown_room"
                await ws.send(json.dumps({"t": "goto", "id": "g3"}))
                assert (await _recv_type(ws, "error"))["error"] == "empty_room"
                await ws.send(json.dumps({"t": "name_room", "id": "n1", "name": "Desk",
                                          "radius": 1.5}))  # fmt: skip
                msg = await _recv_type(ws, "name_room")
                assert msg["ok"] is True and msg["name"] == "desk" and msg["rooms"] == ["desk"]
                assert h.named == [("desk", 1.5, None)]
                await ws.send(json.dumps({"t": "map", "id": "m1", "action": "save",
                                          "name": "flat"}))  # fmt: skip
                msg = await _recv_type(ws, "map")
                assert msg["action"] == "save" and msg["map"] is True and msg["ok"] is True
                await ws.send(json.dumps({"t": "map", "id": "m2", "action": "erase"}))
                assert (await _recv_type(ws, "error"))["error"] == "bad_action"
                assert h.map_calls == [("save", "flat")]

    run(body)


# --- charging dock ----------------------------------------------------------------------------


def test_dock_messages_settings_toggle_and_events() -> None:
    async def body() -> None:
        h = FakeHandlers()
        async for svc, url in _server(h):
            async with connect(url) as ws:
                # settings toggle carries the new key and reaches the handlers.
                await ws.send(json.dumps({"t": "settings", "id": "s1", "dock": True}))
                msg = await _recv_type(ws, "settings")
                assert msg["dock"] is True and svc.flags.dock is True
                assert h.settings_calls[-1][1] == ["dock"]
                await ws.send(json.dumps({"t": "settings", "dock": "yes"}))
                assert (await _recv_type(ws, "error"))["error"].startswith("bad_settings")
                # dock requests.
                h.dock_state = "charging"
                await ws.send(json.dumps({"t": "dock", "id": "d1", "action": "status"}))
                msg = await _recv_type(ws, "dock")
                assert msg == {"t": "dock", "id": "d1", "action": "status", "ok": True,
                               "state": "charging", "enabled": True}  # fmt: skip
                await ws.send(json.dumps({"t": "dock", "id": "d2", "action": "go"}))
                assert (await _recv_type(ws, "dock"))["action"] == "go"
                await ws.send(json.dumps({"t": "dock", "id": "d3", "action": "leave"}))
                assert (await _recv_type(ws, "dock"))["state"] == "charging"
                await ws.send(json.dumps({"t": "dock", "id": "d4", "action": "eject"}))
                assert (await _recv_type(ws, "error"))["error"] == "bad_action"
                assert h.dock_calls == ["status", "go", "leave"]
                h.dock_state = "off"
                await ws.send(json.dumps({"t": "dock", "id": "d5", "action": "go"}))
                msg = await _recv_type(ws, "dock")
                assert msg["ok"] is False and msg["reason"] == "dock_off"
                # events are broadcast to the connected daemon.
                svc.emit_event({"t": "event", "ev": "battery_low", "pct": 14, "dock": "off"})
                ev = await _recv_type(ws, "event")
                assert ev == {"t": "event", "ev": "battery_low", "pct": 14, "dock": "off"}
                svc.emit_event_threadsafe({"t": "event", "ev": "dock_failed", "reason": "x",
                                           "attempts": 3})  # fmt: skip
                assert (await _recv_type(ws, "event"))["ev"] == "dock_failed"
                # presence carries battery + dock when the state has them.
                svc.publish(_state(person=True, battery=(42, 11.52, False), dock="idle"))
                msg = await _recv_type(ws, "presence")
                assert msg["battery"] == {"pct": 42, "voltage": 11.52, "charging": False}
                assert msg["dock"] == "idle"
                svc.publish(_state(person=True, battery=(42, 11.52, True), dock="charging"))
                msg = await _recv_type(ws, "presence", timeout=0.5)  # pushed at once
                assert msg["battery"]["charging"] is True and msg["dock"] == "charging"
            svc.emit_event({"t": "event", "ev": "dock_failed"})  # no client: must not raise

    run(body)


def test_service_busy_tracks_wake_and_speech() -> None:
    async def body() -> None:
        h = FakeHandlers()
        h.say_delay = 0.1
        async for svc, url in _server(h, flags=FeatureFlags(wake=True)):
            assert svc.busy is False
            async with connect(url) as ws:
                await ws.send(json.dumps({"t": "say", "id": "a", "text": "hello"}))
                await asyncio.sleep(0.03)
                assert svc.busy is True
                await _recv_type(ws, "said")
                assert svc.busy is False
                await ws.send(json.dumps({"t": "wake", "id": "w", "lines": ["Up."],
                                          "max_secs": 5}))  # fmt: skip
                await asyncio.sleep(0.03)
                assert svc.busy is True and svc.wake_active
                await ws.send(json.dumps({"t": "touch"}))
                await _recv_type(ws, "wake")
                assert svc.busy is False

    run(body)
