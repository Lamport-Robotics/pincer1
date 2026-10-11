"""Dock state machine against a simulated floor plate: a robot that tracks its distance
to the wall, a pack that only shows charge current while the pogo pins sit on the strips,
a teleporting MockNav, and a fake clock driven by the controller's own sleeps."""

from __future__ import annotations

import asyncio
import math
from pathlib import Path
from typing import Any

import numpy as np
import pytest

from pincer_robot.battery import BatteryMonitor, BatteryState, MockBatterySensor
from pincer_robot.dock import (
    DOCK_ROOM,
    DockConfig,
    DockController,
    DockSite,
    DockStore,
    rear_range_from_scan,
)
from pincer_robot.motion import MockRobot, MotionController
from pincer_robot.nav import MockNav, Navigator, RoomRegistry

WALL_M = 0.25  # lidar range to the wall when sitting on the plate
CONTACT_TOL_M = 0.03  # pogo pins still touch within this of the enrolled spot


class PlateRobot(MockRobot):
    """MockRobot whose walks move it along the dock axis (distance to the wall)."""

    def __init__(self, rig: Rig) -> None:
        super().__init__()
        self.rig = rig

    def walk(self, direction: str, distance_m: float, speed: float = 0.5) -> str:
        note = super().walk(direction, distance_m, speed)
        if note.startswith("OK"):
            sign = -1.0 if direction == "backward" else 1.0
            self.rig.dist = max(0.0, self.rig.dist + sign * distance_m)
        return note


class PlateNav(MockNav):
    def __init__(self, rig: Rig) -> None:
        super().__init__(has_map=True, pose=(5.0, 5.0, 0.0))
        self.rig = rig

    def goto(self, x: float, y: float, yaw: float, timeout_s: float):  # noqa: ANN201
        res = super().goto(x, y, yaw, timeout_s)
        if res.ok:
            self.rig.dist = WALL_M + 0.5 + self.rig.nav_error_m  # arrived at the staging pose
        return res


class Rig:
    def __init__(
        self,
        tmp_path: Path,
        *,
        pct: float = 15.0,
        enabled: bool = True,
        enrol: bool = True,
        powered: bool = True,
        **cfg: Any,
    ) -> None:
        self.t = 0.0
        self.dist = 3.0  # far from the wall
        self.nav_error_m = 0.0
        self.powered = powered  # charger brick plugged in?
        self.contact_after_attempt = 0  # plate "works" only from this attempt on (0 = always)
        self.busy_flag = False
        self.spoken: list[str] = []
        self.events: list[dict[str, Any]] = []
        self.states: list[str] = []
        self.sensor = MockBatterySensor.at_pct(pct)
        self.monitor = BatteryMonitor(self.sensor, tau_s=0.0, clock=self.clock)
        self.nav_backend = PlateNav(self)
        self.nav = Navigator(self.nav_backend, RoomRegistry(tmp_path / "rooms.json"), tmp_path)
        self.robot = PlateRobot(self)
        self.motion = MotionController(self.robot)
        self.store = DockStore(tmp_path / "dock.json")
        self.ctrl = DockController(
            DockConfig(verify_s=cfg.pop("verify_s", 10.0), retry_cooldown_s=900.0, **cfg),
            battery=self.battery,
            nav=self.nav,
            robot=self.robot,
            motion=self.motion,
            rear_range=lambda: self.dist,
            busy=lambda: self.busy_flag,
            say=self.say,
            emit=self.events.append,
            store=self.store,
            enabled=enabled,
            clock=self.clock,
            sleep=self.sleep,
            on_state=self.states.append,
        )
        if enrol:
            self.enrol()

    # -- world ---------------------------------------------------------------------------
    def clock(self) -> float:
        return self.t

    async def sleep(self, secs: float) -> None:
        self.t += secs
        await asyncio.sleep(0)

    def on_plate(self) -> bool:
        return self.dist <= WALL_M + CONTACT_TOL_M

    def battery(self) -> BatteryState | None:
        works = self.powered and self.ctrl.attempts >= self.contact_after_attempt
        self.sensor.set(current_a=2.5 if (self.on_plate() and works) else -0.9)
        return self.monitor.read()

    async def say(self, text: str) -> None:
        self.spoken.append(text)

    def enrol(self) -> DockSite:
        self.nav_backend.pose = (0.0, 0.0, 0.0)
        self.dist = WALL_M
        site = self.ctrl.enroll(WALL_M)
        self.nav_backend.pose = (5.0, 5.0, 0.0)
        self.dist = 3.0
        return site

    async def run_until(self, state: str, max_ticks: int = 2000) -> None:
        for _ in range(max_ticks):
            self.ctrl.tick()
            if self.ctrl.state == state and not self.ctrl.running:
                return
            await asyncio.sleep(0)
        raise AssertionError(f"never reached {state}; at {self.ctrl.state}, states {self.states}")

    def backward_m(self) -> float:
        return sum(abs(lin) * dur * 0.15 for lin, _ang, dur in self.robot.log if lin < 0)

    def forward_m(self) -> float:
        return sum(lin * dur * 0.15 for lin, _ang, dur in self.robot.log if lin > 0)


def test_rear_range_from_scan_picks_beams_behind() -> None:
    angles = np.radians(np.arange(-180.0, 180.0, 1.0))
    ranges = np.full(angles.shape, 3.0)
    valid = np.ones(angles.shape, dtype=bool)
    ranges[np.abs(np.degrees(angles)) >= 170.0] = 0.42  # +-10 degrees around the back
    assert rear_range_from_scan(angles, ranges, valid) == pytest.approx(0.42)
    ranges[np.abs(np.degrees(angles)) >= 175.0] = 0.30
    assert 0.30 <= rear_range_from_scan(angles, ranges, valid) <= 0.42  # type: ignore[operator]
    valid[np.abs(np.degrees(angles)) >= 160.0] = False
    assert rear_range_from_scan(angles, ranges, valid) is None
    # 0..2pi scans (angle_min = 0) wrap correctly.
    angles2 = np.radians(np.arange(0.0, 360.0, 1.0))
    ranges2 = np.full(angles2.shape, 2.0)
    ranges2[175:186] = 0.5
    assert rear_range_from_scan(angles2, ranges2, np.ones(angles2.shape, bool)) == 0.5


def test_enrolment_stores_site_and_staging_room(tmp_path: Path) -> None:
    rig = Rig(tmp_path, enrol=False)
    assert not rig.ctrl.enrolled
    rig.nav_backend.has_map = False
    with pytest.raises(RuntimeError):
        rig.ctrl.enroll(WALL_M)
    rig.nav_backend.has_map = True
    rig.nav_backend.pose = (2.0, 1.0, math.pi / 2)  # facing +y, wall behind at -y
    site = rig.ctrl.enroll(WALL_M)
    assert rig.ctrl.enrolled
    room = rig.nav.rooms.get(DOCK_ROOM)
    assert room is not None and room.radius == 0.3
    assert (room.x, room.y, room.yaw) == pytest.approx((2.0, 1.5, math.pi / 2))
    assert site.wall_m == WALL_M and site.approach_m == 0.5
    path = tmp_path / "dock.json"
    assert oct(path.stat().st_mode & 0o777) == "0o600"
    again = DockStore(path).site
    assert again is not None and (again.x, again.y, again.wall_m) == (2.0, 1.0, WALL_M)
    path.write_text("{broken")
    assert DockStore(path).site is None
    st = rig.ctrl.status()
    assert st["enrolled"] and st["wall_m"] == WALL_M and st["state"] == "idle"


def test_happy_path_navigate_approach_charge_undock(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=15.0)
        rig.ctrl.tick()
        assert rig.ctrl.running and rig.ctrl.state == "navigating"
        await rig.ctrl.wait()
        assert rig.ctrl.state == "charging"
        assert rig.states == ["idle", "navigating", "approaching", "verifying", "charging"]
        assert rig.nav_backend.goals[-1][:2] == pytest.approx((0.5, 0.0))  # the staging pose
        assert rig.backward_m() == pytest.approx(0.5, abs=1e-6)
        assert rig.on_plate() and rig.battery().charging  # type: ignore[union-attr]
        assert rig.ctrl.attempts == 1 and rig.events == [] and rig.spoken == []
        # Backward steps were slow: 0.05 m/s = speed 1/3 of the 0.15 m/s gait.
        assert all(abs(lin) == pytest.approx(1 / 3) for lin, _a, _d in rig.robot.log)
        # Charging: stays below 90 %, leaves at 90 %.
        rig.sensor.set_pct(89.0)
        for _ in range(5):
            rig.ctrl.tick()
            await asyncio.sleep(0)
        assert rig.ctrl.state == "charging"
        rig.sensor.set_pct(90.0)
        await rig.run_until("idle")
        assert rig.states[-2:] == ["undocking", "idle"]
        assert rig.forward_m() == pytest.approx(0.3)
        assert not rig.on_plate() and not rig.ctrl.return_pending
        # And it does not go straight back (90 % > 20 %).
        for _ in range(5):
            rig.ctrl.tick()
            await asyncio.sleep(0)
        assert rig.ctrl.state == "idle" and not rig.ctrl.running

    asyncio.run(body())


def test_approach_uses_lidar_wall_distance_and_retries_then_succeeds(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=10.0)
        rig.nav_error_m = 0.10  # Nav2 stopped 10 cm short of the staging pose
        rig.contact_after_attempt = 2  # first attempt: contacts dirty, no current
        rig.ctrl.tick()
        await rig.ctrl.wait()
        assert rig.ctrl.state == "charging" and rig.ctrl.attempts == 2
        # Attempt 1: 0.60 m to the wall reference (budget allows 0.5 + 0.1 overshoot),
        # 10 s verify, 0.2 m back off; attempt 2: 0.2 m again.
        assert rig.states.count("approaching") == 2 and rig.states.count("verifying") == 2
        assert rig.backward_m() == pytest.approx(0.6 + 0.2, abs=1e-6)
        assert rig.forward_m() == pytest.approx(0.2)
        assert rig.t == pytest.approx(10.0, abs=0.6)  # one failed verify window
        assert rig.events == []

    asyncio.run(body())


def test_approach_fails_after_three_attempts(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=18.0, powered=False)
        rig.ctrl.tick()
        await rig.ctrl.wait()
        assert rig.ctrl.state == "failed" and rig.ctrl.attempts == 3
        assert rig.states.count("approaching") == 3
        assert rig.spoken == ["I could not reach the charger."]
        assert rig.events == [{"t": "event", "ev": "dock_failed", "reason": "no_current",
                               "attempts": 3}]  # fmt: skip
        assert rig.t == pytest.approx(30.0, abs=1.6)  # three 10 s verify windows
        assert rig.forward_m() == pytest.approx(0.4)  # two back-offs, no undock
        # Failed is sticky: no new run, and battery_low fires at 15 % because it cannot help.
        rig.sensor.set_pct(14.0)
        for _ in range(3):
            rig.ctrl.tick()
            await asyncio.sleep(0)
        assert rig.ctrl.state == "failed" and not rig.ctrl.running
        assert rig.events[-1]["ev"] == "battery_low" and rig.events[-1]["pct"] == 14
        assert rig.events[-1]["dock"] == "failed" and len(rig.events) == 2
        # After the cooldown it is allowed to try again on its own.
        rig.t += 900.0
        rig.ctrl.tick()
        assert rig.ctrl.state == "idle"
        rig.dist = 2.0  # the owner carried it off the plate and plugged the brick in
        rig.powered = True
        await rig.run_until("charging")
        assert rig.ctrl.status()["last_error"] is None and rig.ctrl.attempts == 1
        # Still parked on the plate when power comes back: current seen -> charging, no run.
        rig3 = Rig(tmp_path / "c", pct=18.0, powered=False)
        rig3.ctrl.tick()
        await rig3.ctrl.wait()
        assert rig3.ctrl.state == "failed" and rig3.on_plate()
        rig3.powered = True
        n = len(rig3.robot.log)
        rig3.ctrl.tick()
        assert rig3.ctrl.state == "charging" and len(rig3.robot.log) == n

    asyncio.run(body())


def test_nav_failure_is_a_dock_failure(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=12.0)
        rig.nav_backend.blocked.add((0.5, 0.0))
        rig.ctrl.tick()
        await rig.ctrl.wait()
        assert rig.ctrl.state == "failed"
        assert rig.events[0]["reason"] == "nav:blocked" and rig.robot.log == []
        # A daemon `go` clears the failure and tries again.
        rig.nav_backend.blocked.clear()
        assert rig.ctrl.go() == {"ok": True, "reason": "queued"}
        await rig.run_until("charging")

    asyncio.run(body())


def test_busy_blocks_docking_until_free(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=15.0)
        rig.busy_flag = True
        for _ in range(10):
            rig.ctrl.tick()
            await asyncio.sleep(0)
        assert rig.ctrl.state == "idle" and not rig.ctrl.running
        rig.busy_flag = False
        await rig.run_until("charging")
        # Above the threshold nothing happens even when free.
        rig2 = Rig(tmp_path / "b", pct=21.0)
        for _ in range(10):
            rig2.ctrl.tick()
            await asyncio.sleep(0)
        assert rig2.ctrl.state == "idle" and not rig2.ctrl.running
        # ...unless the daemon says go (which also ignores busy).
        rig2.busy_flag = True
        assert rig2.ctrl.go()["ok"] is True
        await rig2.run_until("charging")
        assert rig2.ctrl.go() == {"ok": True, "reason": "charging"}

    asyncio.run(body())


def test_wake_pulls_it_off_the_dock_and_it_returns_afterwards(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=15.0)
        await rig.run_until("charging")
        rig.sensor.set_pct(45.0)  # still well below 90 %
        rig.busy_flag = True  # the wake handler is running
        left = await rig.ctrl.leave_dock_for("wake")
        assert left and rig.ctrl.state == "idle" and rig.ctrl.return_pending
        assert rig.ctrl.leave_reason == "wake"
        assert not rig.on_plate() and rig.forward_m() == pytest.approx(0.3)
        for _ in range(10):  # while the wake runs it stays put
            rig.ctrl.tick()
            await asyncio.sleep(0)
        assert rig.ctrl.state == "idle" and not rig.ctrl.running
        rig.busy_flag = False  # wake finished
        await rig.run_until("charging")
        assert not rig.ctrl.return_pending
        # Not charging: nothing to leave.
        rig.sensor.set_pct(95.0)
        await rig.run_until("idle")
        assert await rig.ctrl.leave_dock_for("wake") is False

    asyncio.run(body())


def test_daemon_leave_does_not_return_and_attention_mid_run_interrupts(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=15.0)
        await rig.run_until("charging")
        rig.sensor.set_pct(50.0)
        assert await rig.ctrl.leave_dock_for("daemon") is True
        assert rig.ctrl.state == "idle" and not rig.ctrl.return_pending
        for _ in range(10):
            rig.ctrl.tick()
            await asyncio.sleep(0)
        assert not rig.ctrl.running  # 50 % > 20 %: stays off the dock
        # Interrupt a run in progress (stuck in the verify wait).
        rig2 = Rig(tmp_path / "b", pct=15.0, powered=False)
        rig2.ctrl.tick()
        for _ in range(500):
            if rig2.ctrl.state == "verifying":
                break
            await asyncio.sleep(0)
        assert rig2.ctrl.state == "verifying"
        assert await rig2.ctrl.leave_dock_for("attention") is True
        await rig2.ctrl.wait()
        assert rig2.ctrl.state == "idle" and rig2.ctrl.return_pending and not rig2.ctrl.running
        assert rig2.events == []  # interrupted, not failed

    asyncio.run(body())


def test_battery_low_event_when_dock_disabled_or_not_enrolled(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=16.0, enabled=False)
        assert rig.ctrl.state == "off"
        rig.ctrl.tick()
        assert rig.events == []
        rig.sensor.set_pct(15.0)
        rig.ctrl.tick()
        rig.ctrl.tick()
        assert rig.events == [{"t": "event", "ev": "battery_low", "pct": 15, "dock": "off"}]
        rig.sensor.set_pct(12.0)
        rig.ctrl.tick()
        assert len(rig.events) == 1  # once, until it recovers
        rig.sensor.set_pct(25.0)
        rig.ctrl.tick()
        rig.sensor.set_pct(14.0)
        rig.ctrl.tick()
        assert len(rig.events) == 2
        assert not rig.ctrl.running and rig.robot.log == []
        # Enabled but never enrolled: cannot help itself either.
        rig2 = Rig(tmp_path / "b", pct=14.0, enrol=False)
        rig2.ctrl.tick()
        assert rig2.events[-1]["ev"] == "battery_low" and rig2.events[-1]["dock"] == "idle"
        assert rig2.ctrl.go() == {"ok": False, "reason": "not_enrolled"}

    asyncio.run(body())


def test_enable_toggle_and_manual_placement(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=15.0)
        await rig.run_until("charging")
        rig.ctrl.enabled = False  # settings {"dock": false} while on the plate
        assert rig.ctrl.state == "off" and rig.ctrl.go() == {"ok": False, "reason": "dock_off"}
        rig.ctrl.tick()
        assert rig.robot.log and not rig.ctrl.running  # stays put, keeps charging physically
        n = len(rig.robot.log)
        rig.ctrl.enabled = True
        rig.ctrl.tick()
        assert rig.ctrl.state == "charging" and len(rig.robot.log) == n  # current seen: resume
        # Disabling mid-run cancels the run.
        rig2 = Rig(tmp_path / "b", pct=15.0, powered=False)
        rig2.ctrl.tick()
        for _ in range(500):
            if rig2.ctrl.state == "verifying":
                break
            await asyncio.sleep(0)
        rig2.ctrl.enabled = False
        await rig2.ctrl.wait()
        assert rig2.ctrl.state == "off" and rig2.events == []
        # Picked up by hand while charging: current gone for 5 s -> idle.
        rig3 = Rig(tmp_path / "c", pct=15.0)
        await rig3.run_until("charging")
        rig3.dist = 1.0
        rig3.ctrl.tick()
        assert rig3.ctrl.state == "charging"
        rig3.t += 6.0
        rig3.ctrl.tick()
        assert rig3.ctrl.state == "idle"
        # Put on the plate by hand while idle -> charging without a run.
        rig3.sensor.set_pct(60.0)
        rig3.dist = WALL_M
        rig3.ctrl.tick()
        assert rig3.ctrl.state == "charging" and not rig3.ctrl.running

    asyncio.run(body())


def test_stale_charging_sample_after_undock_is_ignored(tmp_path: Path) -> None:
    async def body() -> None:
        rig = Rig(tmp_path, pct=15.0)
        await rig.run_until("charging")
        rig.sensor.set_pct(95.0)
        rig.t += 1.0
        await rig.run_until("idle")
        assert rig.forward_m() == pytest.approx(0.3)
        # The pack is sampled every 0.5 s: the first ticks after driving off still see the
        # on-dock current. That must not flip us back to "charging".
        stale = BatteryState(12.4, 2.5, 95.0, True, rig.t)
        rig.ctrl._battery = lambda: stale  # type: ignore[method-assign]
        for _ in range(5):
            rig.ctrl.tick()
            await asyncio.sleep(0)
        assert rig.ctrl.state == "idle"
        # A genuinely newer sample with current (someone carried it back) does count.
        fresh = BatteryState(12.4, 2.5, 95.0, True, rig.t + 1.0)
        rig.ctrl._battery = lambda: fresh  # type: ignore[method-assign]
        rig.ctrl.tick()
        assert rig.ctrl.state == "charging"

    asyncio.run(body())
