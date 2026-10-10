"""Body motion: a thin, safety-clamped wrapper in the ClaudeGO style.

``RobotInterface`` mirrors claudego/robot.py: backends only implement ``_drive``
(hold a normalised Twist for a duration), ``stop`` and ``get_status``; the public
``walk`` / ``turn`` clamp every request inside a survivable envelope. ``MockRobot``
logs; ``HexapodRobot`` (ros_backend.py) publishes geometry_msgs/Twist on /cmd_vel.

``MotionController`` adds the two gestures Pincer1 needs:

* ``attention(bearing, step_m)`` - turn toward the owner (bearing from the lidar cluster
  or the face x-position) and step forward 10 cm (20 cm for the wake-up alarm).
* ``idle_sway()``        - a small left/right twitch so the body reads as alive.

Both are async (blocking drive loops run in a thread) and serialised by a lock.
"""

from __future__ import annotations

import asyncio
import logging
import math
from abc import ABC, abstractmethod
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any, TypeVar

log = logging.getLogger("pincer.motion")

T = TypeVar("T")

# --- Safety envelope (same numbers as ClaudeGO) --------------------------------------------
MAX_DISTANCE_M = 2.0
MAX_TURN_DEG = 180.0
MAX_SPEED = 1.0
SAFE_TILT_DEG = 35.0
MIN_FRONT_CLEARANCE_M = 0.30
GAIT_SPEED_MPS = 0.15  # at speed 1.0
TURN_RATE_DPS = 45.0  # at speed 1.0

ATTENTION_STEP_M = 0.10


def _clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


@dataclass
class RobotStatus:
    tilt_deg: float = 0.0
    battery_pct: float = 100.0
    front_clearance_m: float = 99.0
    is_moving: bool = False
    note: str = ""


class RobotInterface(ABC):
    @abstractmethod
    def get_status(self) -> RobotStatus: ...

    @abstractmethod
    def _drive(self, lin_x: float, ang_z: float, duration_s: float) -> None:
        """Hold a normalised velocity (-1..1) for ``duration_s`` then stop."""

    @abstractmethod
    def stop(self) -> None: ...

    def walk(self, direction: str, distance_m: float, speed: float = 0.5) -> str:
        distance_m = _clamp(distance_m, 0.0, MAX_DISTANCE_M)
        speed = _clamp(speed, 0.1, MAX_SPEED)
        status = self.get_status()
        if status.tilt_deg > SAFE_TILT_DEG:
            return f"REFUSED: body tilted {status.tilt_deg:.0f} deg (limit {SAFE_TILT_DEG:.0f})."
        sign = {"forward": 1.0, "backward": -1.0}.get(direction)
        if sign is None:
            return f"REFUSED: unknown direction '{direction}'."
        if direction == "forward" and status.front_clearance_m < MIN_FRONT_CLEARANCE_M:
            return f"REFUSED: obstacle {status.front_clearance_m:.2f} m ahead."
        duration = distance_m / (GAIT_SPEED_MPS * speed)
        self._drive(lin_x=sign * speed, ang_z=0.0, duration_s=duration)
        return f"OK: walked {direction} {distance_m:.2f} m."

    def turn(self, direction: str, angle_deg: float, speed: float = 0.4) -> str:
        angle_deg = _clamp(angle_deg, 0.0, MAX_TURN_DEG)
        speed = _clamp(speed, 0.1, MAX_SPEED)
        sign = {"left": 1.0, "right": -1.0}.get(direction)
        if sign is None:
            return f"REFUSED: turn direction must be left/right, got '{direction}'."
        duration = angle_deg / (TURN_RATE_DPS * speed)
        self._drive(lin_x=0.0, ang_z=sign * speed, duration_s=duration)
        return f"OK: turned {direction} {angle_deg:.0f} deg."


class MockRobot(RobotInterface):
    def __init__(self) -> None:
        self._status = RobotStatus(note="mock backend")
        self.log: list[tuple[float, float, float]] = []

    def get_status(self) -> RobotStatus:
        return self._status

    def _drive(self, lin_x: float, ang_z: float, duration_s: float) -> None:
        self._status.is_moving = True
        self.log.append((lin_x, ang_z, duration_s))
        log.info("MOCK drive lin_x=%.2f ang_z=%.2f for %.2fs", lin_x, ang_z, duration_s)
        self._status.is_moving = False

    def stop(self) -> None:
        self._status.is_moving = False


class MotionController:
    def __init__(self, robot: RobotInterface) -> None:
        self.robot = robot
        self._lock = asyncio.Lock()

    def _attention_blocking(self, bearing_rad: float | None, step_m: float) -> str:
        notes: list[str] = []
        if bearing_rad is not None and abs(bearing_rad) > math.radians(5.0):
            direction = "left" if bearing_rad > 0 else "right"
            notes.append(self.robot.turn(direction, abs(math.degrees(bearing_rad)), speed=0.5))
        if step_m > 0:
            notes.append(self.robot.walk("forward", step_m, speed=0.5))
        return "; ".join(notes)

    async def attention(self, bearing_rad: float | None, step_m: float = ATTENTION_STEP_M) -> str:
        async with self._lock:
            result = await asyncio.to_thread(self._attention_blocking, bearing_rad, step_m)
        log.info("attention: %s", result)
        return result

    def _sway_blocking(self) -> None:
        self.robot.turn("left", 4.0, speed=0.3)
        self.robot.turn("right", 8.0, speed=0.3)
        self.robot.turn("left", 4.0, speed=0.3)

    async def idle_sway(self) -> None:
        if self._lock.locked():
            return
        async with self._lock:
            await asyncio.to_thread(self._sway_blocking)

    async def run_exclusive(self, fn: Callable[..., T], *args: Any) -> T:
        """Run a blocking drive routine in a thread while holding the motion lock
        (used by the dock approach so attention / sway cannot interleave with it)."""
        async with self._lock:
            return await asyncio.to_thread(fn, *args)

    @property
    def busy(self) -> bool:
        return self._lock.locked()

    async def stop(self) -> None:
        self.robot.stop()
