#!/usr/bin/env python3
"""Climbs steps in MuJoCo with the body package's stair cycle and records a gif.

Same scene as ``walk.py`` plus box steps. The joint targets come from ``pincer_robot.body.climb``: the
cycle is generated from the leg kinematics, interpolated at 50 Hz, and held by the same position
servos; physics says whether the body gets up the step.

    python sim/stairs.py --riser 0.10                 # one step -> report
    python sim/stairs.py --sweep                      # 0.08 .. 0.17, one step each, table
    python sim/stairs.py --riser 0.13 --steps 3       # a staircase
    python sim/stairs.py --riser 0.10 --gif           # -> sim/stairs.gif
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

from pincer_robot.body.climb import ClimbError, ClimbParams, climb_cycle, duration, interpolate  # noqa: E402
from pincer_robot.body.controller import TICK_HZ  # noqa: E402
from pincer_robot.body.geometry import LEG_NAMES, BodyGeometry  # noqa: E402
from pincer_robot.body.kinematics import ReachError, solve_body  # noqa: E402

G = BodyGeometry()
CONTROL_DT = 1.0 / TICK_HZ
PHYSICS_DT = 0.002
ARM_POSE = {"arm_yaw": 0.0, "arm_shoulder": -0.2, "arm_elbow": 0.3, "arm_wrist": -0.1}
KV = 0.3
SERVO_NM = 3.0  # what one leg servo holds at 12 V; the 99th percentile of the sim torque must stay under it
FPS = 25
GIF_SPEEDUP = 3  # the cycle is slow and careful; the gif plays it three times faster
FRAME = (640, 400)
GIF_LIMIT = 2_500_000
SWEEP = (0.08, 0.10, 0.12, 0.13, 0.14, 0.15, 0.17)


def load(params: ClimbParams, kp: float) -> tuple[mujoco.MjModel, mujoco.MjData]:
    spec = mujoco.MjSpec.from_file(str(HERE / "pincer1.urdf"))
    spec.compiler.autolimits = True
    spec.compiler.discardvisual = False
    spec.option.timestep = PHYSICS_DT
    spec.option.integrator = mujoco.mjtIntegrator.mjINT_IMPLICITFAST
    spec.visual.global_.offwidth, spec.visual.global_.offheight = 1280, 800
    floor = spec.worldbody.add_geom(
        name="floor", type=mujoco.mjtGeom.mjGEOM_PLANE, size=[4, 4, 0.1], rgba=[0.86, 0.86, 0.85, 1]
    )
    floor.friction = [1.0, 0.01, 0.001]
    # the steps: the climb frame's floor is at -stance_height, the world floor at 0, so a riser edge
    # at x_e in the climb frame is a box from x_e to x_e + tread, k risers tall
    t = params.terrain
    for k, edge in enumerate(t.edges()):
        top = (k + 1) * t.riser_m
        length = t.tread_m if k < t.steps - 1 else 1.2  # the landing runs on
        g = spec.worldbody.add_geom(
            name=f"step{k}", type=mujoco.mjtGeom.mjGEOM_BOX,
            size=[length / 2, 0.6, top / 2], pos=[edge + length / 2, 0, top / 2], rgba=[0.72, 0.70, 0.66, 1],
        )
        g.friction = [1.0, 0.01, 0.001]
    spec.worldbody.add_light(pos=[0.5, -0.8, 1.6], dir=[-0.2, 0.4, -1])
    spec.add_texture(name="sky", type=mujoco.mjtTexture.mjTEXTURE_SKYBOX, builtin=mujoco.mjtBuiltin.mjBUILTIN_GRADIENT,
                     rgb1=[0.97, 0.97, 0.96], rgb2=[0.80, 0.81, 0.82], width=256, height=256)
    body = spec.body("body")
    body.add_freejoint()
    body.pos = [0, 0, G.stance_height_m + 0.01]
    for j in spec.joints:
        if j.type == mujoco.mjtJoint.mjJNT_HINGE:
            a = spec.add_actuator(name=j.name, target=j.name, trntype=mujoco.mjtTrn.mjTRN_JOINT)
            a.set_to_position(kp=kp, kv=KV)
    model = spec.compile()
    return model, mujoco.MjData(model)


def joint_index(model: mujoco.MjModel) -> dict[str, tuple[int, int]]:
    return {model.actuator(i).name: (model.joint(model.actuator(i).name).qposadr[0], i) for i in range(model.nu)}


def targets_from(angles) -> dict[str, float]:
    out: dict[str, float] = {}
    for i, a in enumerate(angles):
        n = LEG_NAMES[i].lower()
        out[f"{n}_coxa"], out[f"{n}_femur"], out[f"{n}_tibia"] = a.coxa, a.femur, a.tibia - math.pi
    return out


def quat_to_rp(q) -> tuple[float, float]:
    w, x, y, z = q
    roll = math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y))
    pitch = math.asin(max(-1.0, min(1.0, 2 * (w * y - z * x))))
    return roll, pitch


def non_foot_contact(model: mujoco.MjModel, data: mujoco.MjData, ground: set[int]) -> str | None:
    for c in data.contact[: data.ncon]:
        if c.geom1 in ground and c.geom2 in ground:
            continue
        if c.geom1 not in ground and c.geom2 not in ground:
            continue
        other = c.geom2 if c.geom1 in ground else c.geom1
        if model.geom_type[other] != mujoco.mjtGeom.mjGEOM_SPHERE:
            return model.geom(other).name or model.body(model.geom_bodyid[other]).name
    return None


def run(params: ClimbParams, kp: float, gif: Path | None = None, cam_follow: bool = False) -> dict:
    try:
        kfs = climb_cycle(G, params)
    except ClimbError as e:
        return {"riser": params.riser_m, "ok": False, "why": f"kinematics: {e}"}
    model, data = load(params, kp)
    idx = joint_index(model)
    ground = {model.geom("floor").id} | {model.geom(f"step{k}").id for k in range(params.terrain.steps)}
    legs = [i for name, (q, i) in idx.items() if not name.startswith("arm")]

    def set_targets(t: dict[str, float]) -> None:
        for name, value in t.items():
            data.ctrl[idx[name][1]] = value

    samples = list(interpolate(kfs, CONTROL_DT))
    set_targets(targets_from(solve_body(G, *samples[0])))
    set_targets(ARM_POSE)
    for name, (q, a) in idx.items():
        data.qpos[q] = data.ctrl[a]
    mujoco.mj_forward(model, data)

    cam = mujoco.MjvCamera()
    cam.type = mujoco.mjtCamera.mjCAMERA_FREE
    cam.distance, cam.azimuth, cam.elevation = 1.25, 125, -16
    x_mid = params.terrain.edges()[-1] / 2 + 0.1
    cam.lookat[:] = [x_mid, 0, params.terrain.steps * params.riser_m / 2 + 0.08]
    frames: list[Image.Image] = []
    substeps = int(round(CONTROL_DT / PHYSICS_DT))
    frame_every = int(round(1.0 / (FPS * CONTROL_DT)))
    t = params.terrain
    tread_z = [G.stance_height_m + (k + 1) * t.riser_m for k in range(t.steps)]  # body height standing on tread k
    reached = [False] * t.steps
    tilt = [0.0, 0.0]
    peak_torque = 0.0
    torque_ticks: list[float] = []
    scrape: str | None = None
    scrape_at: str | None = None
    fell_at: str | None = None
    settle = int(1.0 * TICK_HZ)
    hold = int(1.5 * TICK_HZ)
    renderer = mujoco.Renderer(model, FRAME[1], FRAME[0]) if gif else None
    try:
        for tick in range(settle + len(samples) + hold):
            if settle <= tick < settle + len(samples):
                feet, pose = samples[tick - settle]
                try:
                    set_targets(targets_from(solve_body(G, feet, pose)))
                except ReachError:
                    pass  # the keyframes solve; a rounding miss between two keeps the last target
            for _ in range(substeps):
                mujoco.mj_step(model, data)
            x, z = float(data.qpos[0]), float(data.qpos[2])
            roll, pitch = quat_to_rp(data.qpos[3:7])
            tilt = [max(tilt[0], abs(roll)), max(tilt[1], abs(pitch))]
            if fell_at is None and (abs(roll) > 0.6 or abs(pitch) > 0.6):
                i = tick - settle
                fell_at = f"{i * CONTROL_DT:.1f} s" if 0 <= i < len(samples) else ("settle" if i < 0 else "hold")
            tick_torque = float(np.max(np.abs(data.actuator_force[legs])))
            peak_torque = max(peak_torque, tick_torque)
            torque_ticks.append(tick_torque)
            if scrape is None:
                scrape = non_foot_contact(model, data, ground)
                if scrape:
                    i = min(max(0, tick - settle), len(samples) - 1)
                    scrape_at = f"{i * CONTROL_DT:.1f} s" if settle <= tick < settle + len(samples) else ("settle" if tick < settle else "hold")
            for k, edge in enumerate(t.edges()):
                if x > edge + 0.02 and abs(z - tread_z[k]) < 0.03:
                    reached[k] = True
            if renderer and tick % frame_every == 0:
                if cam_follow:
                    cam.lookat[0] = x
                renderer.update_scene(data, cam)
                frames.append(Image.fromarray(renderer.render()))
    finally:
        if renderer:
            renderer.close()
    x, z = float(data.qpos[0]), float(data.qpos[2])
    roll, pitch = quat_to_rp(data.qpos[3:7])
    up = abs(z - tread_z[-1]) < 0.03 and x > t.edges()[-1] + 0.02 and abs(roll) < 0.35 and abs(pitch) < 0.35
    standing = z > 0.05 and abs(roll) < 0.35 and abs(pitch) < 0.35
    p99 = float(np.percentile(torque_ticks, 99))
    result = {
        "riser": params.riser_m, "steps": t.steps, "kp": kp, "ok": up and p99 <= SERVO_NM,
        "up": up, "standing": standing, "reached": reached, "final_x": x, "final_z": z,
        "max_roll_deg": math.degrees(tilt[0]), "max_pitch_deg": math.degrees(tilt[1]),
        "peak_torque_nm": peak_torque, "p99_torque_nm": p99,
        "scrape": scrape, "scrape_at": scrape_at, "fell_at": fell_at, "seconds": duration(kfs),
    }
    if gif and frames:
        write_gif(frames, gif)
    return result


def describe(r: dict) -> str:
    if "why" in r:
        return f"riser {r['riser']:.2f}: no cycle ({r['why']})"
    phase = "up" if r["up"] else ((f"fell at {r['fell_at']}" if r["fell_at"] else "fell") if not r["standing"] else "stuck on the lower level")
    return (f"riser {r['riser']:.2f} x{r['steps']} kp {r['kp']:.0f}: {'ok' if r['ok'] else 'no'} ({phase}); "
            f"reached {sum(r['reached'])}/{r['steps']}, roll {r['max_roll_deg']:.1f} deg, pitch {r['max_pitch_deg']:.1f} deg, "
            f"joint torque p99 {r['p99_torque_nm']:.2f} / peak {r['peak_torque_nm']:.2f} N m, "
            f"scrape {r['scrape'] or 'none'}{(' at ' + r['scrape_at']) if r['scrape'] else ''}, {r['seconds']:.0f} s")


def write_gif(frames: list[Image.Image], path: Path) -> None:
    """Same recipe as walk.py: one palette, unchanged pixels transparent, then fewer frames, then smaller."""
    plans = [(1, 1.0), (2, 1.0), (2, 560 / FRAME[0]), (3, 480 / FRAME[0]), (4, 480 / FRAME[0]), (5, 480 / FRAME[0]), (5, 400 / FRAME[0]), (6, 400 / FRAME[0])]
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
            masked[cur == prev] = 255
            img = Image.fromarray(masked, mode="P")
            img.putpalette(q.getpalette())
            encoded.append(img)
            prev = cur
        encoded[0].save(path, save_all=True, append_images=encoded[1:], loop=0, transparency=255,
                        duration=int(1000 * stride / (FPS * GIF_SPEEDUP)), disposal=1, optimize=False)
        size_b = path.stat().st_size
        if size_b <= GIF_LIMIT:
            break
    print(f"gif -> {path} ({len(encoded)} frames at {FPS / stride:.0f} fps played {GIF_SPEEDUP}x, {picked[0].size[0]}x{picked[0].size[1]}, {size_b / 1e6:.2f} MB)")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--riser", type=float, default=0.10)
    ap.add_argument("--tread", type=float, default=0.28)
    ap.add_argument("--steps", type=int, default=1)
    ap.add_argument("--kp", type=float, default=16.0)
    ap.add_argument("--sweep", action="store_true")
    ap.add_argument("--gif", action="store_true")
    ap.add_argument("--out", default=str(HERE / "stairs.gif"))
    args = ap.parse_args()
    if args.sweep:
        for riser in SWEEP:
            print(describe(run(ClimbParams(riser_m=riser, tread_m=args.tread, steps=args.steps), args.kp)), flush=True)
        return
    r = run(ClimbParams(riser_m=args.riser, tread_m=args.tread, steps=args.steps), args.kp,
            gif=Path(args.out) if args.gif else None)
    print(describe(r))


if __name__ == "__main__":
    main()
