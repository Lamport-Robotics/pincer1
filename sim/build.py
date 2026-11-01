#!/usr/bin/env python3
"""Writes pincer1.urdf from the numbers in robot/pincer_robot/body/geometry.py and docs/HARDWARE.md.

Six legs of coxa / femur / tibia, a four-axis arm on the top plate with the camera head on its wrist, the
consent pad and the front display as fixed links. Visuals are plain cylinders and boxes sized from the
segment lengths, so the file needs no mesh folder and loads anywhere that reads URDF (MuJoCo, Isaac,
Gazebo, SimArena). The CAD shells come later as optional meshes in the same frames.

    python sim/build.py            # -> sim/pincer1.urdf
    python sim/build.py --check    # also loads it in MuJoCo, drops it on a floor, reports the stance
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "robot"))

from pincer_robot.body.geometry import LEG_NAMES, BodyGeometry  # noqa: E402

G = BodyGeometry()

# ---- the body as a parts list -------------------------------------------------------------------

# the body frame origin is the centre of the top plate at coxa-axis height (geometry.py); the body hangs below it
BODY_L, BODY_W, BODY_H = 0.210, 0.150, 0.045  # top-plate footprint and the depth of the body between plates
BODY_MASS = 1.70  # plates, Jetson, battery pack, bus driver, consent module
SERVO_MASS = 0.060  # one bus servo with its bracket
SEG_R = 0.014  # radius of the leg / arm segment visuals
FOOT_R = 0.009  # the foot ball
SHIN_GAP = 0.012  # the shin cylinder ends this far above the foot joint, so the ball, not the rim, touches down

# arm: yaw at the base, three pitch joints, camera head on the last link
ARM_BASE = (0.060, 0.0, 0.0)  # where the arm sits on the top plate (body frame)
ARM_SEG = (0.043, 0.080, 0.080, 0.050)  # base riser, upper, forearm, wrist-to-camera
ARM_LIMIT = math.radians(120)
HEAD_MASS = 0.120  # depth camera, shell, trusted display window

PAD_POS = (-0.020, 0.0, 0.002)  # consent touch pad, behind the arm base (HARDWARE.md)
DISPLAY_POS = (BODY_L / 2 + 0.004, 0.0, -BODY_H / 2)  # front display panel


def fmt(v: float) -> str:
    s = f"{v:.5f}".rstrip("0").rstrip(".")
    return "0" if s in ("", "-0") else s


def xyz(a) -> str:
    return " ".join(fmt(x) for x in a)


def inertia_box(m, x, y, z):
    return (m / 12 * (y * y + z * z), m / 12 * (x * x + z * z), m / 12 * (x * x + y * y))


def inertia_rod(m, length, r):
    # a solid cylinder along its own x axis
    a = m * r * r / 2
    b = m * (3 * r * r + length * length) / 12
    return (a, b, b)


out: list[str] = []


def w(s: str = "") -> None:
    out.append(s)


def link(name, mass, inertia, visual, collision=None, origin=(0, 0, 0), rpy=(0, 0, 0), color="shell"):
    ixx, iyy, izz = inertia
    w(f'  <link name="{name}">')
    w("    <inertial>")
    w(f'      <origin xyz="{xyz(origin)}" rpy="{xyz(rpy)}"/>')
    w(f'      <mass value="{fmt(mass)}"/>')
    w(f'      <inertia ixx="{ixx:.3e}" ixy="0" ixz="0" iyy="{iyy:.3e}" iyz="0" izz="{izz:.3e}"/>')
    w("    </inertial>")
    for tag, geom in (("visual", visual), ("collision", collision or visual)):
        w(f"    <{tag}>")
        w(f'      <origin xyz="{xyz(origin)}" rpy="{xyz(rpy)}"/>')
        w(f"      <geometry>{geom}</geometry>")
        if tag == "visual":
            w(f'      <material name="{color}"/>')
        w(f"    </{tag}>")
    w("  </link>")


def frame(name):
    """A massless link that only marks a frame (foot tip, camera optical centre, pad)."""
    w(f'  <link name="{name}"/>')


def joint(name, parent, child, origin, rpy=(0, 0, 0), axis=None, limits=None, effort=3.0, velocity=6.0):
    kind = "revolute" if axis else "fixed"
    w(f'  <joint name="{name}" type="{kind}">')
    w(f'    <origin xyz="{xyz(origin)}" rpy="{xyz(rpy)}"/>')
    w(f'    <parent link="{parent}"/>')
    w(f'    <child link="{child}"/>')
    if axis:
        lo, hi = limits
        w(f'    <axis xyz="{xyz(axis)}"/>')
        w(f'    <limit lower="{fmt(lo)}" upper="{fmt(hi)}" effort="{fmt(effort)}" velocity="{fmt(velocity)}"/>')
        w('    <dynamics damping="0.05" friction="0.02"/>')
    w("  </joint>")


def rod(length):
    return f'<cylinder radius="{fmt(SEG_R)}" length="{fmt(length)}"/>'


def build() -> str:
    out.clear()
    w('<?xml version="1.0"?>')
    w('<robot name="pincer1">')
    w('  <material name="shell"><color rgba="0.13 0.13 0.14 1"/></material>')
    w('  <material name="metal"><color rgba="0.62 0.64 0.66 1"/></material>')
    w('  <material name="glass"><color rgba="0.05 0.08 0.12 1"/></material>')
    w('  <material name="mark"><color rgba="0.93 0.76 0.26 1"/></material>')
    w()

    # body
    link("body", BODY_MASS, inertia_box(BODY_MASS, BODY_L, BODY_W, BODY_H),
         f'<box size="{fmt(BODY_L)} {fmt(BODY_W)} {fmt(BODY_H)}"/>', origin=(0, 0, -BODY_H / 2))
    link("display", 0.08, inertia_box(0.08, 0.006, 0.110, 0.040),
         '<box size="0.006 0.110 0.040"/>', color="glass")
    joint("display_mount", "body", "display", DISPLAY_POS)
    link("consent_pad", 0.01, inertia_box(0.01, 0.030, 0.030, 0.003),
         '<box size="0.030 0.030 0.003"/>', color="mark")
    joint("consent_pad_mount", "body", "consent_pad", PAD_POS)
    w()

    # legs: coxa yaw about z, femur and tibia pitch about the leg's y
    lim = G.limits
    for i, leg in enumerate(LEG_NAMES):
        n = leg.lower()
        mx, my, yaw = G.mount(i)
        w(f"  <!-- leg {leg} -->")
        joint(f"{n}_coxa", "body", f"{n}_coxa_link", (mx, my, 0), (0, 0, yaw), (0, 0, 1), lim.coxa)
        link(f"{n}_coxa_link", SERVO_MASS, inertia_rod(SERVO_MASS, G.coxa_m, SEG_R), rod(G.coxa_m),
             origin=(G.coxa_m / 2, 0, 0), rpy=(0, math.pi / 2, 0), color="metal")
        # femur pitch: positive lifts the knee (geometry.py: pitch from horizontal, z up)
        joint(f"{n}_femur", f"{n}_coxa_link", f"{n}_femur_link", (G.coxa_m, 0, 0), (0, 0, 0), (0, -1, 0), lim.femur)
        link(f"{n}_femur_link", SERVO_MASS + 0.02, inertia_rod(SERVO_MASS + 0.02, G.femur_m, SEG_R), rod(G.femur_m),
             origin=(G.femur_m / 2, 0, 0), rpy=(0, math.pi / 2, 0))
        # tibia: the joint angle is the interior knee angle; pi means the shin continues the femur, so the
        # link hangs at (angle - pi) below the femur line. The offset keeps the URDF zero at a straight leg.
        klo, khi = lim.tibia
        joint(f"{n}_tibia", f"{n}_femur_link", f"{n}_tibia_link", (G.femur_m, 0, 0), (0, 0, 0), (0, -1, 0),
              (klo - math.pi, khi - math.pi))
        # the shin cylinder stops short of the tip so that only the foot ball below it meets the ground;
        # the kinematic length (where the foot joint sits) stays G.tibia_m
        shin = G.tibia_m - SHIN_GAP
        link(f"{n}_tibia_link", 0.045, inertia_rod(0.045, G.tibia_m, SEG_R), rod(shin),
             origin=(shin / 2, 0, 0), rpy=(0, math.pi / 2, 0))
        # the foot is a small rubber ball: the contact point for simulators and the frame the IK solves for
        link(f"{n}_foot", 0.006, (1e-7, 1e-7, 1e-7), f'<sphere radius="{fmt(FOOT_R)}"/>', color="shell")
        joint(f"{n}_foot_tip", f"{n}_tibia_link", f"{n}_foot", (G.tibia_m, 0, 0))
        w()

    # arm and head
    w("  <!-- arm: yaw, shoulder, elbow, wrist; the camera head rides the wrist -->")
    a0, a1, a2, a3 = ARM_SEG
    joint("arm_yaw", "body", "arm_base_link", ARM_BASE, (0, 0, 0), (0, 0, 1), (-math.pi, math.pi))
    link("arm_base_link", SERVO_MASS, inertia_rod(SERVO_MASS, a0, SEG_R), rod(a0), origin=(0, 0, a0 / 2), color="metal")
    joint("arm_shoulder", "arm_base_link", "arm_upper_link", (0, 0, a0), (0, 0, 0), (0, 1, 0), (-ARM_LIMIT, ARM_LIMIT))
    link("arm_upper_link", SERVO_MASS + 0.02, inertia_rod(SERVO_MASS + 0.02, a1, SEG_R), rod(a1), origin=(0, 0, a1 / 2))
    joint("arm_elbow", "arm_upper_link", "arm_fore_link", (0, 0, a1), (0, 0, 0), (0, 1, 0), (-ARM_LIMIT, ARM_LIMIT))
    link("arm_fore_link", SERVO_MASS + 0.02, inertia_rod(SERVO_MASS + 0.02, a2, SEG_R), rod(a2), origin=(0, 0, a2 / 2))
    joint("arm_wrist", "arm_fore_link", "head", (0, 0, a2), (0, 0, 0), (0, 1, 0), (-ARM_LIMIT, ARM_LIMIT))
    # the head: a short wrist riser and the camera block on top of it
    link("head", HEAD_MASS, inertia_box(HEAD_MASS, 0.030, 0.090, 0.040), '<box size="0.030 0.090 0.040"/>',
         origin=(0.0, 0, a3))
    w('  <joint name="head_riser_mount" type="fixed"><origin xyz="0 0 0"/><parent link="head"/><child link="head_riser"/></joint>')
    link("head_riser", 0.02, inertia_rod(0.02, a3, SEG_R), rod(a3), origin=(0, 0, a3 / 2), color="metal")
    frame("camera")
    # optical frame on the head's front face: x forward out of the lens (ROS body convention)
    joint("camera_mount", "head", "camera", (0.016, 0, a3))
    frame("trusted_display")
    joint("trusted_display_mount", "head", "trusted_display", (0.016, 0, a3 - 0.014))
    w("</robot>")
    return "\n".join(out) + "\n"


# ---- optional MuJoCo check -----------------------------------------------------------------------

def check(urdf_path: Path) -> None:
    """Load the URDF in MuJoCo, put the body on a floor in the neutral stance, hold the joints with soft
    position servos for three seconds and report what the body did. A wrong axis or limit shows up here as
    a leg folding the wrong way, a body height far from the stance height, or a tilt."""
    import mujoco
    import numpy as np

    from pincer_robot.body.kinematics import solve_leg

    spec = mujoco.MjSpec.from_file(str(urdf_path))
    spec.compiler.autolimits = True
    spec.option.timestep = 0.002
    spec.visual.global_.offwidth, spec.visual.global_.offheight = 1280, 800
    floor = spec.worldbody.add_geom(name="floor", type=mujoco.mjtGeom.mjGEOM_PLANE, size=[2, 2, 0.1], rgba=[0.86, 0.86, 0.85, 1])
    floor.friction = [0.9, 0.01, 0.001]
    spec.worldbody.add_light(pos=[0.5, -0.5, 1.5], dir=[-0.3, 0.3, -1])
    spec.option.integrator = mujoco.mjtIntegrator.mjINT_IMPLICITFAST
    body = spec.body("body")
    body.add_freejoint()
    body.pos = [0, 0, G.stance_height_m + 0.01]
    # one position servo per hinge, the same model walk.py and stairs.py use
    for j in spec.joints:
        if j.type == mujoco.mjtJoint.mjJNT_HINGE:
            a = spec.add_actuator(name=j.name, target=j.name, trntype=mujoco.mjtTrn.mjTRN_JOINT)
            a.set_to_position(kp=16.0, kv=0.3)
    model = spec.compile()
    data = mujoco.MjData(model)

    def set_joint(name: str, value: float) -> None:
        data.qpos[model.joint(name).qposadr[0]] = value
        data.ctrl[model.actuator(name).id] = value

    for i, leg in enumerate(LEG_NAMES):
        n = leg.lower()
        a = solve_leg(G, i, G.neutral_foot(i))
        set_joint(f"{n}_coxa", a.coxa)
        set_joint(f"{n}_femur", a.femur)
        set_joint(f"{n}_tibia", a.tibia - math.pi)
    set_joint("arm_shoulder", -0.6)
    set_joint("arm_elbow", 1.1)
    set_joint("arm_wrist", -0.5)
    mujoco.mj_forward(model, data)

    for _ in range(1500):
        mujoco.mj_step(model, data)

    roll, pitch = np.degrees(quat_to_rp(data.qpos[3:7]))
    # MuJoCo fuses fixed children into their parent, so the foot is the tibia tip
    feet_z = []
    for l in LEG_NAMES:
        b = model.body(f"{l.lower()}_tibia_link").id
        tip = data.xpos[b] + data.xmat[b].reshape(3, 3) @ np.array([G.tibia_m, 0, 0])
        feet_z.append(float(tip[2]) - FOOT_R)
    print(f"{model.njnt - 1} revolute joints, {model.nbody - 1} bodies, {float(sum(model.body_mass)):.2f} kg")
    print(f"body height {float(data.qpos[2]):.3f} m (stance {G.stance_height_m:.3f}), roll {roll:.1f} deg, pitch {pitch:.1f} deg")
    print("feet above floor (mm):", [round(z * 1000, 1) for z in feet_z])
    render(model, data, urdf_path.with_suffix(".png"))


def quat_to_rp(q):
    w_, x, y, z = q
    roll = math.atan2(2 * (w_ * x + y * z), 1 - 2 * (x * x + y * y))
    pitch = math.asin(max(-1, min(1, 2 * (w_ * y - z * x))))
    return roll, pitch


def render(model, data, path: Path) -> None:
    import mujoco
    from PIL import Image

    cam = mujoco.MjvCamera()
    cam.type = mujoco.mjtCamera.mjCAMERA_FREE
    cam.lookat[:] = [0.02, 0, 0.08]
    cam.distance, cam.azimuth, cam.elevation = 0.95, 150, -18
    with mujoco.Renderer(model, 720, 1080) as r:
        r.update_scene(data, cam)
        Image.fromarray(r.render()).save(path)
    print(f"render -> {path}")


def write_mjcf(urdf_path: Path) -> None:
    """The same robot as a MuJoCo file, for people who start from MJCF."""
    import mujoco

    spec = mujoco.MjSpec.from_file(str(urdf_path))
    spec.modelname = "pincer1"
    spec.compiler.autolimits = True
    spec.compile()
    xml = spec.to_xml()
    dst = urdf_path.with_suffix(".xml")
    dst.write_text(xml)
    print(f"wrote {dst}")


if __name__ == "__main__":
    dst = HERE / "pincer1.urdf"
    dst.write_text(build())
    print(f"wrote {dst}")
    if "--check" in sys.argv:
        write_mjcf(dst)
        check(dst)
