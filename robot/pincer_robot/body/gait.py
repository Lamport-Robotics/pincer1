"""Gait generation: a velocity command in, six foot targets per tick out.

One gait cycle lasts ``cycle_s``. Each leg follows the same phase curve, offset per gait:

* ``tripod``  FR+RR+ML against MR+RL+FL, half a cycle apart, duty 1/2. Fast, always three feet down.
* ``ripple``  one leg at a time starting a swing every sixth of a cycle, duty 2/3, two diagonal
              legs in
              the air at most. Smoother and steadier, about half the speed.
* ``wave``    one leg in the air at a time, duty 5/6. Slow, for rough ground or a heavy payload.

During stance the foot slides backwards under the body at the commanded speed; during swing it
lifts on a half sine and returns. The stride per cycle is ``v * duty * cycle_s``; when the command
would need a longer stride than ``step_length_m`` it is scaled down and ``actual`` reports what the
body does.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

from .geometry import BodyGeometry
from .kinematics import Vec3

GAITS: dict[str, tuple[float, tuple[float, ...]]] = {
    # name: (duty factor, phase offset per leg FR MR RR RL ML FL)
    "tripod": (0.5, (0.0, 0.5, 0.0, 0.5, 0.0, 0.5)),
    "ripple": (2 / 3, (0.0, 4 / 6, 2 / 6, 5 / 6, 1 / 6, 3 / 6)),
    "wave": (5 / 6, (0.0, 1 / 6, 2 / 6, 3 / 6, 4 / 6, 5 / 6)),
}


@dataclass(frozen=True)
class Velocity:
    """Body velocity command: m/s forward, m/s left, rad/s counter-clockwise."""

    vx: float = 0.0
    vy: float = 0.0
    wz: float = 0.0

    @property
    def moving(self) -> bool:
        return abs(self.vx) > 1e-6 or abs(self.vy) > 1e-6 or abs(self.wz) > 1e-6

    def scaled(self, k: float) -> Velocity:
        return Velocity(self.vx * k, self.vy * k, self.wz * k)


@dataclass
class GaitParams:
    name: str = "tripod"
    cycle_s: float = 1.2
    step_length_m: float = 0.06  # longest stride a foot may take per cycle
    step_height_m: float = 0.035

    @property
    def duty(self) -> float:
        return GAITS[self.name][0]

    @property
    def offsets(self) -> tuple[float, ...]:
        return GAITS[self.name][1]


@dataclass
class GaitState:
    phase: float = 0.0  # 0..1 over one cycle
    actual: Velocity = field(default_factory=Velocity)
    # While stopped the global phase freezes; legs that were in the air finish their swing on their
    # own clock and come down at the neutral point, legs on the ground hold where they are.
    # ``landing[i]`` is that leg's swing progress (0..1) or -1 once it is down; ``held[i]`` the
    # ground offset it keeps.
    landing: list[float] = field(default_factory=lambda: [-1.0] * 6)
    held: list[tuple[float, float]] = field(default_factory=lambda: [(0.0, 0.0)] * 6)
    stopped: bool = True


def stride_vectors(
    geometry: BodyGeometry, v: Velocity, params: GaitParams
) -> list[tuple[float, float]]:
    """Per-leg ground displacement of a foot over one stance, body frame."""
    t = params.duty * params.cycle_s
    out = []
    for leg in range(6):
        nx, ny, _ = geometry.neutral_foot(leg)
        # linear part plus the tangent of the rotation at this foot
        out.append((v.vx * t - v.wz * t * ny, v.vy * t + v.wz * t * nx))
    return out


def limit_command(geometry: BodyGeometry, v: Velocity, params: GaitParams) -> Velocity:
    """Scale the command so no foot's stride exceeds ``step_length_m``."""
    longest = max(
        (math.hypot(sx, sy) for sx, sy in stride_vectors(geometry, v, params)), default=0.0
    )
    if longest <= params.step_length_m or longest == 0.0:
        return v
    return v.scaled(params.step_length_m / longest)


def leg_phase(state_phase: float, offset: float) -> float:
    return (state_phase - offset) % 1.0


def foot_offset(phase: float, stride: tuple[float, float], params: GaitParams) -> Vec3:
    """Displacement of one foot from its neutral point at ``phase`` of its own cycle."""
    sx, sy = stride
    if phase < params.duty:
        s = phase / params.duty  # stance: slide from +stride/2 to -stride/2
        k = 0.5 - s
        return (sx * k, sy * k, 0.0)
    s = (phase - params.duty) / (1.0 - params.duty)  # swing: come back through the air
    k = s - 0.5
    return (sx * k, sy * k, params.step_height_m * math.sin(math.pi * s))


def step(
    geometry: BodyGeometry, state: GaitState, command: Velocity, params: GaitParams, dt: float
) -> list[Vec3]:
    """Advance by ``dt`` and return the six foot targets in the body frame.

    Moving: every leg follows the shared phase. Stopping: the phase freezes, airborne legs finish
    their swing and land at their neutral point, planted legs hold their ground offset, so the body
    settles instead of freezing mid-step or hopping.
    """
    v = limit_command(geometry, command, params)
    state.actual = v
    neutral = [geometry.neutral_foot(leg) for leg in range(6)]
    if v.moving:
        if state.stopped:
            # resume: no leg starts in the air, the held ground offsets blend away over the first
            # stance
            # in the air, the held ground offsets blend away over the first stance
            state.stopped = False
        state.phase = (state.phase + dt / params.cycle_s) % 1.0
        strides = stride_vectors(geometry, v, params)
        feet: list[Vec3] = []
        for leg in range(6):
            p = leg_phase(state.phase, params.offsets[leg])
            ox, oy, oz = foot_offset(p, strides[leg], params)
            state.held[leg] = (
                ox,
                oy,
            )  # ground offset, or where the airborne foot was when a stop begins
            state.landing[leg] = (
                -1.0 if p < params.duty else (p - params.duty) / (1.0 - params.duty)
            )
            nx, ny, nz = neutral[leg]
            feet.append((nx + ox, ny + oy, nz + oz))
        return feet
    state.stopped = True
    swing_s = (1.0 - params.duty) * params.cycle_s
    feet = []
    for leg in range(6):
        nx, ny, nz = neutral[leg]
        s = state.landing[leg]
        if s >= 0.0:
            s = min(1.0, s + dt / swing_s)
            hx, hy = state.held[leg]
            k = 1.0 - s  # glide the airborne foot to its neutral point as it comes down
            if s >= 1.0:
                state.landing[leg] = -1.0
                state.held[leg] = (0.0, 0.0)
                feet.append((nx, ny, nz))
            else:
                state.landing[leg] = s
                feet.append(
                    (nx + hx * k, ny + hy * k, nz + params.step_height_m * math.sin(math.pi * s))
                )
        else:
            hx, hy = state.held[leg]
            feet.append((nx + hx, ny + hy, nz))
    return feet


def feet_down(state: GaitState, params: GaitParams) -> list[bool]:
    """Which legs are on the ground right now."""
    if state.stopped:
        return [s < 0.0 for s in state.landing]
    return [leg_phase(state.phase, o) < params.duty for o in params.offsets]
