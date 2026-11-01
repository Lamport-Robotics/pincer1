"""Stairs: a crawl over steps, generated from the body's own kinematics.

The body advances a fixed distance per cycle in three pulls, and between the pulls each pair of legs
(front, middle, rear) steps to where it will be needed next, with its foot on whatever level the
terrain has there. A foot that would land on a riser's edge is moved off it. The body's height and
pitch follow the feet: pitch is the slope from the rear feet to the front feet (nose up when the
front is higher, capped), height keeps the legs near the climbing stance height. Every keyframe is
solved through the leg IK inside the joint limits, with at least three feet down and the body's
centre inside their support polygon; a riser the geometry cannot do raises ``ClimbError`` from the
phase that failed instead of producing a cycle that falls over.

Frame: the body's neutral frame at the start of the climb (x forward toward the stairs, z up, origin
at the top plate centre, ``stance_height_m`` above the lower floor). ``Terrain`` holds the risers; a
``Keyframe`` is six feet in that frame plus a ``BodyPose`` for the body. ``interpolate`` turns the
keyframes into 50 Hz samples that the simulator and the robot play the same way.
"""

from __future__ import annotations

import math
from collections.abc import Iterator
from dataclasses import dataclass, field

from .geometry import BodyGeometry
from .kinematics import BodyPose, JointAngles, ReachError, Vec3, rotate, solve_body
from .sim import _polygon_margin

FRONT, MIDDLE, REAR = (0, 5), (1, 4), (2, 3)  # leg pairs: FR FL, MR ML, RR RL
# length, width, depth below the top plate (sim/build.py): what has to clear the steps
BODY_BOX_M = (0.210, 0.150, 0.045)
COXA_SPAN_M = 0.186  # front coxa to rear coxa, for the pitch estimate


class ClimbError(ValueError):
    def __init__(self, reason: str, phase: str):
        super().__init__(f"{phase}: {reason}")
        self.reason, self.phase = reason, phase


@dataclass(frozen=True)
class Terrain:
    """Risers at ``edge0 + k * tread_m`` for k < steps, each ``riser_m`` up; flat beyond the
    last."""

    riser_m: float
    tread_m: float = 0.28
    steps: int = 1
    edge0_m: float = 0.30
    # the lower floor in the climb frame: the body starts at walking height
    floor_z: float = -BodyGeometry().stance_height_m

    def edges(self) -> list[float]:
        return [self.edge0_m + k * self.tread_m for k in range(self.steps)]

    def height(self, x: float) -> float:
        k = sum(1 for e in self.edges() if x >= e)
        return self.floor_z + k * self.riser_m

    def top_z(self) -> float:
        return self.floor_z + self.steps * self.riser_m


@dataclass
class ClimbParams:
    riser_m: float
    tread_m: float = 0.28
    steps: int = 1
    advance_m: float = 0.11  # body travel per cycle
    # where a foot lands, as a fraction of the advance ahead of its neutral point. the rear feet
    # land
    # well ahead so they are still under the body, not stretched out behind it, when they next swing
    place_ahead: tuple[float, float, float] = (0.5, 0.5, 0.8)  # front, middle, rear
    # front and rear feet well under the body: more height, and the rear legs keep their reach
    stance_radius_m: float = 0.14
    middle_radius_m: float = 0.215  # middle feet wide: a foot far out can sit above its own coxa
    stance_height_m: float = 0.18  # leg extension the body aims for while climbing
    lift_clearance_m: float = 0.04  # swing height above the higher of the two levels
    swing_radius_m: float = 0.22  # how far out from the coxa a foot travels while it is in the air
    edge_margin_m: float = 0.03  # no foot within this of a riser's edge (the foot ball is 9 mm)
    pitch_max: float = math.radians(30)
    body_clearance_m: float = 0.015  # the body's underside keeps this much above the step below it
    # support margin the body centre must keep; the head on its arm moves the real centre of mass
    # a centimetre or two when the body is pitched
    margin_min_m: float = 0.035
    pull_sub_m: float = 0.02  # body displacement per pull keyframe
    swing_s: float = 0.25  # per swing keyframe (up, over, down)
    pull_s: float = 0.35  # per pull keyframe
    terrain: Terrain = field(init=False)

    def __post_init__(self) -> None:
        self.terrain = Terrain(self.riser_m, self.tread_m, self.steps)


@dataclass(frozen=True)
class Keyframe:
    feet_world: list[Vec3]
    pose: BodyPose
    down: list[bool]
    seconds: float
    note: str


def body_centre_margin(feet: list[Vec3], down: list[bool], pose: BodyPose) -> float:
    support = [(f[0], f[1]) for f, d in zip(feet, down, strict=True) if d]
    return _polygon_margin(support, pose.x, pose.y)


def body_lowest_gap(pose: BodyPose, terrain: Terrain) -> float:
    """Smallest gap between the body's underside and the terrain under it: the four bottom
    corners of the body box, placed by the pose, against the step height at each corner's x."""
    length, width, depth = BODY_BOX_M
    gap = math.inf
    for sx in (-0.5, 0.5):
        for sy in (-0.5, 0.5):
            cx, cy, cz = rotate((sx * length, sy * width, -depth), pose.roll, pose.pitch, pose.yaw)
            gap = min(gap, pose.z + cz - terrain.height(pose.x + cx))
    return gap


def check(geometry: BodyGeometry, kf: Keyframe, params: ClimbParams) -> list[JointAngles]:
    """Angles for a keyframe, or ClimbError when it does not solve or is not supported."""
    if sum(kf.down) < 3:
        raise ClimbError("fewer than three feet down", kf.note)
    m = body_centre_margin(kf.feet_world, kf.down, kf.pose)
    if m < params.margin_min_m:
        raise ClimbError(f"support margin {m * 1000:.0f} mm", kf.note)
    low = body_lowest_gap(kf.pose, params.terrain)
    if low < params.body_clearance_m:
        raise ClimbError(f"body {low * 1000:.0f} mm above the step", kf.note)
    try:
        return solve_body(geometry, kf.feet_world, kf.pose)
    except ReachError as e:
        raise ClimbError(str(e), kf.note) from None


def neutral_offset(geometry: BodyGeometry, leg: int, radius: float) -> tuple[float, float]:
    mx, my, yaw = geometry.mount(leg)
    return (mx + radius * math.cos(yaw), my + radius * math.sin(yaw))


def _fit_pose(geometry: BodyGeometry, feet: list[Vec3], x: float, params: ClimbParams) -> BodyPose:
    """Body pose over the feet: pitch from the front/rear foot heights, height so the most and the
    least extended leg are equally far from the climbing stance height."""
    front_z = (feet[FRONT[0]][2] + feet[FRONT[1]][2]) / 2
    rear_z = (feet[REAR[0]][2] + feet[REAR[1]][2]) / 2
    # the body's pitch rotation is about +y, so nose up is a negative pitch
    pitch = -max(
        -params.pitch_max, min(params.pitch_max, math.atan2(front_z - rear_z, COXA_SPAN_M))
    )
    return BodyPose(x=x, z=_height_for(geometry, feet, pitch, params), pitch=pitch)


def _height_for(
    geometry: BodyGeometry, feet: list[Vec3], pitch: float, params: ClimbParams
) -> float:
    prefs = []
    for leg, foot in enumerate(feet):
        mx, my, _ = geometry.mount(leg)
        prefs.append(foot[2] + params.stance_height_m - rotate((mx, my, 0.0), 0.0, pitch, 0.0)[2])
    return (min(prefs) + max(prefs)) / 2


def _fit_keyframe(
    geometry: BodyGeometry,
    feet: list[Vec3],
    down: list[bool],
    base: BodyPose,
    params: ClimbParams,
    note: str,
    seconds: float,
) -> Keyframe:
    """``base`` if every leg can hold it, else the nearest pose that works: the fitted pitch first,
    then level through to full nose-up in 5 degree steps, the height searched outward from the
    fit."""
    last: ClimbError | None = None
    pitches = [base.pitch] + [
        -math.radians(d) for d in range(0, int(math.degrees(params.pitch_max)) + 1, 5)
    ]
    seen: set[float] = set()
    for pitch in pitches:
        if round(pitch, 4) in seen:
            continue
        seen.add(round(pitch, 4))
        z0 = _height_for(geometry, feet, pitch, params) if pitch != base.pitch else base.z
        for dz in sorted(range(-30, 31), key=abs):
            pose = BodyPose(x=base.x, y=base.y, z=z0 + dz * 0.005, pitch=pitch)
            kf = Keyframe(list(feet), pose, list(down), seconds, note)
            try:
                check(geometry, kf, params)
                return kf
            except ClimbError as e:
                last = e
    assert last is not None
    raise last


def _best_pose(
    geometry: BodyGeometry,
    feet: list[Vec3],
    down: list[bool],
    x: float,
    params: ClimbParams,
    note: str,
) -> Keyframe:
    return _fit_keyframe(
        geometry, feet, down, _fit_pose(geometry, feet, x, params), params, note, params.pull_s
    )


def _snap_off_edge(x: float, terrain: Terrain, margin: float) -> float:
    for e in terrain.edges():
        if e - margin < x < e:
            return e - margin
        if e <= x < e + margin:
            return e + margin
    return x


def climb_cycle(
    geometry: BodyGeometry, params: ClimbParams, steps: int | None = None
) -> list[Keyframe]:
    """Keyframes from standing before the first riser to standing on the top landing."""
    if steps is not None:
        params.steps = steps
        params.terrain = Terrain(params.riser_m, params.tread_m, steps)
    terrain = params.terrain
    radius0 = geometry.stance_radius_m
    feet: list[Vec3] = [(*neutral_offset(geometry, i, radius0), terrain.floor_z) for i in range(6)]
    down = [True] * 6
    x = 0.0
    frames = [Keyframe(list(feet), BodyPose(), list(down), 1.0, "stand")]
    # done when the rear feet can stand at the climbing radius on the top landing, off the last edge
    rear_off = neutral_offset(geometry, REAR[0], params.stance_radius_m)[0]
    x_goal = (
        terrain.edges()[-1]
        + params.edge_margin_m
        - rear_off
        + params.advance_m * params.place_ahead[2]
    )
    cycles = 0
    while x < x_goal - 1e-9:
        cycles += 1
        if cycles > 60:
            raise ClimbError("no progress", "cycle")
        adv = min(params.advance_m, x_goal - x)
        for k, pair in enumerate((FRONT, MIDDLE, REAR)):
            radius = params.middle_radius_m if pair is MIDDLE else params.stance_radius_m
            for leg in pair:
                ox, oy = neutral_offset(geometry, leg, radius)
                tx = _snap_off_edge(
                    x + ox + adv * params.place_ahead[k], terrain, params.edge_margin_m
                )
                target = (tx, oy, terrain.height(tx))
                frames += _swing(geometry, feet, down, leg, target, frames[-1].pose, params)
                feet[leg] = target
            frames += _pull(geometry, feet, down, x, x + adv / 3, params)
            x += adv / 3
    # level out on the landing: first bring the body level and down to walking height over the
    # climbing stance, in small checked steps, then walk the feet out to the walking radius one
    # at a time. The other way round (feet out while the body is still pitched and high) drops
    # the body onto the new stance in one go
    final = BodyPose(x=x, z=terrain.top_z() + geometry.stance_height_m)
    frames += _settle(geometry, feet, down, frames[-1].pose, final, params)
    for pair in (FRONT, MIDDLE, REAR):
        for leg in pair:
            ox, oy = neutral_offset(geometry, leg, radius0)
            target = (x + ox, oy, terrain.top_z())
            frames += _swing(geometry, feet, down, leg, target, frames[-1].pose, params)
            feet[leg] = target
    frames += _settle(geometry, feet, down, frames[-1].pose, final, params)
    kf = Keyframe(list(feet), final, [True] * 6, params.pull_s, "level")
    check(geometry, kf, params)
    frames.append(kf)
    return frames


def _settle(
    geometry: BodyGeometry,
    feet: list[Vec3],
    down: list[bool],
    start: BodyPose,
    goal: BodyPose,
    params: ClimbParams,
) -> list[Keyframe]:
    """Body from ``start`` to ``goal`` over planted feet in steps of at most 1 cm and 5 degrees,
    each checked; stops short at the last pose that still solves rather than failing the climb."""
    n = max(
        1,
        int(
            math.ceil(
                max(
                    abs(goal.z - start.z) / 0.01,
                    abs(goal.pitch - start.pitch) / math.radians(5),
                    abs(goal.x - start.x) / params.pull_sub_m,
                )
            )
        ),
    )
    out: list[Keyframe] = []
    for i in range(1, n + 1):
        s = i / n
        pose = BodyPose(
            start.x + (goal.x - start.x) * s,
            start.y,
            start.z + (goal.z - start.z) * s,
            0.0,
            start.pitch + (goal.pitch - start.pitch) * s,
            0.0,
        )
        kf = Keyframe(list(feet), pose, list(down), params.pull_s, f"settle {i}/{n}")
        try:
            check(geometry, kf, params)
        except ClimbError:
            break
        out.append(kf)
    return out


def _swing(
    geometry: BodyGeometry,
    feet: list[Vec3],
    down: list[bool],
    leg: int,
    target: Vec3,
    pose: BodyPose,
    params: ClimbParams,
) -> list[Keyframe]:
    """Out and up, over, in and down. The leg reaches highest with the foot far out (knee open,
    femur up), so the swing goes out to ``swing_radius_m`` from the coxa while it lifts, crosses the
    edge there, and comes back in as it drops onto the target."""
    start = feet[leg]
    top = max(start[2], target[2]) + params.lift_clearance_m
    mx, my, _ = geometry.mount(leg)
    cx, cy, _ = rotate((mx, my, 0.0), pose.roll, pose.pitch, pose.yaw)
    cx, cy = cx + pose.x, cy + pose.y

    def out_point(p: Vec3) -> tuple[float, float]:
        b = math.atan2(p[1] - cy, p[0] - cx)
        return (cx + params.swing_radius_m * math.cos(b), cy + params.swing_radius_m * math.sin(b))

    out = []
    lifted = list(down)
    lifted[leg] = False
    for note, foot in (
        (f"leg {leg} up", (*out_point(start), top)),
        (f"leg {leg} over", (*out_point(target), top)),
        (f"leg {leg} down", target),
    ):
        f = list(feet)
        f[leg] = foot
        # the body may shift a little to make the lifted foot reachable; the planted feet stay put
        kf = _fit_keyframe(geometry, f, lifted, pose, params, note, params.swing_s)
        pose = kf.pose
        out.append(kf)
    down[leg] = True
    return out


def _pull(
    geometry: BodyGeometry,
    feet: list[Vec3],
    down: list[bool],
    x_from: float,
    x_to: float,
    params: ClimbParams,
) -> list[Keyframe]:
    """Move the body forward (and up / pitched as the feet dictate) in small checked steps."""
    n = max(1, int(math.ceil((x_to - x_from) / params.pull_sub_m)))
    out = []
    for i in range(1, n + 1):
        x = x_from + (x_to - x_from) * i / n
        out.append(_best_pose(geometry, feet, down, x, params, f"pull to {x:.3f}"))
    return out


# how fast the body may be moved between keyframes: a keyframe takes its own ``seconds`` or
# longer when the pose change asks for it, so a 30 degree pitch change is not done in 0.35 s
PITCH_RATE = math.radians(30)  # rad/s
HEIGHT_RATE = 0.06  # m/s
TRAVEL_RATE = 0.10  # m/s


def seconds_between(prev: Keyframe, kf: Keyframe) -> float:
    """Time from ``prev`` to ``kf``: the keyframe's duration, stretched for a large pose change."""
    p, q = prev.pose, kf.pose
    return max(
        kf.seconds,
        abs(q.pitch - p.pitch) / PITCH_RATE,
        abs(q.z - p.z) / HEIGHT_RATE,
        math.hypot(q.x - p.x, q.y - p.y) / TRAVEL_RATE,
    )


def interpolate(
    keyframes: list[Keyframe], dt: float = 0.02
) -> Iterator[tuple[list[Vec3], BodyPose]]:
    """Linear in time between keyframes; the first keyframe is held for its own duration."""
    prev = keyframes[0]
    yield list(prev.feet_world), prev.pose
    for kf in keyframes[1:]:
        n = max(1, int(round(seconds_between(prev, kf) / dt)))
        for i in range(1, n + 1):
            s = i / n
            feet = [
                tuple(a + (b - a) * s for a, b in zip(pa, pb, strict=True))
                for pa, pb in zip(prev.feet_world, kf.feet_world, strict=True)
            ]
            p, q = prev.pose, kf.pose
            pose = BodyPose(
                p.x + (q.x - p.x) * s,
                p.y + (q.y - p.y) * s,
                p.z + (q.z - p.z) * s,
                p.roll + (q.roll - p.roll) * s,
                p.pitch + (q.pitch - p.pitch) * s,
                p.yaw + (q.yaw - p.yaw) * s,
            )
            yield feet, pose  # type: ignore[misc]
        prev = kf


def angles_at(geometry: BodyGeometry, feet: list[Vec3], pose: BodyPose) -> list[JointAngles]:
    return solve_body(geometry, feet, pose)


def duration(keyframes: list[Keyframe]) -> float:
    if not keyframes:
        return 0.0
    later = zip(keyframes, keyframes[1:], strict=False)
    return keyframes[0].seconds + sum(seconds_between(a, b) for a, b in later)
