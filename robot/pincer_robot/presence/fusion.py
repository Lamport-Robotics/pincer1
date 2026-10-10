"""Presence fusion: pure logic, no I/O, no clocks of its own.

Combines the per-sensor signals into the ``presence`` object of PROTOCOL.md
section 2::

    {"t":"presence","person":true,"owner":true,"voice_ok":false,"duress":false,
     "moved":false,"confidence":{"lidar":0.92,"face":0.88},"ts":1791600000}

Rules (every threshold lives in ``FusionConfig``):

* ``person``   - lidar confidence >= ``person_on`` for ``person_on_frames`` consecutive
  updates turns it on; it turns off only after ``person_off_frames`` consecutive updates
  below ``person_off`` (or the lidar going stale). A live face also counts as a person.
* ``owner``    - face owner-confidence >= ``owner_on`` for ``owner_on_frames`` consecutive
  frames turns it on; while on, any frame >= ``owner_off`` keeps it alive. It drops when
  no such frame has been seen for ``owner_grace_s`` (so one bad frame never flips it),
  and no matter what after ``session_timeout_s`` without a *strong* (>= ``owner_on``)
  match - a session must be refreshed by a real look at the camera.
* ``voice_ok`` - the passphrase + speaker check passed within ``voice_ttl_s``; cleared by
  ``moved`` and when the owner is lost.
* ``moved``    - IMU acceleration spike (|a| far from 1 g) or body tilt beyond
  ``moved_tilt_deg``; held for ``moved_hold_s``. The daemon locks the controller on it.
* ``duress``   - latched by ``raise_duress`` (duress word or an explicit expression hook)
  until ``clear_duress``. The other fields are deliberately *not* altered so the robot's
  visible behaviour does not change under duress.

Optional fields (PROTOCOL.md section 2, "additional states"), each present on the wire
only when its feature is on:

* ``persons`` / ``owners`` - lidar body count and enrolled labels matched, when
  ``two_person`` is enabled (``PresenceFusion.two_person``, runtime-switchable).
* ``room_match``, ``network``, ``room`` - supplied by the app (room fingerprint or map
  localisation, air-gap relay, navigation) through ``PresenceFusion.set_extras`` and
  copied into every state; ``None`` means "omit".
* ``battery`` (``{"pct","voltage","charging"}``) and ``dock`` (the dock controller's
  state) - also app-supplied extras; present on every message once a battery monitor
  exists (always, in ``pincer-robot``; mock pack on a laptop).

All times are seconds (``time.time()`` style floats) supplied by the caller, which
makes every transition deterministic and testable.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

G = 9.80665


@dataclass
class FusionConfig:
    person_on: float = 0.6
    person_off: float = 0.3
    person_on_frames: int = 2
    person_off_frames: int = 5
    lidar_stale_s: float = 3.0
    owner_on: float = 0.7
    owner_off: float = 0.4
    owner_on_frames: int = 2
    owner_grace_s: float = 5.0
    session_timeout_s: float = 300.0
    voice_ttl_s: float = 120.0
    moved_accel_threshold: float = 3.0
    moved_tilt_deg: float = 25.0
    moved_hold_s: float = 10.0
    face_stale_s: float = 2.0


@dataclass(frozen=True)
class PresenceState:
    person: bool
    owner: bool
    voice_ok: bool
    duress: bool
    moved: bool
    lidar_conf: float
    face_conf: float
    ts: float
    # Feature fields; None = feature off / unknown = omitted from the wire object.
    room_match: float | None = None
    persons: int | None = None
    owners: tuple[str, ...] | None = None
    network: bool | None = None
    room: str | None = None
    room_known: bool = False  # True when a map exists, so ``room`` (even null) is sent
    battery: tuple[int, float, bool] | None = None  # (pct, voltage, charging)
    dock: str | None = None  # dock controller state

    def to_message(self) -> dict[str, object]:
        """Exactly the wire object of PROTOCOL.md section 2."""
        msg: dict[str, object] = {
            "t": "presence",
            "person": self.person,
            "owner": self.owner,
            "voice_ok": self.voice_ok,
            "duress": self.duress,
            "moved": self.moved,
            "confidence": {
                "lidar": round(self.lidar_conf, 2),
                "face": round(self.face_conf, 2),
            },
            "ts": int(self.ts),
        }
        if self.room_match is not None:
            msg["room_match"] = round(self.room_match, 2)
        if self.persons is not None:
            msg["persons"] = self.persons
        if self.owners is not None:
            msg["owners"] = list(self.owners)
        if self.network is not None:
            msg["network"] = self.network
        if self.room_known:
            msg["room"] = self.room
        if self.battery is not None:
            pct, voltage, charging = self.battery
            msg["battery"] = {"pct": pct, "voltage": voltage, "charging": charging}
        if self.dock is not None:
            msg["dock"] = self.dock
        return msg

    def signature(self) -> tuple[object, ...]:
        """What counts as a *change* worth pushing immediately (the discrete fields)."""
        return (
            self.person, self.owner, self.voice_ok, self.duress, self.moved,
            self.persons, self.owners, self.network, self.room,
            None if self.battery is None else self.battery[2], self.dock,
        )  # fmt: skip


@dataclass
class _Inputs:
    lidar_conf: float = 0.0
    lidar_ts: float = -math.inf
    face_conf: float = 0.0
    face_ts: float = -math.inf
    face_live_present_ts: float = -math.inf
    persons: int = 0
    owners: tuple[str, ...] = ()


class PresenceFusion:
    def __init__(self, cfg: FusionConfig | None = None) -> None:
        self.cfg = cfg or FusionConfig()
        self._in = _Inputs()
        self._person = False
        self._person_on_count = 0
        self._person_off_count = 0
        self._owner = False
        self._owner_on_count = 0
        self._owner_last_ok_ts = -math.inf
        self._owner_last_strong_ts = -math.inf
        self._voice_until = -math.inf
        self._moved_until = -math.inf
        self._duress = False
        self._duress_source: str | None = None
        self.last_state: PresenceState | None = None
        self.events: list[tuple[float, str]] = []
        self.two_person = False  # publish persons/owners (runtime switch)
        self._extras: dict[str, object] = {}
        self.last_seen_bearing: float | None = None
        self.last_seen_room: str | None = None
        self.last_seen_ts: float = -math.inf

    # -- feature inputs ------------------------------------------------------------------
    def set_extras(
        self,
        *,
        room_match: float | None = None,
        network: bool | None = None,
        room: str | None = None,
        room_known: bool = False,
        battery: tuple[int, float, bool] | None = None,
        dock: str | None = None,
    ) -> None:
        """App-supplied fields copied into every state (None = omit)."""
        self._extras = dict(
            room_match=room_match, network=network, room=room, room_known=room_known,
            battery=battery, dock=dock,
        )  # fmt: skip

    def note_owner_seen(self, ts: float, bearing: float | None, room: str | None) -> None:
        """Remember where the owner was last seen (for ``find_owner`` / ``wake``)."""
        self.last_seen_ts = ts
        if bearing is not None:
            self.last_seen_bearing = bearing
        if room is not None:
            self.last_seen_room = room

    # -- sensor inputs -----------------------------------------------------------------
    def update_lidar(self, confidence: float, ts: float, persons: int = 0) -> None:
        cfg = self.cfg
        c = min(1.0, max(0.0, confidence))
        self._in.lidar_conf = c
        self._in.lidar_ts = ts
        self._in.persons = max(0, int(persons))
        if c >= cfg.person_on:
            self._person_on_count += 1
            self._person_off_count = 0
        elif c < cfg.person_off:
            self._person_off_count += 1
            self._person_on_count = 0
        else:
            # Hysteresis band: neither strengthens nor weakens.
            self._person_on_count = 0
            self._person_off_count = 0

    def update_face(
        self,
        owner_confidence: float,
        ts: float,
        *,
        live_present: bool = True,
        owners: tuple[str, ...] | list[str] = (),
    ) -> None:
        """``owner_confidence`` is already liveness-weighted by the face module.

        ``live_present`` says a live (not static) face of *someone* is in view;
        ``owners`` are the enrolled labels the face module matched in its window.
        """
        cfg = self.cfg
        c = min(1.0, max(0.0, owner_confidence))
        self._in.face_conf = c
        self._in.face_ts = ts
        self._in.owners = tuple(owners)
        if live_present:
            self._in.face_live_present_ts = ts
        if c >= cfg.owner_on:
            self._owner_on_count += 1
            self._owner_last_strong_ts = ts
            self._owner_last_ok_ts = ts
        else:
            self._owner_on_count = 0
            if self._owner and c >= cfg.owner_off:
                self._owner_last_ok_ts = ts

    def update_voice(self, ok: bool, ts: float) -> None:
        if ok:
            self._voice_until = ts + self.cfg.voice_ttl_s
            self.events.append((ts, "voice_ok"))
        else:
            self._voice_until = -math.inf

    def update_imu(
        self,
        accel: tuple[float, float, float],
        ts: float,
        tilt_deg: float | None = None,
    ) -> None:
        """``accel`` is linear acceleration including gravity (sensor_msgs/Imu)."""
        mag = math.sqrt(accel[0] ** 2 + accel[1] ** 2 + accel[2] ** 2)
        spike = abs(mag - G) >= self.cfg.moved_accel_threshold
        tilted = tilt_deg is not None and abs(tilt_deg) >= self.cfg.moved_tilt_deg
        if spike or tilted:
            self.mark_moved(ts, "accel" if spike else "tilt")

    def mark_moved(self, ts: float, source: str = "pose") -> None:
        """Explicit hook for pose jumps (e.g. odometry says the body teleported)."""
        if ts + self.cfg.moved_hold_s > self._moved_until:
            self._moved_until = ts + self.cfg.moved_hold_s
        self._voice_until = -math.inf
        self.events.append((ts, f"moved:{source}"))

    def raise_duress(self, ts: float, source: str = "word") -> None:
        if not self._duress:
            self.events.append((ts, f"duress:{source}"))
        self._duress = True
        self._duress_source = source

    def clear_duress(self) -> None:
        self._duress = False
        self._duress_source = None

    @property
    def duress(self) -> bool:
        return self._duress

    # -- evaluation --------------------------------------------------------------------
    def tick(self, ts: float) -> PresenceState:
        cfg = self.cfg
        i = self._in

        # Person (lidar with hysteresis + staleness).
        lidar_stale = ts - i.lidar_ts > cfg.lidar_stale_s
        if lidar_stale:
            lidar_conf = 0.0
            self._person_on_count = 0
            self._person_off_count = cfg.person_off_frames
        else:
            lidar_conf = i.lidar_conf
        if not self._person and self._person_on_count >= cfg.person_on_frames:
            self._person = True
            self.events.append((ts, "person:on"))
        elif self._person and self._person_off_count >= cfg.person_off_frames:
            self._person = False
            self.events.append((ts, "person:off"))
        live_face_recent = ts - i.face_live_present_ts <= cfg.face_stale_s

        # Owner (face with grace + session timeout).
        face_stale = ts - i.face_ts > cfg.face_stale_s
        face_conf = 0.0 if face_stale else i.face_conf
        if not self._owner and self._owner_on_count >= cfg.owner_on_frames:
            self._owner = True
            self.events.append((ts, "owner:on"))
        if self._owner:
            lost = ts - self._owner_last_ok_ts > cfg.owner_grace_s
            expired = ts - self._owner_last_strong_ts > cfg.session_timeout_s
            if lost or expired:
                self._owner = False
                self._owner_on_count = 0
                self._voice_until = -math.inf
                self.events.append((ts, "owner:off" if lost else "owner:session_timeout"))

        moved = ts < self._moved_until
        voice_ok = ts < self._voice_until and self._owner and not moved

        persons: int | None = None
        owners: tuple[str, ...] | None = None
        if self.two_person:
            persons = 0 if lidar_stale else i.persons
            owners = () if face_stale else i.owners
        ex = self._extras
        state = PresenceState(
            person=self._person or self._owner or live_face_recent,
            owner=self._owner,
            voice_ok=voice_ok,
            duress=self._duress,
            moved=moved,
            lidar_conf=lidar_conf,
            face_conf=face_conf,
            ts=ts,
            room_match=ex.get("room_match"),  # type: ignore[arg-type]
            persons=persons,
            owners=owners,
            network=ex.get("network"),  # type: ignore[arg-type]
            room=ex.get("room"),  # type: ignore[arg-type]
            room_known=bool(ex.get("room_known", False)),
            battery=ex.get("battery"),  # type: ignore[arg-type]
            dock=ex.get("dock"),  # type: ignore[arg-type]
        )
        self.last_state = state
        return state
