"""Leg kinematics: three revolute joints per leg, closed-form both ways.

Joint angles (radians): ``coxa`` yaw about the leg's z axis, ``femur`` pitch from horizontal (up is
positive), ``tibia`` the interior knee angle (pi = straight leg, pi/2 = tibia perpendicular to the
femur).
The body pose moves every foot target the opposite way, so a pose change keeps the feet planted.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from .geometry import BodyGeometry

Vec3 = tuple[float, float, float]


class ReachError(ValueError):
    pass


@dataclass(frozen=True)
class JointAngles:
    coxa: float
    femur: float
    tibia: float

    def as_tuple(self) -> tuple[float, float, float]:
        return (self.coxa, self.femur, self.tibia)


@dataclass(frozen=True)
class BodyPose:
    """Translation and rotation of the body relative to its neutral stance."""

    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    roll: float = 0.0
    pitch: float = 0.0
    yaw: float = 0.0


def rotate(v: Vec3, roll: float, pitch: float, yaw: float) -> Vec3:
    """R = Rz(yaw) Ry(pitch) Rx(roll) applied to v."""
    x, y, z = v
    cr, sr = math.cos(roll), math.sin(roll)
    y, z = y * cr - z * sr, y * sr + z * cr
    cp, sp = math.cos(pitch), math.sin(pitch)
    x, z = x * cp + z * sp, -x * sp + z * cp
    cy, sy = math.cos(yaw), math.sin(yaw)
    x, y = x * cy - y * sy, x * sy + y * cy
    return (x, y, z)


def rotate_inverse(v: Vec3, roll: float, pitch: float, yaw: float) -> Vec3:
    """R^T v: the body rotated by R sees a fixed world point at R^T (v - t)."""
    x, y, z = v
    cy, sy = math.cos(yaw), math.sin(yaw)
    x, y = x * cy + y * sy, -x * sy + y * cy
    cp, sp = math.cos(pitch), math.sin(pitch)
    x, z = x * cp - z * sp, x * sp + z * cp
    cr, sr = math.cos(roll), math.sin(roll)
    y, z = y * cr + z * sr, -y * sr + z * cr
    return (x, y, z)


def body_to_leg(geometry: BodyGeometry, leg: int, point: Vec3) -> Vec3:
    """A body-frame point expressed in leg ``leg``'s frame."""
    mx, my, yaw = geometry.mount(leg)
    dx, dy, dz = point[0] - mx, point[1] - my, point[2]
    c, s = math.cos(-yaw), math.sin(-yaw)
    return (dx * c - dy * s, dx * s + dy * c, dz)


def leg_to_body(geometry: BodyGeometry, leg: int, point: Vec3) -> Vec3:
    mx, my, yaw = geometry.mount(leg)
    c, s = math.cos(yaw), math.sin(yaw)
    x, y, z = point
    return (mx + x * c - y * s, my + x * s + y * c, z)


def foot_in_body_frame(geometry: BodyGeometry, leg: int, foot_world: Vec3, pose: BodyPose) -> Vec3:
    """Where a planted foot is from the body's point of view once the body has moved by ``pose``."""
    shifted = (foot_world[0] - pose.x, foot_world[1] - pose.y, foot_world[2] - pose.z)
    return rotate_inverse(shifted, pose.roll, pose.pitch, pose.yaw)


def forward(geometry: BodyGeometry, angles: JointAngles) -> Vec3:
    """Foot position in the leg frame."""
    horizontal = (
        geometry.coxa_m
        + geometry.femur_m * math.cos(angles.femur)
        + geometry.tibia_m * math.cos(angles.femur + angles.tibia - math.pi)
    )
    z = geometry.femur_m * math.sin(angles.femur) + geometry.tibia_m * math.sin(
        angles.femur + angles.tibia - math.pi
    )
    return (horizontal * math.cos(angles.coxa), horizontal * math.sin(angles.coxa), z)


def inverse(geometry: BodyGeometry, target: Vec3, *, check_limits: bool = True) -> JointAngles:
    """Joint angles that put the foot at ``target`` (leg frame); ``ReachError`` when it cannot."""
    x, y, z = target
    coxa = math.atan2(y, x)
    horizontal = math.hypot(x, y) - geometry.coxa_m
    if horizontal < 0:
        raise ReachError(f"target {target} is inside the coxa radius")
    d = math.hypot(horizontal, z)
    f, t = geometry.femur_m, geometry.tibia_m
    if d > f + t - 1e-9 or d < abs(f - t) + 1e-9:
        raise ReachError(
            f"target {target} is {d:.3f} m from the femur joint; "
            f"this leg spans {abs(f - t):.3f}..{f + t:.3f} m"
        )
    knee = math.acos(max(-1.0, min(1.0, (f * f + t * t - d * d) / (2 * f * t))))
    lift = math.acos(max(-1.0, min(1.0, (f * f + d * d - t * t) / (2 * f * d))))
    femur = math.atan2(z, horizontal) + lift
    angles = JointAngles(coxa, femur, knee)
    if check_limits:
        lim = geometry.limits
        for name, value, (lo, hi) in (
            ("coxa", coxa, lim.coxa),
            ("femur", femur, lim.femur),
            ("tibia", knee, lim.tibia),
        ):
            if value < lo - 1e-9 or value > hi + 1e-9:
                raise ReachError(
                    f"{name} would be {math.degrees(value):.1f} deg, "
                    f"limit {math.degrees(lo):.0f}..{math.degrees(hi):.0f}"
                )
    return angles


def solve_leg(geometry: BodyGeometry, leg: int, foot_body: Vec3) -> JointAngles:
    """Angles for leg ``leg`` so that its foot sits at ``foot_body`` (body frame)."""
    return inverse(geometry, body_to_leg(geometry, leg, foot_body))


def solve_body(geometry: BodyGeometry, feet_world: list[Vec3], pose: BodyPose) -> list[JointAngles]:
    """All six legs for planted feet ``feet_world`` (neutral body frame) and a body pose."""
    return [
        solve_leg(geometry, i, foot_in_body_frame(geometry, i, foot, pose))
        for i, foot in enumerate(feet_world)
    ]
