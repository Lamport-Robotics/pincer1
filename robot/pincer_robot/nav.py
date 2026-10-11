"""Mapping, localisation and room-to-room navigation on the chassis' ROS2 stack.

Layers:

* ``RoomRegistry`` (pure) - ``~/.pincer1/rooms.json``: places the owner named by standing
  there (``name_room``): a map pose plus a 2 m radius, or a polygon. ``room_at(x, y)``
  answers "which room is this point in" (polygon first, then the nearest radius).
* ``NavBackend`` - what the robot stack must provide: ``Ros2Nav`` wraps slam_toolbox
  (mapping), ``nav2_map_server`` (``map_saver_cli``), AMCL / slam localisation
  (``/amcl_pose`` or the ``map -> base_link`` transform) and Nav2's
  ``BasicNavigator`` (``nav2_simple_commander``) for goals. Every rclpy / nav2 import is
  lazy and lives in that class. ``MockNav`` is a teleporting fake for the laptop and tests.
* ``Navigator`` (pure logic over a backend) - ``start_mapping``, ``save_map``,
  ``localize`` -> (x, y, yaw, room | None), ``goto(room)`` with a timeout,
  ``find_owner()``: go to the room where the owner was last seen, else the priority list
  (``bedroom``, ``desk``, ``living``) then the rest; at each stop turn through 360 degrees
  and watch for a person cluster + face for ``dwell_s`` (10 s); return as soon as found.
* ``room_match_from_localization`` - the map-based room lock: 1.0 when the robot is
  localised (pose covariance below ``cov_threshold``) inside a named room or the mapped
  area, 0.0 when AMCL has not converged or the pose is nowhere we know.

What a legged base changes (none of it verifiable on the laptop): slam_toolbox map
quality under the hexapod's gait wobble (the lidar is not level while walking), AMCL on a
small, noisy 2D lidar in cluttered rooms, and the Nav2 parameters - footprint, velocity
limits, recovery behaviours - must be written for the hexapod's gait, not a wheeled
robot's defaults. ``deploy/jetson`` is where those launch files and params belong.
"""

from __future__ import annotations

import asyncio
import json
import logging
import math
import os
import shutil
import subprocess
import time
from collections.abc import Awaitable, Callable, Sequence
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Protocol

log = logging.getLogger("pincer.nav")

DEFAULT_ROOM_PRIORITY: tuple[str, ...] = ("bedroom", "desk", "living")


# --- rooms ------------------------------------------------------------------------------------


@dataclass
class RoomDef:
    name: str
    x: float
    y: float
    yaw: float = 0.0
    radius: float = 2.0
    polygon: list[tuple[float, float]] | None = None
    created_ts: float = field(default_factory=time.time)

    def contains(self, x: float, y: float) -> bool:
        if self.polygon and len(self.polygon) >= 3:
            return _point_in_polygon(x, y, self.polygon)
        return math.hypot(x - self.x, y - self.y) <= self.radius

    def distance(self, x: float, y: float) -> float:
        return math.hypot(x - self.x, y - self.y)

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        if self.polygon is not None:
            d["polygon"] = [list(p) for p in self.polygon]
        return d

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> RoomDef:
        poly = d.get("polygon")
        return cls(
            name=str(d["name"]),
            x=float(d["x"]),
            y=float(d["y"]),
            yaw=float(d.get("yaw", 0.0)),
            radius=float(d.get("radius", 2.0)),
            polygon=[(float(p[0]), float(p[1])) for p in poly] if poly else None,
            created_ts=float(d.get("created_ts", 0.0)),
        )


def _point_in_polygon(x: float, y: float, poly: Sequence[tuple[float, float]]) -> bool:
    inside = False
    n = len(poly)
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y):
            x_cross = (xj - xi) * (y - yi) / ((yj - yi) or 1e-12) + xi
            if x < x_cross:
                inside = not inside
        j = i
    return inside


class RoomRegistry:
    def __init__(self, path: Path) -> None:
        self.path = path.expanduser()
        self.rooms: dict[str, RoomDef] = {}
        self.load()

    def load(self) -> None:
        self.rooms = {}
        if not self.path.exists():
            return
        try:
            data = json.loads(self.path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            log.error("rooms.json unreadable (%s); starting empty", exc)
            return
        for d in data.get("rooms", []):
            try:
                r = RoomDef.from_dict(d)
            except (KeyError, TypeError, ValueError):
                continue
            self.rooms[r.name] = r

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps({"rooms": [r.to_dict() for r in self.rooms.values()]}, indent=1))
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)

    def add(
        self,
        name: str,
        x: float,
        y: float,
        yaw: float = 0.0,
        *,
        radius: float = 2.0,
        polygon: Sequence[Sequence[float]] | None = None,
    ) -> RoomDef:
        name = name.strip().lower()
        if not name:
            raise ValueError("room name is empty")
        poly = None
        if polygon:
            poly = [(float(p[0]), float(p[1])) for p in polygon]
            if len(poly) < 3:
                raise ValueError("polygon needs at least 3 points")
        r = RoomDef(name=name, x=x, y=y, yaw=yaw, radius=max(0.5, float(radius)), polygon=poly)
        self.rooms[name] = r
        self.save()
        return r

    def remove(self, name: str) -> bool:
        if self.rooms.pop(name, None) is None:
            return False
        self.save()
        return True

    def names(self) -> list[str]:
        return list(self.rooms)

    def get(self, name: str) -> RoomDef | None:
        return self.rooms.get(name.strip().lower())

    def room_at(self, x: float, y: float) -> str | None:
        """Polygon rooms win; otherwise the nearest room whose radius contains the point."""
        for r in self.rooms.values():
            if r.polygon and r.contains(x, y):
                return r.name
        best: RoomDef | None = None
        for r in self.rooms.values():
            if r.polygon is None and r.contains(x, y):
                if best is None or r.distance(x, y) < best.distance(x, y):
                    best = r
        return best.name if best else None

    def ordered(self, priority: Sequence[str], first: str | None = None) -> list[str]:
        """Visit order: ``first`` (last seen), then ``priority`` names that exist, then the rest."""
        out: list[str] = []
        for name in [first, *priority, *self.rooms]:
            if name and name in self.rooms and name not in out:
                out.append(name)
        return out


# --- backend -----------------------------------------------------------------------------------


@dataclass
class Localization:
    x: float
    y: float
    yaw: float
    cov_xy: float = 0.0  # max of the x/y variances from the pose covariance
    score: float | None = None  # backend match score 0..1 if it has one (slam_toolbox)
    ts: float = 0.0
    in_map: bool = True  # inside the map's known free space


@dataclass
class NavResult:
    ok: bool
    reason: str
    secs: float
    room: str | None = None


class NavBackend(Protocol):
    def start_mapping(self) -> None: ...

    def stop_mapping(self) -> None: ...

    def mapping(self) -> bool: ...

    def save_map(self, path: Path) -> bool: ...

    def map_available(self) -> bool: ...

    def localize(self) -> Localization | None: ...

    def goto(self, x: float, y: float, yaw: float, timeout_s: float) -> NavResult: ...

    def cancel(self) -> None: ...

    def close(self) -> None: ...


class MockNav:
    """Teleporting fake: ``goto`` moves the pose instantly (or fails for ``blocked`` rooms)."""

    def __init__(self, *, has_map: bool = False, pose: tuple[float, float, float] = (0, 0, 0)):
        self.has_map = has_map
        self.pose = pose
        self._mapping = False
        self.goals: list[tuple[float, float, float]] = []
        self.saved: list[Path] = []
        self.blocked: set[tuple[float, float]] = set()
        self.cov_xy = 0.05
        self.score: float | None = None
        self.in_map = True
        self.travel_s = 0.0  # simulated travel time per goal (tests keep it 0)

    def start_mapping(self) -> None:
        self._mapping = True

    def stop_mapping(self) -> None:
        self._mapping = False

    def mapping(self) -> bool:
        return self._mapping

    def save_map(self, path: Path) -> bool:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.with_suffix(".yaml").write_text(f"image: {path.name}.pgm\nresolution: 0.05\n")
        self.saved.append(path)
        self.has_map = True
        return True

    def map_available(self) -> bool:
        return self.has_map

    def localize(self) -> Localization | None:
        if not self.has_map:
            return None
        x, y, yaw = self.pose
        return Localization(x, y, yaw, self.cov_xy, self.score, time.time(), self.in_map)

    def goto(self, x: float, y: float, yaw: float, timeout_s: float) -> NavResult:
        self.goals.append((x, y, yaw))
        if (x, y) in self.blocked:
            return NavResult(False, "blocked", 0.0)
        if self.travel_s > timeout_s:
            return NavResult(False, "timeout", timeout_s)
        if self.travel_s:
            time.sleep(self.travel_s)
        self.pose = (x, y, yaw)
        return NavResult(True, "arrived", self.travel_s)

    def cancel(self) -> None:
        pass

    def close(self) -> None:
        pass


class Ros2Nav:
    """slam_toolbox + Nav2 through rclpy. Constructed only on the robot.

    Topics / tools (ROS2 Humble):
      mapping      ``ros2 launch slam_toolbox online_async_launch.py`` (subprocess)
      save         ``ros2 run nav2_map_server map_saver_cli -f <path>``
      localisation ``/amcl_pose`` (geometry_msgs/PoseWithCovarianceStamped) when Nav2's
                   AMCL runs; slam_toolbox publishes ``/pose`` with the same type while
                   mapping. Both are subscribed; the newest wins.
      goals        ``nav2_simple_commander.BasicNavigator.goToPose`` (map frame).
    The Nav2 bringup itself (``ros2 launch nav2_bringup bringup_launch.py map:=...``) is a
    systemd unit in deploy/jetson, not started from here.
    """

    def __init__(
        self,
        maps_dir: Path,
        *,
        map_name: str = "home",
        node_name: str = "pincer_nav",
        slam_launch: Sequence[str] = ("ros2", "launch", "slam_toolbox", "online_async_launch.py"),
    ) -> None:
        import rclpy  # type: ignore[import-not-found]
        from geometry_msgs.msg import PoseWithCovarianceStamped  # type: ignore[import-not-found]
        from nav2_simple_commander.robot_navigator import (  # type: ignore[import-not-found]
            BasicNavigator,
            TaskResult,
        )

        if not rclpy.ok():
            rclpy.init()
        self._rclpy = rclpy
        self._TaskResult = TaskResult
        self._nav = BasicNavigator(node_name=node_name)
        self._pose_msg: Any | None = None
        self._pose_ts = 0.0
        for topic in ("/amcl_pose", "/pose"):
            self._nav.create_subscription(PoseWithCovarianceStamped, topic, self._on_pose, 10)
        self.maps_dir = maps_dir.expanduser()
        self.map_name = map_name
        self._slam_launch = list(slam_launch)
        self._slam_proc: subprocess.Popen[bytes] | None = None

    # -- pose --------------------------------------------------------------------------------
    def _on_pose(self, msg: Any) -> None:
        self._pose_msg = msg
        self._pose_ts = time.time()

    def _spin_once(self) -> None:
        self._rclpy.spin_once(self._nav, timeout_sec=0.05)

    def localize(self) -> Localization | None:
        self._spin_once()
        msg = self._pose_msg
        if msg is None or time.time() - self._pose_ts > 5.0:
            return None
        p = msg.pose.pose.position
        q = msg.pose.pose.orientation
        yaw = math.atan2(2.0 * (q.w * q.z + q.x * q.y), 1.0 - 2.0 * (q.y * q.y + q.z * q.z))
        cov = list(msg.pose.covariance)
        cov_xy = max(float(cov[0]), float(cov[7])) if len(cov) >= 36 else 0.0
        return Localization(float(p.x), float(p.y), float(yaw), cov_xy, None, self._pose_ts, True)

    # -- mapping -----------------------------------------------------------------------------
    def start_mapping(self) -> None:
        if self._slam_proc is not None and self._slam_proc.poll() is None:
            return
        self._slam_proc = subprocess.Popen(self._slam_launch)
        log.info("slam_toolbox started (pid %d)", self._slam_proc.pid)

    def stop_mapping(self) -> None:
        if self._slam_proc is not None and self._slam_proc.poll() is None:
            self._slam_proc.terminate()
            try:
                self._slam_proc.wait(10)
            except subprocess.TimeoutExpired:
                self._slam_proc.kill()
        self._slam_proc = None

    def mapping(self) -> bool:
        return self._slam_proc is not None and self._slam_proc.poll() is None

    def save_map(self, path: Path) -> bool:
        path.parent.mkdir(parents=True, exist_ok=True)
        if shutil.which("ros2") is None:
            log.error("ros2 CLI not on PATH; cannot save the map")
            return False
        res = subprocess.run(
            ["ros2", "run", "nav2_map_server", "map_saver_cli", "-f", str(path)],
            capture_output=True,
            timeout=60,
        )
        if res.returncode != 0:
            log.error("map_saver_cli failed: %s", res.stderr.decode(errors="replace")[-400:])
            return False
        return path.with_suffix(".yaml").exists()

    def map_available(self) -> bool:
        return (self.maps_dir / f"{self.map_name}.yaml").exists()

    # -- goals -------------------------------------------------------------------------------
    def goto(self, x: float, y: float, yaw: float, timeout_s: float) -> NavResult:
        from geometry_msgs.msg import PoseStamped  # type: ignore[import-not-found]

        goal = PoseStamped()
        goal.header.frame_id = "map"
        goal.header.stamp = self._nav.get_clock().now().to_msg()
        goal.pose.position.x = float(x)
        goal.pose.position.y = float(y)
        goal.pose.orientation.z = math.sin(yaw / 2.0)
        goal.pose.orientation.w = math.cos(yaw / 2.0)
        t0 = time.time()
        if not self._nav.goToPose(goal):
            return NavResult(False, "rejected", 0.0)
        while not self._nav.isTaskComplete():
            if time.time() - t0 > timeout_s:
                self._nav.cancelTask()
                return NavResult(False, "timeout", time.time() - t0)
            time.sleep(0.2)
        result = self._nav.getResult()
        ok = result == self._TaskResult.SUCCEEDED
        return NavResult(ok, "arrived" if ok else str(result).lower(), time.time() - t0)

    def cancel(self) -> None:
        try:
            self._nav.cancelTask()
        except Exception as exc:  # pragma: no cover
            log.warning("cancel failed: %s", exc)

    def close(self) -> None:
        self.stop_mapping()


def make_nav_backend(maps_dir: Path, *, mock: bool) -> NavBackend:
    if mock:
        return MockNav()
    try:
        return Ros2Nav(maps_dir)
    except Exception as exc:  # ImportError (no ROS2 / Nav2), rclpy init failure
        log.warning("Nav2 unavailable (%s): navigation disabled (mock backend)", exc)
        return MockNav()


# --- room lock from localisation ----------------------------------------------------------


def room_match_from_localization(
    loc: Localization | None, rooms: RoomRegistry, cov_threshold: float = 0.25
) -> float:
    """1.0 = localised inside a known place; 0.0 = not converged / unknown place."""
    if loc is None:
        return 0.0
    if loc.cov_xy > cov_threshold:
        return 0.0
    if loc.score is not None and loc.score < 0.5:
        return 0.0
    if rooms.room_at(loc.x, loc.y) is not None:
        return 1.0
    return 1.0 if loc.in_map else 0.0


# --- high level --------------------------------------------------------------------------------


@dataclass
class FindResult:
    found: bool
    room: str | None
    travelled: bool
    visited: list[str]
    secs: float


class Navigator:
    def __init__(
        self,
        backend: NavBackend,
        rooms: RoomRegistry,
        maps_dir: Path,
        *,
        map_name: str = "home",
        goal_timeout_s: float = 90.0,
        dwell_s: float = 10.0,
        room_radius_m: float = 2.0,
        priority: Sequence[str] = DEFAULT_ROOM_PRIORITY,
        localized_cov: float = 0.25,
    ) -> None:
        self.backend = backend
        self.rooms = rooms
        self.maps_dir = maps_dir.expanduser()
        self.map_name = map_name
        self.goal_timeout_s = goal_timeout_s
        self.dwell_s = dwell_s
        self.room_radius_m = room_radius_m
        self.priority = list(priority)
        self.localized_cov = localized_cov
        self._lock = asyncio.Lock()
        self.last_localization: Localization | None = None

    # -- mapping -----------------------------------------------------------------------------
    def start_mapping(self) -> None:
        self.backend.start_mapping()

    def save_map(self, name: str | None = None) -> Path | None:
        name = (name or self.map_name).strip() or self.map_name
        path = self.maps_dir / name
        ok = self.backend.save_map(path)
        if ok:
            self.map_name = name
            self.backend.stop_mapping()
            log.info("map saved: %s", path)
            return path
        return None

    @property
    def available(self) -> bool:
        return self.backend.map_available()

    def status(self) -> dict[str, Any]:
        loc = self.localize_raw()
        return {
            "mapping": self.backend.mapping(),
            "map": self.available,
            "map_name": self.map_name,
            "rooms": self.rooms.names(),
            "pose": None if loc is None else {"x": round(loc.x, 2), "y": round(loc.y, 2),
                                              "yaw": round(loc.yaw, 3)},  # fmt: skip
            "room": None if loc is None else self.rooms.room_at(loc.x, loc.y),
            "localized": loc is not None and loc.cov_xy <= self.localized_cov,
        }

    # -- localisation ------------------------------------------------------------------------
    def localize_raw(self) -> Localization | None:
        try:
            loc = self.backend.localize()
        except Exception as exc:
            log.error("localize failed: %s", exc)
            loc = None
        self.last_localization = loc
        return loc

    def localize(self) -> tuple[float, float, float, str | None] | None:
        loc = self.localize_raw()
        if loc is None:
            return None
        return loc.x, loc.y, loc.yaw, self.rooms.room_at(loc.x, loc.y)

    def current_room(self) -> str | None:
        loc = self.localize_raw()
        return None if loc is None else self.rooms.room_at(loc.x, loc.y)

    def room_match(self) -> float | None:
        """Map-based room lock; None when there is no map (caller falls back to the fingerprint)."""
        if not self.available:
            return None
        return room_match_from_localization(self.localize_raw(), self.rooms, self.localized_cov)

    def name_room(
        self,
        name: str,
        *,
        radius: float | None = None,
        polygon: Sequence[Sequence[float]] | None = None,
    ) -> RoomDef:
        loc = self.localize_raw()
        if loc is None:
            raise RuntimeError("not localised: build/load a map first")
        return self.rooms.add(
            name, loc.x, loc.y, loc.yaw, radius=radius or self.room_radius_m, polygon=polygon
        )

    # -- motion ------------------------------------------------------------------------------
    async def goto(self, room: str, timeout_s: float | None = None) -> NavResult:
        r = self.rooms.get(room)
        if r is None:
            return NavResult(False, "unknown_room", 0.0, room)
        if not self.available:
            return NavResult(False, "no_map", 0.0, room)
        timeout = self.goal_timeout_s if timeout_s is None else max(1.0, timeout_s)
        async with self._lock:
            res = await asyncio.to_thread(self.backend.goto, r.x, r.y, r.yaw, timeout)
        res.room = room
        log.info("goto %s: %s (%.0fs)", room, res.reason, res.secs)
        return res

    async def find_owner(
        self,
        visible: Callable[[], bool],
        *,
        last_seen_room: str | None = None,
        look_around: Callable[[], Awaitable[None]] | None = None,
        max_secs: float = 120.0,
        dwell_s: float | None = None,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
    ) -> FindResult:
        """Visit rooms until ``visible()`` is True; ``look_around`` (a 360 degree turn)
        runs once per stop while ``visible`` is polled for ``dwell_s``."""
        t0 = clock()
        dwell = self.dwell_s if dwell_s is None else dwell_s
        visited: list[str] = []
        if visible():
            return FindResult(True, self.current_room(), False, visited, 0.0)
        if not self.available:
            return FindResult(False, None, False, visited, clock() - t0)
        travelled = False
        for room in self.rooms.ordered(self.priority, last_seen_room):
            remaining = max_secs - (clock() - t0)
            if remaining <= 1.0:
                break
            res = await self.goto(room, timeout_s=min(self.goal_timeout_s, remaining))
            visited.append(room)
            travelled = travelled or res.ok or res.secs > 0
            if not res.ok:
                continue
            found = await self._dwell(visible, look_around, min(dwell, max_secs - (clock() - t0)),
                                      clock, sleep)  # fmt: skip
            if found:
                return FindResult(True, room, travelled, visited, clock() - t0)
        return FindResult(False, None, travelled, visited, clock() - t0)

    async def _dwell(
        self,
        visible: Callable[[], bool],
        look_around: Callable[[], Awaitable[None]] | None,
        secs: float,
        clock: Callable[[], float],
        sleep: Callable[[float], Awaitable[None]],
    ) -> bool:
        t0 = clock()
        turn: asyncio.Task[None] | None = None
        if look_around is not None:
            turn = asyncio.create_task(look_around())
        try:
            while clock() - t0 < secs:
                if visible():
                    return True
                await sleep(0.5)
            return visible()
        finally:
            if turn is not None:
                if not turn.done():
                    turn.cancel()
                try:
                    await turn
                except (asyncio.CancelledError, Exception):
                    pass

    def close(self) -> None:
        self.backend.close()
