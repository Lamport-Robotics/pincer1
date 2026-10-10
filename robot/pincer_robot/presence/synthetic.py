"""Synthetic 2D lidar scans of a rectangular room with cylinder obstacles.

Shared by the unit tests and by ``pincer-robot --mock`` so the mock mode exercises
the same clustering code the real robot runs.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from .lidar import Scan


@dataclass(frozen=True)
class Room:
    x_min: float = -1.5
    x_max: float = 2.5
    y_min: float = -2.0
    y_max: float = 2.0


@dataclass(frozen=True)
class Cylinder:
    x: float
    y: float
    radius: float


def _wall_distance(
    room: Room, cos_a: float, sin_a: float, ox: float = 0.0, oy: float = 0.0
) -> float:
    best = math.inf
    if cos_a > 1e-9:
        best = min(best, (room.x_max - ox) / cos_a)
    elif cos_a < -1e-9:
        best = min(best, (room.x_min - ox) / cos_a)
    if sin_a > 1e-9:
        best = min(best, (room.y_max - oy) / sin_a)
    elif sin_a < -1e-9:
        best = min(best, (room.y_min - oy) / sin_a)
    return best


def _cylinder_distance(
    c: Cylinder, cos_a: float, sin_a: float, ox: float = 0.0, oy: float = 0.0
) -> float:
    # Ray from (ox, oy) along (cos_a, sin_a); circle centre (c.x, c.y).
    cx, cy = c.x - ox, c.y - oy
    b = -2.0 * (cx * cos_a + cy * sin_a)
    cc = cx * cx + cy * cy - c.radius * c.radius
    disc = b * b - 4.0 * cc
    if disc < 0:
        return math.inf
    t = (-b - math.sqrt(disc)) / 2.0
    return t if t > 0 else math.inf


def make_scan(
    ts: float,
    objects: list[Cylinder] | None = None,
    *,
    room: Room | None = None,
    beams: int = 360,
    noise_m: float = 0.0,
    rng: np.random.Generator | None = None,
    range_max: float = 12.0,
    pose: tuple[float, float, float] = (0.0, 0.0, 0.0),
) -> Scan:
    """Ray-cast one scan from ``pose`` = (x, y, yaw); default: robot at (0,0) facing +x.

    Beam ``i`` of the scan is at robot-frame angle ``angle_min + i*inc``; in the room
    frame it points along ``yaw + that angle``. Objects and the room stay in the room
    frame, so moving ``pose`` is exactly "the robot was put down somewhere else".
    """
    room = room or Room()
    objs = objects or []
    ox, oy, yaw = pose
    inc = 2.0 * math.pi / beams
    angle_min = -math.pi
    ranges = np.empty(beams, dtype=np.float64)
    for i in range(beams):
        a = angle_min + i * inc + yaw
        ca, sa = math.cos(a), math.sin(a)
        d = _wall_distance(room, ca, sa, ox, oy)
        for o in objs:
            d = min(d, _cylinder_distance(o, ca, sa, ox, oy))
        ranges[i] = d if d <= range_max else math.inf
    if noise_m > 0:
        g = rng or np.random.default_rng(0)
        finite = np.isfinite(ranges)
        ranges[finite] += g.normal(0.0, noise_m, finite.sum())
    return Scan(angle_min=angle_min, angle_increment=inc, ranges=ranges, ts=ts, range_max=range_max)


def person_at(x: float, y: float) -> Cylinder:
    """A standing person approximated as a 0.4 m wide cylinder."""
    return Cylinder(x, y, 0.20)


def chair_at(x: float, y: float) -> Cylinder:
    return Cylinder(x, y, 0.22)
