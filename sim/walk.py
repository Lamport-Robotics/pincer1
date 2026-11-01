#!/usr/bin/env python3
"""Walks pincer1.urdf in MuJoCo with the body package's own gait and records a gif.

The joint targets come from ``pincer_robot.body`` exactly as they would on the robot: ``Body.targets``
advances the gait at 50 Hz and solves the leg IK, this script only holds the joints with a position
servo and lets physics decide whether the body goes where the gait says. The arm keeps the camera head
up and level the whole time.

    python sim/walk.py                       # tripod at the default speed, 6 s -> sim/walk.gif
    python sim/walk.py --gait ripple --seconds 10
    python sim/walk.py --speed 0.3 --cycle 0.7 --no-gif      # a faster setting, report only
    python sim/walk.py --turn 1.0 --no-gif                   # turn in place at 1 rad/s
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import mujoco
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "robot"))

from pincer_robot.body.controller import TICK_HZ, Body  # noqa: E402
from pincer_robot.body.gait import GaitParams  # noqa: E402
from pincer_robot.body.geometry import LEG_NAMES, BodyGeometry  # noqa: E402
from pincer_robot.motion import GAIT_SPEED_MPS  # noqa: E402

G = BodyGeometry()
CONTROL_DT = 1.0 / TICK_HZ
PHYSICS_DT = 0.002
SPEED_MPS = GAIT_SPEED_MPS  # the service's "speed 1.0"
# camera head up and level: shoulder back a little, elbow forward, wrist back the difference
ARM_POSE = {"arm_yaw": 0.0, "arm_shoulder": -0.2, "arm_elbow": 0.3, "arm_wrist": -0.1}
KP, KV = 16.0, 0.3  # default position servo per joint, N m / rad and N m s / rad; the implicit integrator takes the damping
FPS = 25
FRAME = (640, 400)
GIF_LIMIT = 2_500_000


def load(urdf: Path, kp: float = KP, kv: float = KV) -> tuple[mujoco.MjModel, mujoco.MjData]:
    spec = mujoco.MjSpec.from_file(str(urdf))
    spec.compiler.autolimits = True
    spec.compiler.discardvisual = False  # the urdf default drops the sky texture below
    spec.option.timestep = PHYSICS_DT
    spec.option.integrator = mujoco.mjtIntegrator.mjINT_IMPLICITFAST
    spec.visual.global_.offwidth, spec.visual.global_.offheight = 1280, 800
    floor = spec.worldbody.add_geom(
        name="floor", type=mujoco.mjtGeom.mjGEOM_PLANE, size=[3, 3, 0.1], rgba=[0.86, 0.86, 0.85, 1]
    )
    floor.friction = [1.0, 0.01, 0.001]
    spec.worldbody.add_light(pos=[0.5, -0.5, 1.5], dir=[-0.3, 0.3, -1])
    spec.add_texture(name="sky", type=mujoco.mjtTexture.mjTEXTURE_SKYBOX, builtin=mujoco.mjtBuiltin.mjBUILTIN_GRADIENT,
                     rgb1=[0.97, 0.97, 0.96], rgb2=[0.80, 0.81, 0.82], width=256, height=256)
    body = spec.body("body")
    body.add_freejoint()
    body.pos = [0, 0, G.stance_height_m + 0.01]
    # one position servo per joint, named after it; explicit PD on massless-looking links blows up
    for j in spec.joints:
        if j.type == mujoco.mjtJoint.mjJNT_HINGE:
            a = spec.add_actuator(name=j.name, target=j.name, trntype=mujoco.mjtTrn.mjTRN_JOINT)
            a.set_to_position(kp=kp, kv=kv)
    model = spec.compile()
    return model, mujoco.MjData(model)


def joint_index(model: mujoco.MjModel) -> dict[str, tuple[int, int]]:
    """(qpos address, actuator index) of every hinge joint; the free joint takes the first seven qpos slots."""
    return {model.actuator(i).name: (model.joint(model.actuator(i).name).qposadr[0], i) for i in range(model.nu)}


def leg_targets(body: Body, dt: float) -> dict[str, float]:
    out: dict[str, float] = {}
    for i, a in enumerate(body.targets(dt)):
        n = LEG_NAMES[i].lower()
        out[f"{n}_coxa"], out[f"{n}_femur"], out[f"{n}_tibia"] = a.coxa, a.femur, a.tibia - math.pi
    return out


def quat_to_rp(q) -> tuple[float, float]:
    w, x, y, z = q
    roll = math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y))
    pitch = math.asin(max(-1.0, min(1.0, 2 * (w * y - z * x))))
    return roll, pitch


def quat_to_yaw(q) -> float:
    w, x, y, z = q
    return math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))


def non_foot_floor_contact(model: mujoco.MjModel, data: mujoco.MjData) -> bool:
    """True when something other than a foot ball is on the floor (a shin, the body, the head)."""
    floor = model.geom("floor").id
    for c in data.contact[: data.ncon]:
        if floor not in (c.geom1, c.geom2):
            continue
        other = c.geom2 if c.geom1 == floor else c.geom1
        if model.geom_type[other] != mujoco.mjtGeom.mjGEOM_SPHERE:
            return True
    return False


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gait", default="tripod", choices=["tripod", "ripple", "wave"])
    ap.add_argument("--seconds", type=float, default=6.0, help="how long to walk forward")
    ap.add_argument("--speed", type=float, default=SPEED_MPS, help="forward speed, m/s")
    ap.add_argument("--turn", type=float, default=0.0, help="turn in place instead, rad/s")
    ap.add_argument("--cycle", type=float, default=None, help="gait cycle, s (default from GaitParams)")
    ap.add_argument("--step", type=float, default=None, help="longest stride per cycle, m")
    ap.add_argument("--lift", type=float, default=None, help="swing height, m")
    ap.add_argument("--kp", type=float, default=KP, help="position servo stiffness, N m / rad")
    ap.add_argument("--no-gif", action="store_true")
    ap.add_argument("--out", default=str(HERE / "walk.gif"))
    args = ap.parse_args()

    model, data = load(HERE / "pincer1.urdf", kp=args.kp)
    idx = joint_index(model)
    gait = GaitParams(args.gait)
    if args.cycle is not None:
        gait.cycle_s = args.cycle
    if args.step is not None:
        gait.step_length_m = args.step
    if args.lift is not None:
        gait.step_height_m = args.lift
    body = Body(geometry=G, gait=gait)
    body.stand(actuate=False)
    speed = 0.0 if args.turn else args.speed

    def set_targets(t: dict[str, float]) -> None:
        for name, value in t.items():
            q, a = idx[name]
            data.ctrl[a] = value

    set_targets(leg_targets(body, CONTROL_DT))
    set_targets(ARM_POSE)
    for name, (q, a) in idx.items():
        data.qpos[q] = data.ctrl[a]
    mujoco.mj_forward(model, data)

    cam = mujoco.MjvCamera()
    cam.type = mujoco.mjtCamera.mjCAMERA_FREE
    # a fixed camera looking at the middle of the path: the background stays still, which is what keeps the gif small
    cam.distance, cam.azimuth, cam.elevation = 1.0, 150, -18
    cam.lookat[:] = [speed * args.seconds * 0.6, 0, 0.10]  # the body walks towards the viewer; favour the far end
    frames: list[Image.Image] = []
    substeps = int(round(CONTROL_DT / PHYSICS_DT))
    frame_every = int(round(1.0 / (FPS * CONTROL_DT)))

    phases = (("settle", 1.5, 0.0), ("walk", args.seconds, speed), ("stop", 1.0, 0.0))
    heights: list[float] = []
    tilt = [0.0, 0.0]
    x0 = x1 = None
    yaw_prev = None
    turned = 0.0  # yaw accumulated tick by tick, so a turn past 180 degrees does not wrap
    held = None
    scraped = False
    tick = 0
    renderer = None if args.no_gif else mujoco.Renderer(model, FRAME[1], FRAME[0])
    try:
        for name, seconds, v in phases:
            body.set_velocity(v, 0.0, args.turn if name == "walk" else 0.0)
            if name == "walk":
                x0, yaw_prev = float(data.qpos[0]), quat_to_yaw(data.qpos[3:7])
            for _ in range(int(seconds * TICK_HZ)):
                set_targets(leg_targets(body, CONTROL_DT))
                for _ in range(substeps):
                    mujoco.mj_step(model, data)
                if name == "walk":
                    held = body.state.actual
                    heights.append(float(data.qpos[2]))
                    yaw = quat_to_yaw(data.qpos[3:7])
                    turned += (yaw - yaw_prev + math.pi) % (2 * math.pi) - math.pi
                    yaw_prev = yaw
                    roll, pitch = quat_to_rp(data.qpos[3:7])
                    tilt = [max(tilt[0], abs(roll)), max(tilt[1], abs(pitch))]
                    scraped = scraped or non_foot_floor_contact(model, data)
                if renderer is not None and tick % frame_every == 0:
                    renderer.update_scene(data, cam)
                    frames.append(Image.fromarray(renderer.render()))
                tick += 1
            if name == "walk":
                x1 = float(data.qpos[0])
    finally:
        if renderer is not None:
            renderer.close()

    assert x0 is not None and x1 is not None
    actual = held if held is not None else body.state.actual
    if args.turn:
        print(f"{args.gait}: turned {math.degrees(turned):.1f} deg in {args.seconds:.1f} s "
              f"(commanded {math.degrees(args.turn * args.seconds):.1f} deg at {args.turn} rad/s, gait held {actual.wz:.2f} rad/s)")
    else:
        print(f"{args.gait}: walked {x1 - x0:.3f} m in {args.seconds:.1f} s "
              f"(commanded {speed * args.seconds:.3f} m at {speed} m/s, gait held {actual.vx:.3f} m/s)")
    print(f"gait: cycle {gait.cycle_s} s, step {gait.step_length_m} m, lift {gait.step_height_m} m, servo kp {args.kp}")
    print(f"body height {np.mean(heights):.3f} m mean, {np.min(heights):.3f} min (stance {G.stance_height_m:.3f})")
    print(f"max roll {math.degrees(tilt[0]):.1f} deg, max pitch {math.degrees(tilt[1]):.1f} deg")
    print("floor contact other than feet:", "yes" if scraped else "no")
    if frames:
        write_gif(frames, Path(args.out))


def write_gif(frames: list[Image.Image], path: Path) -> None:
    """Palette gif under the size limit. One palette for the whole clip and pixels that did not change
    since the previous frame left transparent, so the still background costs nothing; if that is not
    enough the frame rate drops, then the size."""
    plans = [(1, 1.0), (2, 1.0), (2, 560 / FRAME[0]), (2, 480 / FRAME[0])]
    for stride, scale in plans:
        picked = frames[::stride]
        if scale != 1.0:
            size = (int(FRAME[0] * scale), int(FRAME[1] * scale))
            picked = [f.resize(size, Image.LANCZOS) for f in picked]
        palette = picked[len(picked) // 2].quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        quantised = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in picked]
        encoded = [quantised[0]]
        prev = np.asarray(quantised[0])
        for q in quantised[1:]:
            cur = np.asarray(q)
            masked = cur.copy()
            masked[cur == prev] = 255  # transparent: keep the previous frame's pixel
            img = Image.fromarray(masked, mode="P")
            img.putpalette(q.getpalette())
            encoded.append(img)
            prev = cur
        encoded[0].save(path, save_all=True, append_images=encoded[1:], loop=0, transparency=255,
                        duration=int(1000 * stride / FPS), disposal=1, optimize=False)
        size_b = path.stat().st_size
        if size_b <= GIF_LIMIT:
            break
    print(f"gif -> {path} ({len(encoded)} frames at {FPS / stride:.0f} fps, {picked[0].size[0]}x{picked[0].size[1]}, {size_b / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
