"""The body as numbers: where the six legs sit, how long their segments are, where joints may go.

Frames (right-handed, metres, radians):

* body frame: origin at the centre of the top plate, x forward, y left, z up.
* leg frame i: origin at leg i's coxa axis, x pointing outward along the mount yaw, z up.

Legs are numbered clockwise from the front right: 0 FR, 1 MR, 2 RR, 3 RL, 4 ML, 5 FL. The default
numbers fit a six-legged body on 3 mm plates with 30 kg·cm bus servos (docs/HARDWARE.md); a
different
build changes the dataclass, not the code.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

LEG_NAMES = ("FR", "MR", "RR", "RL", "ML", "FL")


@dataclass(frozen=True)
class JointLimits:
    """Servo travel per joint, radians from the calibrated zero."""

    coxa: tuple[float, float] = (-math.radians(45), math.radians(45))
    femur: tuple[float, float] = (-math.radians(70), math.radians(90))
    tibia: tuple[float, float] = (math.radians(25), math.radians(165))  # interior knee angle


@dataclass(frozen=True)
class BodyGeometry:
    coxa_m: float = 0.045
    femur_m: float = 0.080
    tibia_m: float = 0.120
    # coxa axis positions in the body frame (x, y) and the mount yaw, per leg
    mounts: tuple[tuple[float, float, float], ...] = (
        (0.085, -0.065, -math.radians(50)),  # FR
        (0.000, -0.095, -math.radians(90)),  # MR
        (-0.085, -0.065, -math.radians(130)),  # RR
        (-0.085, 0.065, math.radians(130)),  # RL
        (0.000, 0.095, math.radians(90)),  # ML
        (0.085, 0.065, math.radians(50)),  # FL
    )
    limits: JointLimits = field(default_factory=JointLimits)
    # neutral stance: foot this far out along the mount yaw, body this high above the ground
    stance_radius_m: float = 0.145
    stance_height_m: float = 0.090
    # servo ids: leg i uses ids base + 3*i + (0 coxa, 1 femur, 2 tibia)
    servo_id_base: int = 1

    def mount(self, leg: int) -> tuple[float, float, float]:
        return self.mounts[leg]

    def servo_ids(self, leg: int) -> tuple[int, int, int]:
        b = self.servo_id_base + 3 * leg
        return (b, b + 1, b + 2)

    def neutral_foot(self, leg: int) -> tuple[float, float, float]:
        """Foot position in the body frame for the neutral stance."""
        mx, my, yaw = self.mounts[leg]
        return (
            mx + self.stance_radius_m * math.cos(yaw),
            my + self.stance_radius_m * math.sin(yaw),
            -self.stance_height_m,
        )

    @property
    def reach_m(self) -> float:
        return self.coxa_m + self.femur_m + self.tibia_m
