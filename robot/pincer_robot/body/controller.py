"""The body controller: velocity in, servo packets out, at a fixed tick.

``Body`` owns the geometry, the gait and the pose, and turns a ``Velocity`` command into joint
angles
for the eighteen leg servos. ``tick(dt)`` is the only thing that moves anything. Whatever drives the
servos (a ``Bus`` on the real body, a ``Simulation`` without one) is injected, so the same
controller
runs in tests, on a bench with a single leg, and on the robot.

``BodyBackend`` plugs this into the ``RobotInterface`` the rest of the service already uses
(walk / turn / attention / idle sway), replacing the external gait controller that backend talked
to.
"""

from __future__ import annotations

import logging
import math
import threading
import time
from dataclasses import dataclass, field
from typing import Protocol

from ..motion import GAIT_SPEED_MPS, TURN_RATE_DPS, RobotInterface, RobotStatus
from .gait import GaitParams, GaitState, Velocity, feet_down, step
from .geometry import BodyGeometry
from .kinematics import BodyPose, JointAngles, ReachError, Vec3, solve_body
from .servo import Bus
from .sim import Simulation

log = logging.getLogger("pincer.body")

TICK_HZ = 50.0


class Actuator(Protocol):
    """Where joint angles go. ``Bus.move`` on hardware; ``Simulation.apply`` without it."""

    def apply(self, angles: list[JointAngles]) -> None: ...


class BusActuator:
    """Maps the six legs' angles to servo ids and sends one sync-write per tick."""

    def __init__(self, bus: Bus, geometry: BodyGeometry, move_time_ms: int = 0):
        self.bus, self.geometry, self.move_time_ms = bus, geometry, move_time_ms

    def servo_ids(self) -> list[int]:
        return [sid for leg in range(6) for sid in self.geometry.servo_ids(leg)]

    def apply(self, angles: list[JointAngles]) -> None:
        targets: dict[int, float] = {}
        for leg, a in enumerate(angles):
            c, f, t = self.geometry.servo_ids(leg)
            targets[c], targets[f], targets[t] = a.coxa, a.femur, a.tibia
        self.bus.move(targets, time_ms=self.move_time_ms)

    def torque(self, on: bool) -> None:
        self.bus.torque(self.servo_ids(), on)


@dataclass
class Body:
    geometry: BodyGeometry = field(default_factory=BodyGeometry)
    gait: GaitParams = field(default_factory=GaitParams)
    actuator: Actuator | None = None
    pose: BodyPose = field(default_factory=BodyPose)
    command: Velocity = field(default_factory=Velocity)
    state: GaitState = field(default_factory=GaitState)
    sit_height_m: float = 0.035
    angles: list[JointAngles] = field(default_factory=list)
    feet: list[Vec3] = field(default_factory=list)
    _height: float | None = None

    def __post_init__(self) -> None:
        if self._height is None:
            self._height = self.geometry.stance_height_m

    # --- commands -----------------------------------------------------------------------
    def set_velocity(self, vx: float, vy: float = 0.0, wz: float = 0.0) -> None:
        self.command = Velocity(vx, vy, wz)

    def stop(self) -> None:
        self.command = Velocity()

    def set_pose(self, pose: BodyPose) -> None:
        self.pose = pose

    def set_height(self, height_m: float) -> None:
        lo, hi = self.sit_height_m, self.geometry.reach_m * 0.8
        self._height = max(lo, min(hi, height_m))

    # --- the tick -----------------------------------------------------------------------
    def targets(self, dt: float) -> list[JointAngles]:
        """Advance the gait by ``dt`` and compute the joint angles, without sending them."""
        feet = step(self.geometry, self.state, self.command, self.gait, dt)
        dz = self.geometry.stance_height_m - (self._height or self.geometry.stance_height_m)
        feet = [(x, y, z + dz) for x, y, z in feet]
        self.feet = feet
        self.angles = solve_body(self.geometry, feet, self.pose)
        return self.angles

    def tick(self, dt: float = 1.0 / TICK_HZ) -> list[JointAngles]:
        angles = self.targets(dt)
        if self.actuator is not None:
            self.actuator.apply(angles)
        return angles

    def feet_down(self) -> list[bool]:
        return feet_down(self.state, self.gait)

    def stand(self, seconds: float = 1.0, actuate: bool = True) -> None:
        """Rise from wherever the legs are to the neutral stance over ``seconds``."""
        self.stop()
        self.set_height(self.geometry.stance_height_m)
        self._settle(seconds, actuate)

    def sit(self, seconds: float = 1.0, actuate: bool = True) -> None:
        self.stop()
        self.set_height(self.sit_height_m)
        self._settle(seconds, actuate)

    def _settle(self, seconds: float, actuate: bool) -> None:
        n = max(1, int(seconds * TICK_HZ))
        for _ in range(n):
            self.tick(1.0 / TICK_HZ) if actuate else self.targets(1.0 / TICK_HZ)
            if actuate and self.actuator is not None:
                time.sleep(1.0 / TICK_HZ)


class BodyBackend(RobotInterface):
    """The service's robot: drives ``Body`` from the normalised Twist the motion layer produces."""

    def __init__(self, body: Body, status: RobotStatus | None = None):
        self.body = body
        self._status = status or RobotStatus(note="body: own gait")
        self._lock = threading.Lock()
        self._moving = False

    def get_status(self) -> RobotStatus:
        self._status.is_moving = self._moving
        return self._status

    def _drive(self, lin_x: float, ang_z: float, duration_s: float) -> None:
        with self._lock:
            self._moving = True
            self.body.set_velocity(lin_x * GAIT_SPEED_MPS, 0.0, math.radians(ang_z * TURN_RATE_DPS))
            end = time.monotonic() + duration_s
            period = 1.0 / TICK_HZ
            try:
                while time.monotonic() < end:
                    t0 = time.monotonic()
                    try:
                        self.body.tick(period)
                    except ReachError as e:  # a pose the legs cannot hold: stop rather than fold
                        log.warning("body: %s; stopping", e)
                        break
                    time.sleep(max(0.0, period - (time.monotonic() - t0)))
            finally:
                self.body.stop()
                # let the feet come down before reporting still
                for _ in range(int(self.body.gait.cycle_s * TICK_HZ)):
                    self.body.tick(period)
                    if all(self.body.feet_down()):
                        break
                    time.sleep(period)
                self._moving = False

    def stop(self) -> None:
        self.body.stop()
        self._moving = False


def simulated_body(
    geometry: BodyGeometry | None = None, gait: GaitParams | None = None
) -> tuple[Body, Simulation]:
    sim = Simulation(geometry or BodyGeometry())
    body = Body(geometry=sim.geometry, gait=gait or GaitParams(), actuator=sim)
    return body, sim
