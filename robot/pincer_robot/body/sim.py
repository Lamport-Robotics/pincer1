"""A kinematic stand-in for the body: no servos, same maths, with the checks a real body needs.

``Simulation`` takes the joint targets a controller would send, runs forward kinematics, and keeps
track of which feet are on the ground and whether the body's centre still sits inside the support
polygon. It is what the tests drive and what ``--mock`` can show without hardware.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

from .geometry import BodyGeometry
from .kinematics import JointAngles, Vec3, forward, leg_to_body


@dataclass
class StabilityReport:
    feet_down: list[bool]
    # distance from the centre to the nearest edge of the support polygon; negative = outside
    support_margin_m: float
    lowest_swing_clearance_m: float  # how high the lowest airborne foot is above the ground

    @property
    def stable(self) -> bool:
        return sum(self.feet_down) >= 3 and self.support_margin_m > 0.0


def _polygon_margin(points: list[tuple[float, float]], px: float = 0.0, py: float = 0.0) -> float:
    """Signed distance from (px, py) to the convex hull of ``points``; negative outside."""
    if len(points) < 3:
        return -math.inf
    pts = sorted(set(points))
    if len(pts) < 3:
        return -math.inf

    def cross(o: tuple[float, float], a: tuple[float, float], b: tuple[float, float]) -> float:
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower: list[tuple[float, float]] = []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper: list[tuple[float, float]] = []
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    hull = lower[:-1] + upper[:-1]
    margin = math.inf
    for i in range(len(hull)):
        ax, ay = hull[i]
        bx, by = hull[(i + 1) % len(hull)]
        ex, ey = bx - ax, by - ay
        length = math.hypot(ex, ey)
        if length == 0:
            continue
        # counter-clockwise hull: inside is to the left of every edge
        margin = min(margin, (ex * (py - ay) - ey * (px - ax)) / length)
    return margin


@dataclass
class Simulation:
    geometry: BodyGeometry = field(default_factory=BodyGeometry)
    ground_z: float | None = None  # body-frame z of the ground; None = taken from the first stance
    contact_tolerance_m: float = 0.004
    angles: list[JointAngles] = field(default_factory=list)

    def apply(self, angles: list[JointAngles]) -> None:
        if len(angles) != 6:
            raise ValueError("six legs")
        lim = self.geometry.limits
        for i, a in enumerate(angles):
            for name, value, (lo, hi) in (
                ("coxa", a.coxa, lim.coxa),
                ("femur", a.femur, lim.femur),
                ("tibia", a.tibia, lim.tibia),
            ):
                if value < lo - 1e-9 or value > hi + 1e-9:
                    raise ValueError(
                        f"leg {i} {name} {math.degrees(value):.1f} deg outside "
                        f"{math.degrees(lo):.0f}..{math.degrees(hi):.0f}"
                    )
        self.angles = list(angles)
        if self.ground_z is None:
            self.ground_z = min(f[2] for f in self.feet())

    def feet(self) -> list[Vec3]:
        return [
            leg_to_body(self.geometry, i, forward(self.geometry, a))
            for i, a in enumerate(self.angles)
        ]

    def report(self) -> StabilityReport:
        feet = self.feet()
        ground = self.ground_z if self.ground_z is not None else min(f[2] for f in feet)
        down = [abs(f[2] - ground) <= self.contact_tolerance_m for f in feet]
        support = [(f[0], f[1]) for f, d in zip(feet, down, strict=True) if d]
        airborne = [f[2] - ground for f, d in zip(feet, down, strict=True) if not d]
        return StabilityReport(
            down, _polygon_margin(support), min(airborne) if airborne else math.inf
        )
