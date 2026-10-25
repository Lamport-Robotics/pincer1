"""The daemon-facing websocket server (PROTOCOL.md section 2), ws://127.0.0.1:7402.

Robot -> daemon: ``presence`` pushed on every connect, on every change of the discrete
fields, and at least every ``heartbeat_s`` (2 s).

Daemon -> robot::

    {"t":"say","id":"x1","text":"..."}        -> {"t":"said","id":"x1"} when speech ends
    {"t":"ask_passphrase","id":"x2"}          -> {"t":"passphrase","id":"x2","ok":true}
    {"t":"attention"}                         (turn to owner, step forward; no reply)
    {"t":"morning_report","items":[...]}      -> items read one by one, {"t":"done","id"} if id
    {"t":"brief","id","facts":{...},"lines":[...]}
                                              -> morning briefing; the brain phrases the facts in
                                                 its voice (plain lines when no brain), {"t":"done"}
    {"t":"chat","id":"c1","text":"..."}       -> {"t":"chat","id":"c1","text":"<reply>"}, spoken
                                                 (optional; needs PINCER_BRAIN, else error)

Owner-optional features (all off by default, switched by ``settings``)::

    {"t":"settings","room_lock":b,"two_person":b,"airgap":"off|strict|night","wake":b}
                                              -> {"t":"settings",...current flags}
    {"t":"enroll_room","id"}                  -> {"t":"enroll_room","id","ok":true}
    {"t":"network","on":b,"reason":s,"for_secs":n}   (air-gap override; no reply)
    {"t":"touch"}                             (a touch relayed by the daemon; acks a wake)
    {"t":"wake","id","reason","lines":[...],"max_secs":120}
                                              -> {"t":"wake","id","acked":b,"secs":n,
                                                  "reason":"touch|passphrase|timeout|wake_off",
                                                  "travelled":b,"room":s|null}
    {"t":"goto","id","room":"bedroom"}        -> {"t":"goto","id","ok":b,"reason":s,"secs":n}
    {"t":"name_room","id","name":"bedroom","radius"?:m,"polygon"?:[[x,y],...]}
                                              -> {"t":"name_room","id","ok":b,...}
    {"t":"map","id","action":"start|save|status","name"?:"home"}
                                              -> {"t":"map","id","ok":b,...status}
    {"t":"dock","id","action":"go|leave|status"}
                                              -> {"t":"dock","id","action":s,"ok":b,"state":s,...}

Robot -> daemon events (unsolicited, additive)::

    {"t":"event","ev":"dock_failed","reason":s,"attempts":n}
    {"t":"event","ev":"battery_low","pct":n,"dock":s}

``presence`` carries ``battery`` and ``dock`` on every message once the app has a battery
monitor (see presence/fusion.py).

The acknowledgements and ``error`` replies are additive: a daemon that only knows the
four original requests can ignore them. Each connection is independent, so the daemon can
drop and reconnect at any time; speech already in progress finishes.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import re
import time
from collections.abc import Iterable, Mapping
from typing import Any, Protocol

from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from .config import FeatureFlags
from .presence.fusion import PresenceState

log = logging.getLogger("pincer.service")

WAKE_MIN_PAUSE_S = 0.3


class ServiceHandlers(Protocol):
    async def say(self, text: str, urgency: float = 0.0) -> None: ...

    async def ask_passphrase(self) -> bool: ...

    async def attention(self) -> None: ...

    async def chat(self, text: str, context: dict[str, Any]) -> str: ...

    # -- features (all may raise; the service turns that into an error reply) ------------
    async def apply_settings(self, flags: FeatureFlags, changed: list[str]) -> None: ...

    async def enroll_room(self) -> bool: ...

    async def network_override(self, on: bool, reason: str, for_secs: float) -> bool: ...

    async def wake_approach(self) -> None: ...

    def owner_visible(self) -> bool: ...

    async def find_owner(self, max_secs: float) -> Any: ...  # nav.FindResult | None

    async def goto(self, room: str) -> Any: ...  # nav.NavResult

    async def name_room(self, name: str, radius: float | None, polygon: Any) -> dict[str, Any]: ...

    async def map_action(self, action: str, name: str | None) -> dict[str, Any]: ...

    async def dock_action(self, action: str) -> dict[str, Any]: ...

    async def leave_dock_for(self, reason: str) -> bool: ...


def brief_prompt(lines: list[str], facts: Mapping[str, Any]) -> str:
    """What the brain is asked. Facts only; the brain styles, it does not invent."""
    notes = [str(n) for n in (facts.get("notes") or [])]
    body = "\n".join(f"- {x}" for x in lines)
    extra = ("\nGaps you may mention briefly: " + "; ".join(notes)) if notes else ""
    return (
        "Read your owner this morning briefing out loud, in your own voice, as one short spoken "
        "piece under 150 words. Keep every number and every name exactly as written. Do not add "
        "facts, prices, advice or opinions. Skip nothing that is a warning (battery, inheritance, "
        "waiting payments).\n"
        f"{body}{extra}"
    )


_NUM = re.compile(r"\d[\d,]*(?:\.\d+)?")


def numbers_preserved(lines: list[str], reply: str) -> bool:
    """Every number the daemon stated must appear in the brain's rewrite (commas ignored)."""
    want = {m.replace(",", "") for line in lines for m in _NUM.findall(line)}
    have = {m.replace(",", "") for m in _NUM.findall(reply)}
    return want <= have


def report_item_text(item: Any) -> str:
    """Morning-report items may be strings or small objects; read what is readable."""
    if isinstance(item, str):
        return item
    if isinstance(item, dict):
        for key in ("text", "summary", "body", "line"):
            if isinstance(item.get(key), str):
                title = item.get("title")
                body = item[key]
                return f"{title}. {body}" if isinstance(title, str) and title else body
        if isinstance(item.get("title"), str):
            return item["title"]
    return json.dumps(item, ensure_ascii=False)


def wake_urgency(step: int, total_steps: int) -> float:
    """0 for the first line, 1 by the end of the third pass (then stays at 1)."""
    if total_steps <= 1:
        return 1.0 if step > 0 else 0.0
    return min(1.0, step / (total_steps - 1))


class RobotService:
    def __init__(
        self,
        handlers: ServiceHandlers,
        *,
        host: str = "127.0.0.1",
        port: int = 7402,
        heartbeat_s: float = 2.0,
        report_pause_s: float = 0.7,
        flags: FeatureFlags | None = None,
        wake_pause_s: float = 2.0,
        wake_max_secs: int = 120,
    ) -> None:
        self.handlers = handlers
        self.host = host
        self.port = port
        self.heartbeat_s = heartbeat_s
        self.report_pause_s = report_pause_s
        self.flags = flags or FeatureFlags()
        self.wake_pause_s = wake_pause_s
        self.wake_max_secs = wake_max_secs
        self._state: PresenceState | None = None
        self._changed = asyncio.Event()
        self._clients: set[ServerConnection] = set()
        self._server: Server | None = None
        self._broadcast_task: asyncio.Task[None] | None = None
        self._speech_lock = asyncio.Lock()
        self._tasks: set[asyncio.Task[None]] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._wake_ack: asyncio.Event | None = None
        self._wake_ack_by: str | None = None
        self.wake_active = False

    # -- lifecycle ----------------------------------------------------------------------
    async def start(self) -> None:
        self._loop = asyncio.get_running_loop()
        # origins=[None]: only clients that send no Origin header, i.e. programs, never a browser
        # tab on this machine (a page could otherwise drive say / settings / dock from JavaScript)
        self._server = await serve(
            self._handle, self.host, self.port, max_size=1 << 20, origins=[None]
        )
        self._broadcast_task = asyncio.create_task(self._broadcast_loop(), name="presence-push")
        log.info("robot service listening on ws://%s:%d", self.host, self.port)

    async def stop(self) -> None:
        if self._broadcast_task is not None:
            self._broadcast_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._broadcast_task
        for t in list(self._tasks):
            t.cancel()
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
        self._clients.clear()

    @property
    def bound_port(self) -> int:
        assert self._server is not None
        sock = next(iter(self._server.sockets))
        return int(sock.getsockname()[1])

    # -- presence ----------------------------------------------------------------------
    def publish(self, state: PresenceState) -> None:
        """Set the latest presence; pushes immediately if the discrete fields changed."""
        prev = self._state
        self._state = state
        if prev is None or prev.signature() != state.signature():
            self._changed.set()

    def publish_threadsafe(self, state: PresenceState) -> None:
        if self._loop is None:
            self.publish(state)
        else:
            self._loop.call_soon_threadsafe(self.publish, state)

    @property
    def state(self) -> PresenceState | None:
        return self._state

    async def _broadcast_loop(self) -> None:
        while True:
            try:
                await asyncio.wait_for(self._changed.wait(), timeout=self.heartbeat_s)
            except (TimeoutError, asyncio.TimeoutError):  # asyncio's is distinct on 3.10
                pass
            self._changed.clear()
            await self._broadcast_presence()

    async def _broadcast_presence(self) -> None:
        if self._state is None or not self._clients:
            return
        payload = json.dumps(self._state.to_message()) + "\n"
        await asyncio.gather(
            *(self._send_raw(ws, payload) for ws in list(self._clients)), return_exceptions=True
        )

    async def _send_raw(self, ws: ServerConnection, payload: str) -> None:
        try:
            await ws.send(payload)
        except ConnectionClosed:
            self._clients.discard(ws)

    async def _send(self, ws: ServerConnection, obj: dict[str, Any]) -> None:
        await self._send_raw(ws, json.dumps(obj) + "\n")

    # -- wake acknowledgement (touch via the daemon, or the passphrase heard) ----------------
    def ack_wake(self, source: str) -> None:
        """Stop a running wake-up alarm. Called from the asyncio loop."""
        if self._wake_ack is not None and not self._wake_ack.is_set():
            self._wake_ack_by = source
            self._wake_ack.set()

    def ack_wake_threadsafe(self, source: str) -> None:
        if self._loop is None:
            self.ack_wake(source)
        else:
            self._loop.call_soon_threadsafe(self.ack_wake, source)

    # -- events (robot -> daemon, unsolicited) ------------------------------------------------
    def emit_event(self, obj: dict[str, Any]) -> None:
        """Broadcast ``{"t":"event",...}`` to every connected daemon. Asyncio loop only."""
        if not self._clients:
            log.info("event %s (no daemon connected)", obj.get("ev"))
            return
        payload = json.dumps(obj) + "\n"
        self._spawn(self._broadcast_raw(payload))

    def emit_event_threadsafe(self, obj: dict[str, Any]) -> None:
        if self._loop is None:
            self.emit_event(obj)
        else:
            self._loop.call_soon_threadsafe(self.emit_event, obj)

    async def _broadcast_raw(self, payload: str) -> None:
        await asyncio.gather(
            *(self._send_raw(ws, payload) for ws in list(self._clients)), return_exceptions=True
        )

    @property
    def busy(self) -> bool:
        """A wake is running or speech / a passphrase check is in progress (the dock must
        not drive away for a charge while the daemon is talking to the owner)."""
        return self.wake_active or self._speech_lock.locked()

    # -- connections ----------------------------------------------------------------------
    async def _handle(self, ws: ServerConnection) -> None:
        self._clients.add(ws)
        log.info("daemon connected (%d client(s))", len(self._clients))
        try:
            if self._state is not None:
                await self._send(ws, self._state.to_message())
            async for raw in ws:
                for obj in _parse_lines(raw):
                    if self._inflight >= self.MAX_INFLIGHT:
                        mid = obj.get("id") if isinstance(obj, dict) else None
                        await self._send(ws, {"t": "error", "id": mid, "error": "busy"})
                        continue
                    self._spawn(self._tracked(self._dispatch(ws, obj)))
        except ConnectionClosed:
            pass
        finally:
            self._clients.discard(ws)
            log.info("daemon disconnected (%d client(s))", len(self._clients))

    MAX_INFLIGHT = 16

    _inflight = 0


    async def _tracked(self, coro):  # noqa: ANN001, ANN201

        self._inflight += 1

        try:

            return await coro

        finally:

            self._inflight -= 1


    def _spawn(self, coro: Any) -> None:
        task = asyncio.create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _dispatch(self, ws: ServerConnection, msg: dict[str, Any]) -> None:
        t = msg.get("t")
        mid = msg.get("id")
        try:
            if t == "say":
                text = str(msg.get("text", ""))
                async with self._speech_lock:
                    await self.handlers.say(text)
                if mid is not None:
                    await self._send(ws, {"t": "said", "id": mid})
            elif t == "ask_passphrase":
                async with self._speech_lock:
                    ok = bool(await self.handlers.ask_passphrase())
                await self._send(ws, {"t": "passphrase", "id": mid, "ok": ok})
            elif t == "attention":
                await self.handlers.attention()
                if mid is not None:
                    await self._send(ws, {"t": "done", "id": mid})
            elif t == "morning_report":
                items = msg.get("items") or []
                await self.morning_report(items)
                if mid is not None:
                    await self._send(ws, {"t": "done", "id": mid})
            elif t == "brief":
                lines = [str(x) for x in (msg.get("lines") or [])]
                await self.brief(msg.get("facts") or {}, lines)
                if mid is not None:
                    await self._send(ws, {"t": "done", "id": mid})
            elif t == "chat":
                text = str(msg.get("text", "")).strip()
                if not text:
                    await self._send(ws, {"t": "error", "id": mid, "error": "empty_text"})
                    return
                reply = await self.handlers.chat(text, self._chat_context())
                # The text goes back first so a UI can show it while the robot talks.
                await self._send(ws, {"t": "chat", "id": mid, "text": reply})
                async with self._speech_lock:
                    await self.handlers.say(reply)
            elif t == "settings":
                try:
                    changed = self.flags.update(msg)
                except ValueError as exc:
                    await self._send(ws, {"t": "error", "id": mid, "error": f"bad_settings:{exc}"})
                    return
                await self.handlers.apply_settings(self.flags, changed)
                log.info("settings: %s (changed %s)", vars(self.flags), changed)
                reply = self.flags.to_message()
                if mid is not None:
                    reply["id"] = mid
                await self._send(ws, reply)
            elif t == "enroll_room":
                ok = bool(await self.handlers.enroll_room())
                await self._send(ws, {"t": "enroll_room", "id": mid, "ok": ok})
            elif t == "network":
                applied = await self.handlers.network_override(
                    bool(msg.get("on", False)),
                    str(msg.get("reason", "")),
                    float(msg.get("for_secs", 0) or 0),
                )
                if mid is not None:
                    await self._send(ws, {"t": "network", "id": mid, "applied": bool(applied)})
            elif t == "touch":
                self.ack_wake("touch")
            elif t == "wake":
                await self._send(ws, await self.wake(msg))
            elif t == "goto":
                room = str(msg.get("room", "")).strip().lower()
                if not room:
                    await self._send(ws, {"t": "error", "id": mid, "error": "empty_room"})
                    return
                res = await self.handlers.goto(room)
                await self._send(
                    ws,
                    {
                        "t": "goto",
                        "id": mid,
                        "room": room,
                        "ok": bool(res.ok),
                        "reason": res.reason,
                        "secs": round(float(res.secs)),
                    },  # fmt: skip
                )
            elif t == "name_room":
                name = str(msg.get("name", "")).strip().lower()
                if not name:
                    await self._send(ws, {"t": "error", "id": mid, "error": "empty_name"})
                    return
                radius = msg.get("radius")
                info = await self.handlers.name_room(
                    name, float(radius) if radius is not None else None, msg.get("polygon")
                )
                await self._send(ws, {"t": "name_room", "id": mid, "ok": True, **info})
            elif t == "map":
                action = str(msg.get("action", "status"))
                if action not in ("start", "save", "status"):
                    await self._send(ws, {"t": "error", "id": mid, "error": "bad_action"})
                    return
                name = msg.get("name")
                if name is not None and not re.fullmatch(r"[a-z0-9_-]{1,32}", str(name)):
                    await self._send(ws, {"t": "error", "id": mid, "error": "bad_name"})
                    return
                info = await self.handlers.map_action(action, str(name) if name else None)
                await self._send(ws, {"t": "map", "id": mid, "action": action, **info})
            elif t == "dock":
                action = str(msg.get("action", "status"))
                if action not in ("go", "leave", "status"):
                    await self._send(ws, {"t": "error", "id": mid, "error": "bad_action"})
                    return
                info = await self.handlers.dock_action(action)
                await self._send(ws, {"t": "dock", "id": mid, "action": action, **info})
            else:
                await self._send(ws, {"t": "error", "id": mid, "error": f"unknown_type:{t}"})
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            log.exception("request %s failed", t)
            with contextlib.suppress(Exception):
                await self._send(ws, {"t": "error", "id": mid, "error": str(exc)})

    def _chat_context(self) -> dict[str, Any]:
        """Whitelisted presence facts for the brain (never anything secret)."""
        st = self._state
        ctx: dict[str, Any] = {"time": time.strftime("%H:%M")}
        if st is not None:
            ctx.update(
                person=st.person, owner=st.owner, voice_ok=st.voice_ok, moved=st.moved,
                duress=st.duress,
            )  # fmt: skip
        return ctx

    async def brief(self, facts: Mapping[str, Any], lines: list[str]) -> None:
        """Morning briefing. The brain rewrites the daemon's plain sentences in the robot's voice;
        every number must survive, nothing may be added. Without a brain (or when it fails) the
        lines are read as they are, one at a time."""
        if not lines:
            await self.morning_report([])
            return
        spoken: str | None = None
        try:
            reply = await self.handlers.chat(brief_prompt(lines, facts), self._chat_context())
            if reply and reply.strip() and numbers_preserved(lines, reply):
                spoken = reply.strip()
        except Exception as exc:  # noqa: BLE001 - any brain failure means plain reading
            log.info("brief: brain unavailable (%s); reading plainly", exc)
        if spoken is not None:
            async with self._speech_lock:
                await self.handlers.say(spoken)
            return
        async with self._speech_lock:
            for n, text in enumerate(lines, 1):
                await self.handlers.say(text)
                if n < len(lines):
                    await asyncio.sleep(self.report_pause_s)

    async def morning_report(self, items: Iterable[Any]) -> None:
        """Spoken newspaper: one item at a time with a short pause between them."""
        texts = [report_item_text(i) for i in items]
        async with self._speech_lock:
            if not texts:
                await self.handlers.say("Nothing to report this morning.")
                return
            await self.handlers.say(f"Good morning. {len(texts)} items.")
            await asyncio.sleep(self.report_pause_s)
            for n, text in enumerate(texts, 1):
                await self.handlers.say(text)
                if n < len(texts):
                    await asyncio.sleep(self.report_pause_s)

    # -- wake-up alarm -----------------------------------------------------------------------
    async def wake(self, msg: dict[str, Any]) -> dict[str, Any]:
        """Find the owner (if navigation can), face them, then repeat the lines with
        rising urgency until a touch / the passphrase / ``max_secs``."""
        mid = msg.get("id")
        reply: dict[str, Any] = {"t": "wake", "id": mid, "acked": False, "secs": 0}
        if not self.flags.wake:
            reply["reason"] = "wake_off"
            return reply
        lines = [str(x) for x in (msg.get("lines") or []) if str(x).strip()]
        if not lines:
            lines = [f"Wake up. {msg.get('reason', 'Alert')}."]
        max_secs = float(msg.get("max_secs") or self.wake_max_secs)
        max_secs = max(1.0, min(max_secs, 3600.0))
        if self.wake_active:
            reply["reason"] = "busy"
            return reply
        self.wake_active = True
        loop = asyncio.get_running_loop()
        t0 = loop.time()
        self._wake_ack = asyncio.Event()
        self._wake_ack_by = None
        travelled = False
        room: str | None = None
        try:
            # 0. Charging? The alarm outranks the dock: get off it first.
            try:
                await self.handlers.leave_dock_for("wake")
            except Exception as exc:
                log.error("leave_dock_for failed: %s", exc)
            # 1. Get to the owner: navigate if they are not in view and a map exists.
            if not self.handlers.owner_visible():
                try:
                    found = await self.handlers.find_owner(max_secs * 0.6)
                except Exception as exc:
                    log.error("find_owner failed: %s", exc)
                    found = None
                if found is not None:
                    travelled = bool(found.travelled)
                    room = found.room
            # 2. Face the owner and step closer.
            try:
                await self.handlers.wake_approach()
            except Exception as exc:
                log.error("wake approach failed: %s", exc)
            # 3. Speak, louder and faster, until acknowledged.
            total = 3 * len(lines)
            step = 0
            while not self._wake_ack.is_set() and loop.time() - t0 < max_secs:
                line = lines[step % len(lines)]
                urgency = wake_urgency(step, total)
                async with self._speech_lock:
                    if self._wake_ack.is_set():
                        break
                    await self.handlers.say(line, urgency=urgency)
                pause = max(WAKE_MIN_PAUSE_S, self.wake_pause_s * (1.0 - 0.8 * urgency))
                remaining = max_secs - (loop.time() - t0)
                if remaining <= 0:
                    break
                with contextlib.suppress(TimeoutError, asyncio.TimeoutError):
                    await asyncio.wait_for(self._wake_ack.wait(), min(pause, remaining))
                step += 1
            acked = self._wake_ack.is_set()
            reply.update(
                acked=acked,
                secs=int(round(loop.time() - t0)),
                reason=self._wake_ack_by if acked else "timeout",
                travelled=travelled,
                room=room,
            )
            return reply
        finally:
            self.wake_active = False
            self._wake_ack = None


def _parse_lines(raw: str | bytes) -> list[dict[str, Any]]:
    text = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw
    out: list[dict[str, Any]] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            log.warning("ignoring non-JSON line from daemon")
            continue
        if isinstance(obj, dict):
            out.append(obj)
    return out
