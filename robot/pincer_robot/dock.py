"""Autonomous charging dock (optional; ``FeatureFlags.dock``, off by default).

Hardware: two spring-loaded pogo contacts under the body meet two copper strips on a floor
plate standing against a wall; the plate is fed by a 3S CC/CV brick (12.6 V, 3 A) through a
reverse-polarity diode. There is no dock-side electronics, so the robot knows it is on the
dock only by current flowing INTO the pack (``BatteryState.charging``, from the INA226).

Enrolment: the owner puts the robot ON the dock, rear to the wall, and sends
``{"t":"name_room","name":"dock"}``. We store the exact on-dock map pose and the lidar range
to the wall behind us (``~/.pincer1/dock.json``) and register a room named ``dock`` at the
*staging pose* 0.5 m in front of the plate (radius 0.3 m) - that is what ``goto("dock")``
navigates to, because Nav2 should not be asked to park on top of the contacts.

State machine (``DockController.state``)::

    off -> idle -> navigating -> approaching -> verifying -> charging -> undocking -> idle
                       \\__________ failed (after 3 approach attempts, or nav failure)

* idle -> navigating when the state of charge is <= ``dock_at_pct`` (20 %), docking is
  enabled, the dock is enrolled, a map exists and ``busy()`` is False (no wake running, no
  speech / passphrase in progress, the owner did not just call it). A manual ``go`` skips
  the SoC and busy checks.
* approaching: from the staging pose, straight back at 0.05 m/s in 5 cm steps until the
  rear lidar range reaches the enrolled wall distance (+3 cm), or current flows, or we
  have gone the approach distance plus 10 cm.
* verifying: wait up to 10 s for ``charging``; otherwise back off 0.2 m forward and try
  again, three attempts in all; then ``failed``: say so, raise ``dock_failed``, and retry
  on our own only after ``retry_cooldown_s``.
* charging: stay until ``undock_at_pct`` (90 %) or until something more important pulls it
  off (``leave_dock_for(reason)`` from the wake handler / ``attention`` / a daemon
  ``dock leave``). Undocking is 0.3 m forward. After an involuntary leave the robot goes
  back on its own once ``busy()`` clears, if still below ``undock_at_pct``.
* ``battery_low`` is raised once at ``low_pct`` (15 %) whenever the robot cannot help
  itself: docking off, failed, or not enrolled; the daemon can phone the owner.

Everything that touches hardware is injected (battery reader, navigator, robot, rear lidar
range, speech, event sink, clock and sleep), so the whole machine runs under pytest with
mocks and in ``pincer-robot --mock`` with a scripted pack.
"""

from __future__ import annotations

import asyncio
import json
import logging
import math
import os
import threading
import time
from collections.abc import Awaitable, Callable
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Literal

import numpy as np

from .battery import BatteryState
from .motion import GAIT_SPEED_MPS, MotionController, RobotInterface
from .nav import Navigator, RoomDef

log = logging.getLogger("pincer.dock")

DockState = Literal[
    "off", "idle", "navigating", "approaching", "verifying", "charging", "undocking", "failed"
]
DOCK_ROOM = "dock"
DOCK_ROOM_RADIUS_M = 0.3


def rear_range_from_scan(
    angles: np.ndarray, ranges: np.ndarray, valid: np.ndarray, half_width_deg: float = 10.0
) -> float | None:
    """Median range of the valid beams within ``half_width_deg`` of straight behind (angle pi)."""
    a = np.asarray(angles, dtype=np.float64)
    off = np.abs((a - math.pi + math.pi) % (2 * math.pi) - math.pi)
    sel = valid & (off <= math.radians(half_width_deg))
    if not np.any(sel):
        return None
    return float(np.median(np.asarray(ranges, dtype=np.float64)[sel]))


# --- enrolment record ---------------------------------------------------------------------------


@dataclass
class DockSite:
    x: float  # on-dock pose, map frame
    y: float
    yaw: float  # robot heading when docked (rear to the wall)
    wall_m: float | None  # lidar range to the wall behind, on the dock
    approach_m: float = 0.5
    created_ts: float = field(default_factory=time.time)

    def staging(self) -> tuple[float, float, float]:
        """Pose ``approach_m`` in front of the plate, same heading: the ``goto`` goal."""
        return (
            self.x + self.approach_m * math.cos(self.yaw),
            self.y + self.approach_m * math.sin(self.yaw),
            self.yaw,
        )

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> DockSite:
        wall = d.get("wall_m")
        return cls(
            x=float(d["x"]),
            y=float(d["y"]),
            yaw=float(d.get("yaw", 0.0)),
            wall_m=None if wall is None else float(wall),
            approach_m=float(d.get("approach_m", 0.5)),
            created_ts=float(d.get("created_ts", 0.0)),
        )


class DockStore:
    def __init__(self, path: Path) -> None:
        self.path = path.expanduser()
        self.site: DockSite | None = None
        self.load()

    def load(self) -> DockSite | None:
        self.site = None
        if self.path.exists():
            try:
                self.site = DockSite.from_dict(json.loads(self.path.read_text()))
            except (OSError, ValueError, KeyError, TypeError) as exc:
                log.error("dock.json unreadable (%s); dock not enrolled", exc)
        return self.site

    def save(self, site: DockSite) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(site.to_dict(), indent=1))
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)
        self.site = site


# --- controller ----------------------------------------------------------------------------------


@dataclass
class DockConfig:
    dock_at_pct: float = 20.0
    undock_at_pct: float = 90.0
    low_pct: float = 15.0
    approach_m: float = 0.5
    approach_speed_mps: float = 0.05
    step_m: float = 0.05
    overshoot_m: float = 0.10  # how far past approach_m we accept going before giving up
    wall_tol_m: float = 0.03
    verify_s: float = 10.0
    poll_s: float = 0.5
    backoff_m: float = 0.2
    max_attempts: int = 3
    undock_m: float = 0.3
    retry_cooldown_s: float = 900.0
    contact_lost_s: float = 5.0  # charging state ends when current is gone this long
    resample_s: float = 1.0  # after an undock, trust "charging" only from a sample this much newer


class DockController:
    def __init__(
        self,
        cfg: DockConfig,
        *,
        battery: Callable[[], BatteryState | None],
        nav: Navigator,
        robot: RobotInterface,
        motion: MotionController,
        rear_range: Callable[[], float | None],
        busy: Callable[[], bool],
        say: Callable[[str], Awaitable[None]],
        emit: Callable[[dict[str, Any]], None],
        store: DockStore,
        enabled: bool = False,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
        on_state: Callable[[str], None] | None = None,
    ) -> None:
        self.cfg = cfg
        self._battery = battery
        self.nav = nav
        self.robot = robot
        self.motion = motion
        self._rear_range = rear_range
        self._busy = busy
        self._say = say
        self._emit = emit
        self.store = store
        self._clock = clock
        self._sleep = sleep
        self._on_state = on_state
        self._enabled = enabled
        self._state: str = "off"
        self._task: asyncio.Task[None] | None = None
        self._abort = threading.Event()
        self._go_requested = False
        self._return_pending = False
        self._failed_ts = -math.inf
        self._low_sent = False
        self._no_current_since: float | None = None
        self._undock_sample_ts = -math.inf  # battery sample time when the last undock ended
        self.attempts = 0
        self.last_error: str | None = None
        self.leave_reason: str | None = None
        if enabled:
            self._set_state("idle")

    # -- state -----------------------------------------------------------------------------
    @property
    def state(self) -> str:
        return self._state

    def _set_state(self, new: str) -> None:
        if new != self._state:
            log.info("dock %s -> %s", self._state, new)
            self._state = new
            if self._on_state is not None:
                try:
                    self._on_state(new)
                except Exception as exc:  # pragma: no cover - observer bug must not kill us
                    log.error("dock on_state failed: %s", exc)

    @property
    def enabled(self) -> bool:
        return self._enabled

    @enabled.setter
    def enabled(self, value: bool) -> None:
        value = bool(value)
        if value == self._enabled:
            return
        self._enabled = value
        if value:
            self._set_state("idle")
        else:
            self._cancel_run()
            self._go_requested = False
            self._return_pending = False
            self._set_state("off")

    @property
    def enrolled(self) -> bool:
        return self.store.site is not None and self.nav.rooms.get(DOCK_ROOM) is not None

    @property
    def running(self) -> bool:
        return self._task is not None and not self._task.done()

    @property
    def return_pending(self) -> bool:
        return self._return_pending

    def status(self) -> dict[str, Any]:
        site = self.store.site
        b = self._battery()
        return {
            "state": self._state,
            "enabled": self._enabled,
            "enrolled": self.enrolled,
            "map": bool(self.nav.available),
            "attempts": self.attempts,
            "return_pending": self._return_pending,
            "wall_m": None if site is None or site.wall_m is None else round(site.wall_m, 3),
            "battery": None if b is None else b.to_message(),
            "last_error": self.last_error,
        }

    # -- enrolment ---------------------------------------------------------------------------
    def enroll(self, wall_m: float | None, *, approach_m: float | None = None) -> DockSite:
        """Robot is ON the dock, rear to the wall: store the pose, name the staging pose."""
        loc = self.nav.localize_raw()
        if loc is None:
            raise RuntimeError("not localised: build/load a map first")
        site = DockSite(
            loc.x, loc.y, loc.yaw, wall_m, approach_m=approach_m or self.cfg.approach_m
        )
        self.store.save(site)
        sx, sy, syaw = site.staging()
        # Registered directly: the registry's 0.5 m floor is for rooms, this is a parking spot.
        self.nav.rooms.rooms[DOCK_ROOM] = RoomDef(
            DOCK_ROOM, sx, sy, syaw, radius=DOCK_ROOM_RADIUS_M
        )
        self.nav.rooms.save()
        if wall_m is None:
            log.warning("dock enrolled without a rear lidar range: approach by distance only")
        log.info("dock enrolled at (%.2f, %.2f) yaw %.2f, wall %.2f m", loc.x, loc.y, loc.yaw,
                 -1.0 if wall_m is None else wall_m)  # fmt: skip
        self._failed_ts = -math.inf
        if self._state == "failed":
            self._set_state("idle")
        return site

    # -- requests ----------------------------------------------------------------------------
    def go(self) -> dict[str, Any]:
        """Daemon ``dock go``: dock now regardless of SoC (still needs map + enrolment)."""
        if not self._enabled:
            return {"ok": False, "reason": "dock_off"}
        if self._state in ("charging", "navigating", "approaching", "verifying"):
            return {"ok": True, "reason": self._state}
        if not self.enrolled:
            return {"ok": False, "reason": "not_enrolled"}
        if not self.nav.available:
            return {"ok": False, "reason": "no_map"}
        self._go_requested = True
        if self._state in ("failed", "idle"):
            self._set_state("idle")
            self._failed_ts = -math.inf
        return {"ok": True, "reason": "queued"}

    async def leave_dock_for(self, reason: str) -> bool:
        """Something more important than charging: cancel a docking run or undock.

        Returns True if the robot was docking/charging. For an involuntary leave (anything
        but the daemon's own ``leave``) the robot returns to the dock once ``busy()`` clears
        and the pack is still below ``undock_at_pct``.
        """
        if not self._enabled or self._state in ("off", "idle", "failed", "undocking"):
            return False
        voluntary = reason == "daemon"
        self.leave_reason = reason
        if self._state == "charging":
            log.info("leaving the dock for %s", reason)
            await self._undock(return_after=not voluntary)
            return True
        # navigating / approaching / verifying: abandon the run.
        log.info("docking interrupted for %s", reason)
        self._cancel_run()
        self.robot.stop()
        self._return_pending = not voluntary
        self._set_state("idle")
        return True

    # -- periodic ----------------------------------------------------------------------------
    def tick(self, now: float | None = None) -> None:
        """Called from the fusion loop (10 Hz). Decides; the work runs in a task."""
        now = self._clock() if now is None else now
        b = self._battery()
        if b is None:
            return
        if not self._enabled:
            self._check_low(b, can_dock=False)
            return
        if self.running:
            return
        st = self._state
        if st == "charging":
            self._return_pending = False
            if b.charging:
                self._no_current_since = None
            else:
                if self._no_current_since is None:
                    self._no_current_since = now
                elif now - self._no_current_since >= self.cfg.contact_lost_s:
                    log.warning("charging current gone for %.0fs: off the dock?",
                                now - self._no_current_since)  # fmt: skip
                    self._no_current_since = None
                    self._set_state("idle")
                    return
            if b.pct >= self.cfg.undock_at_pct:
                self._start(self._undock(return_after=False))
            return
        if (
            b.charging
            and st in ("idle", "failed")
            and b.ts - self._undock_sample_ts >= self.cfg.resample_s
        ):
            # Placed on the plate by hand (or the contacts came back). The sample must
            # postdate the last undock: the pack is read every 0.5 s, so the first tick
            # after driving off still carries the on-dock current.
            self._no_current_since = None
            self._set_state("charging")
            self._low_sent = False
            return
        if st == "failed":
            self._check_low(b, can_dock=False)
            if now - self._failed_ts >= self.cfg.retry_cooldown_s:
                self._set_state("idle")
            return
        if st != "idle":
            return
        can_dock = self.enrolled and self.nav.available
        self._check_low(b, can_dock=can_dock)
        want = (
            self._go_requested
            or b.pct <= self.cfg.dock_at_pct
            or (self._return_pending and b.pct < self.cfg.undock_at_pct)
        )
        if self._return_pending and b.pct >= self.cfg.undock_at_pct:
            self._return_pending = False
        if not want:
            return
        if not can_dock:
            if self._go_requested:
                self._go_requested = False
            return
        if not self._go_requested and self._busy():
            return
        self._go_requested = False
        self._set_state("navigating")  # visible at once; the task confirms it
        self._start(self._run_dock())

    def _check_low(self, b: BatteryState, *, can_dock: bool) -> None:
        if b.charging or b.pct >= self.cfg.low_pct + 5.0:
            self._low_sent = False
            return
        if can_dock or self._low_sent or b.pct > self.cfg.low_pct:
            return
        self._low_sent = True
        log.warning("battery low (%.0f %%) and docking cannot help", b.pct)
        self._emit({"t": "event", "ev": "battery_low", "pct": int(round(b.pct)),
                    "dock": self._state})  # fmt: skip

    # -- runs ----------------------------------------------------------------------------------
    def _start(self, coro: Awaitable[None]) -> None:
        self._abort.clear()
        self._task = asyncio.get_running_loop().create_task(coro)  # type: ignore[arg-type]

    def _cancel_run(self) -> None:
        self._abort.set()
        if self._task is not None and not self._task.done():
            self._task.cancel()

    async def wait(self) -> None:
        """Await the current run (tests)."""
        if self._task is not None:
            try:
                await self._task
            except asyncio.CancelledError:
                pass

    def _speed(self) -> float:
        return max(0.1, min(1.0, self.cfg.approach_speed_mps / GAIT_SPEED_MPS))

    def _approach_blocking(self, wall_m: float | None) -> float:
        """Back up in steps until the wall distance / current / distance budget. Returns m."""
        cfg = self.cfg
        travelled = 0.0
        budget = cfg.approach_m + (cfg.overshoot_m if wall_m is not None else 0.0)
        while not self._abort.is_set():
            b = self._battery()
            if b is not None and b.charging:
                break
            rear = self._rear_range()
            if wall_m is not None and rear is not None and rear <= wall_m + cfg.wall_tol_m:
                break
            if travelled + 1e-9 >= budget:
                break
            step = min(cfg.step_m, budget - travelled)
            note = self.robot.walk("backward", step, speed=self._speed())
            if note.startswith("REFUSED"):
                log.warning("approach stopped: %s", note)
                break
            travelled += step
        self.robot.stop()
        return travelled

    async def _wait_charging(self, secs: float) -> bool:
        t0 = self._clock()
        while True:
            b = self._battery()
            if b is not None and b.charging:
                return True
            if self._clock() - t0 >= secs:
                return False
            await self._sleep(self.cfg.poll_s)

    async def _run_dock(self) -> None:
        cfg = self.cfg
        site = self.store.site
        self.attempts = 0
        self.last_error = None
        try:
            self._set_state("navigating")
            res = await self.nav.goto(DOCK_ROOM)
            if not res.ok:
                await self._fail(f"nav:{res.reason}")
                return
            wall_m = None if site is None else site.wall_m
            for attempt in range(1, cfg.max_attempts + 1):
                self.attempts = attempt
                self._set_state("approaching")
                travelled = await self.motion.run_exclusive(self._approach_blocking, wall_m)
                if self._abort.is_set():
                    return
                self._set_state("verifying")
                if await self._wait_charging(cfg.verify_s):
                    log.info("docked after %d attempt(s), %.2f m", attempt, travelled)
                    self._no_current_since = None
                    self._return_pending = False
                    self._low_sent = False
                    self._set_state("charging")
                    return
                log.warning("no charge current after attempt %d (%.2f m back)", attempt, travelled)
                if attempt < cfg.max_attempts:
                    await self.motion.run_exclusive(
                        self.robot.walk, "forward", cfg.backoff_m, self._speed()
                    )
            await self._fail("no_current")
        except asyncio.CancelledError:
            self.robot.stop()
            raise
        except Exception as exc:
            log.exception("docking run crashed")
            await self._fail(f"error:{exc}")

    async def _fail(self, reason: str) -> None:
        self.last_error = reason
        self._failed_ts = self._clock()
        self._return_pending = False
        self._set_state("failed")
        self._emit({"t": "event", "ev": "dock_failed", "reason": reason,
                    "attempts": self.attempts})  # fmt: skip
        try:
            await self._say("I could not reach the charger.")
        except Exception as exc:
            log.error("dock failure speech failed: %s", exc)

    async def _undock(self, *, return_after: bool) -> None:
        self._set_state("undocking")
        try:
            await self.motion.run_exclusive(
                self.robot.walk, "forward", self.cfg.undock_m, self._speed()
            )
        except asyncio.CancelledError:
            self.robot.stop()
            raise
        except Exception as exc:
            log.error("undock drive failed: %s", exc)
        finally:
            b = self._battery()
            self._undock_sample_ts = b.ts if b is not None else self._clock()
            self._return_pending = return_after
            self._set_state("idle")

    def close(self) -> None:
        self._cancel_run()
