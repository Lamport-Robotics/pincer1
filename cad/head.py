"""Pincer1's head, as code: the shell that rides on the camera arm's wrist.

Three printed parts, millimetres, same frame as the robot package (x forward out of the lens,
y left, z up). The owner leans over the robot and sees, top to bottom: the depth camera, the trusted
display behind its window, the consent pad under the shell skin.

    camera_bracket   cradle for the depth camera on the roof of the housing, one 1/4"-20 bolt from below
    head_housing     shell with the display window and the pad dimple; open at the back
    wrist_adapter    plate that closes the back and bolts to the arm's last servo horn

Every dimension that depends on a bought part sits in the dataclasses below; measure, edit, rerun.
`python cad/head.py` writes STL (print orientation), STEP (editing) and PNG previews to cad/out/.
"""

from __future__ import annotations

import math
import sys
from dataclasses import dataclass
from pathlib import Path

import cadquery as cq

OUT = Path(__file__).with_name("out")


@dataclass(frozen=True)
class Oled:
    """2.42" 128x64 SSD1309 module, 7-pin SPI header on the back edge."""

    pcb_w: float = 60.5  # y
    pcb_h: float = 37.0  # z
    pcb_t: float = 1.6
    hole_d: float = 2.6  # m2.5 clearance
    hole_pitch_w: float = 52.0
    hole_pitch_h: float = 29.0
    glass_w: float = 57.0  # window, slightly larger than the active area
    glass_h: float = 29.0
    glass_dz: float = 1.0  # active area sits a little above the pcb centre; + is up
    front_t: float = 2.0  # glass above the pcb face
    rear_t: float = 6.0  # header pins and components behind the pcb


@dataclass(frozen=True)
class Pad:
    """TTP223 touch module; it senses through the shell skin, no cutout."""

    w: float = 15.0  # y
    h: float = 11.0  # z
    t: float = 2.2  # board plus the pad side components
    skin_t: float = 1.2  # shell thickness over the pad; keep under 2 mm for a clean trigger
    dimple_d: float = 10.0  # sphere radius of the finger dimple outside, marks where to touch
    dimple_depth: float = 0.5  # leaves skin_t - dimple_depth of plastic over the pad centre


@dataclass(frozen=True)
class Camera:
    """Depth camera body: 91 x 28 x 17.5 mm, 1/4"-20 tripod socket centred on the bottom face."""

    w: float = 91.0  # y, lenses along this edge
    d: float = 28.0  # x, front (lenses) to back (usb-c)
    h: float = 17.5
    bolt_d: float = 6.6  # 1/4" clearance
    bolt_head_d: float = 11.5  # socket head counterbore from below
    bolt_head_h: float = 5.0
    lip: float = 5.0  # how high the cradle wraps the camera's lower edge


@dataclass(frozen=True)
class Horn:
    """Round horn on the arm's last bus servo. Measure on the arm before printing."""

    bolt_circle_d: float = 14.0
    screws: int = 4
    screw_d: float = 2.3  # m2 clearance
    boss_d: float = 6.0  # the horn's centre boss
    boss_depth: float = 1.5  # pocket so the plate sits flat on the horn


@dataclass(frozen=True)
class Shell:
    wall: float = 2.5
    fillet: float = 3.0
    clear: float = 0.5  # fit gap around boards
    standoff: float = 3.0  # pcb face to the back of the front wall
    insert_d: float = 4.0  # heat-set m3 insert hole in the corner bosses
    boss_d: float = 7.0
    screw_d: float = 3.4  # m3 clearance in the mating plates
    insert_depth: float = 4.5  # heat-set insert pocket
    gap: float = 3.0  # space between the display pcb and the pad board


OLED, PAD, CAM, HORN, SH = Oled(), Pad(), Camera(), Horn(), Shell()


# ----------------------------------------------------------------------------------------------
# derived layout (assembly frame: housing back face at x = 0, front face at x = DEPTH, centred on y, floor at z = 0)

INNER_W = OLED.pcb_w + 2 * SH.clear
INNER_H = SH.boss_d + SH.clear + OLED.pcb_h + SH.gap + PAD.h + SH.clear  # boss row above the board, pad row below
WIDTH = INNER_W + 2 * SH.wall
HEIGHT = INNER_H + 2 * SH.wall
CAVITY = SH.standoff + OLED.pcb_t + OLED.rear_t  # front wall to the open back
DEPTH = SH.wall + CAVITY
PLATE_T = 3.0

# z of part centres: display at the top of the cavity, pad below it
Z_OLED = HEIGHT - SH.wall - SH.boss_d - SH.clear - OLED.pcb_h / 2
Z_PAD = SH.wall + SH.clear + PAD.h / 2
# four m3 bosses for the back plate: a row above the board, a row beside the pad; the top pair also takes the bracket screws from the roof
BITE = 0.6  # bosses sink this far into the wall and floor/roof so the union has no tangent seam
BOSS_XY = [(sy * (INNER_W / 2 - SH.boss_d / 2 + BITE), sz) for sy in (-1, 1) for sz in (SH.wall + SH.boss_d / 2 - BITE, HEIGHT - SH.wall - SH.boss_d / 2 + BITE)]
ROOF_HOLES = [(sy * (INNER_W / 2 - SH.boss_d / 2 + BITE), DEPTH / 2) for sy in (-1, 1)]  # (y, x) on the roof, into the top bosses


def head_housing() -> cq.Workplane:
    """Shell open at the back. Window for the display, thin skin with a dimple over the pad."""
    outer = (
        cq.Workplane("XY")
        .box(DEPTH, WIDTH, HEIGHT, centered=(False, True, False))
        .edges("|X").fillet(SH.fillet)
    )
    # cavity, open at the back (x = 0)
    cavity = cq.Workplane("XY").box(CAVITY, INNER_W, INNER_H, centered=(False, True, False)).translate((0, 0, SH.wall))
    body = outer.cut(cavity)
    # corner bosses, full depth, with insert holes from the back
    for y, z in BOSS_XY:
        boss = cq.Workplane("YZ").center(y, z).circle(SH.boss_d / 2).extrude(CAVITY)
        body = body.union(boss.intersect(outer))
        hole = cq.Workplane("YZ").center(y, z).circle(SH.insert_d / 2).extrude(SH.insert_depth)
        body = body.cut(hole)
    # display window through the front wall, with a small chamfer outside
    win = (
        cq.Workplane("YZ").workplane(offset=DEPTH - SH.wall - 0.01)
        .center(0, Z_OLED + OLED.glass_dz).rect(OLED.glass_w, OLED.glass_h).extrude(SH.wall + 0.02)
    )
    body = body.cut(win)
    # display standoffs with m2.5 holes
    for sy in (-1, 1):
        for sz in (-1, 1):
            y, z = sy * OLED.hole_pitch_w / 2, Z_OLED + sz * OLED.hole_pitch_h / 2
            post = cq.Workplane("YZ").workplane(offset=DEPTH - SH.wall - SH.standoff).center(y, z).circle(2.8).extrude(SH.standoff)
            body = body.union(post)
            body = body.cut(cq.Workplane("YZ").workplane(offset=DEPTH - SH.wall - SH.standoff - 0.01).center(y, z).circle(1.1).extrude(SH.standoff + 0.01))
    # pad: pocket behind a thin skin, finger dimple outside
    pocket_depth = SH.wall - PAD.skin_t
    pocket = (
        cq.Workplane("YZ").workplane(offset=DEPTH - SH.wall - 0.01)
        .center(0, Z_PAD).rect(PAD.w + 2 * SH.clear, PAD.h + 2 * SH.clear).extrude(pocket_depth + 0.01)
    )
    body = body.cut(pocket)
    # two clips either side of the pad board keep it against the skin
    for sy in (-1, 1):
        clip = (
            cq.Workplane("YZ").workplane(offset=DEPTH - SH.wall)
            .center(sy * (PAD.w / 2 + SH.clear + 1.0), Z_PAD).rect(2.0, PAD.h).extrude(-(PAD.t + 0.6))
        )
        body = body.union(clip)
    dimple = cq.Workplane("YZ").workplane(offset=DEPTH).center(0, Z_PAD).sphere(PAD.dimple_d).translate((PAD.dimple_d - PAD.dimple_depth, 0, 0))
    body = body.cut(dimple)
    # cable slot in the floor at the back edge: display and pad leads leave downwards into the arm
    slot = cq.Workplane("XY").box(8.0, 12.0, SH.wall + 1, centered=(False, True, False)).translate((0, 0, -0.5))
    body = body.cut(slot)
    # roof: two insert holes for the camera bracket
    for y, x in ROOF_HOLES:
        body = body.cut(cq.Workplane("XY").workplane(offset=HEIGHT - 6.0).center(x, y).circle(SH.insert_d / 2).extrude(6.1))
    return body


def camera_bracket() -> cq.Workplane:
    """Cradle on the roof: a floor the camera stands on, a lip around its lower edge, the
    1/4"-20 bolt from below. Front and back stay open so lenses and the usb-c lead are free."""
    floor_t = 3.0
    w = CAM.w + 2 * (SH.clear + 2.0)
    d = CAM.d + 2 * (SH.clear + 2.0)
    base = cq.Workplane("XY").box(d, w, floor_t, centered=(True, True, False)).edges("|Z").fillet(2.0)
    # end walls (left/right of the camera) with a lip
    for sy in (-1, 1):
        wall = cq.Workplane("XY").box(d, 2.0, floor_t + CAM.lip, centered=(True, True, False)).translate((0, sy * (w / 2 - 1.0), 0))
        base = base.union(wall)
    # low rails along the front and back edges, just enough to locate the body, no lens cover
    for sx in (-1, 1):
        rail = cq.Workplane("XY").box(2.0, w - 4.0, floor_t + 2.0, centered=(True, True, False)).translate((sx * (d / 2 - 1.0), 0, 0))
        base = base.union(rail)
    # 1/4"-20 clearance with a counterbore from below
    base = base.cut(cq.Workplane("XY").circle(CAM.bolt_d / 2).extrude(floor_t + CAM.lip))
    base = base.cut(cq.Workplane("XY").circle(CAM.bolt_head_d / 2).extrude(min(CAM.bolt_head_h, floor_t - 1.0)))
    # two m3 clearance holes down into the housing roof
    for y, x in ROOF_HOLES:
        base = base.cut(cq.Workplane("XY").center(x - DEPTH / 2, y).circle(SH.screw_d / 2).extrude(floor_t + 0.1))
    return base.translate((DEPTH / 2, 0, HEIGHT))


def wrist_adapter() -> cq.Workplane:
    """Closes the back of the housing and bolts to the servo horn. Four m3 countersunk screws into
    the housing's corner bosses; the horn pattern is in `Horn`."""
    plate = (
        cq.Workplane("XY").box(PLATE_T, WIDTH, HEIGHT, centered=(False, True, False))
        .edges("|X").fillet(SH.fillet)
        .translate((-PLATE_T, 0, 0))
    )
    for y, z in BOSS_XY:
        plate = plate.cut(cq.Workplane("YZ").workplane(offset=-PLATE_T - 0.01).center(y, z).circle(SH.screw_d / 2).extrude(PLATE_T + 0.02))
        plate = plate.cut(cq.Workplane("YZ").workplane(offset=-PLATE_T - 0.01).center(y, z).circle(SH.screw_d / 2 + 1.6).extrude(1.2))  # countersink seat
    # horn pattern, centred on the display window so the camera's weight sits over the wrist
    zc = HEIGHT / 2
    plate = plate.cut(cq.Workplane("YZ").workplane(offset=-PLATE_T - 0.01).center(0, zc).circle(HORN.boss_d / 2 + 0.2).extrude(HORN.boss_depth + 0.01))
    for i in range(HORN.screws):
        a = 2 * math.pi * i / HORN.screws + math.pi / HORN.screws
        y, z = HORN.bolt_circle_d / 2 * math.cos(a), zc + HORN.bolt_circle_d / 2 * math.sin(a)
        plate = plate.cut(cq.Workplane("YZ").workplane(offset=-PLATE_T - 0.01).center(y, z).circle(HORN.screw_d / 2).extrude(PLATE_T + 0.02))
    # cable slot matching the housing floor
    plate = plate.cut(cq.Workplane("XY").box(PLATE_T + 0.02, 12.0, SH.wall + 1, centered=(False, True, False)).translate((-PLATE_T - 0.01, 0, -0.5)))
    return plate


# ----------------------------------------------------------------------------------------------
# export

PRINT_ORIENTATION = {
    # part: rotation (axis start, axis end, degrees) that puts the print face on the bed
    "head_housing": ((0, 0, 0), (0, 1, 0), 90),  # front face on the bed, cavity and standoffs grow upwards, no supports
    "wrist_adapter": ((0, 0, 0), (0, 1, 0), 90),  # flat, horn side and countersinks up
    "camera_bracket": None,  # already flat
}


def export(name: str, part: cq.Workplane) -> None:
    OUT.mkdir(exist_ok=True)
    rot = PRINT_ORIENTATION.get(name)
    printed = part.rotate(*rot) if rot else part
    bb = printed.val().BoundingBox()
    printed = printed.translate((-bb.xmin, -bb.ymin, -bb.zmin))
    stl = OUT / f"{name}.stl"
    cq.exporters.export(printed, str(stl), tolerance=0.05, angularTolerance=0.2)
    cq.exporters.export(part, str(OUT / f"{name}.step"))
    _seal(stl)


def _seal(stl: Path) -> None:
    """Merge the tessellation seams the kernel leaves between faces so slicers see one closed shell."""
    import trimesh

    mesh = trimesh.load(str(stl), force="mesh")
    mesh.merge_vertices()
    mesh.update_faces(mesh.nondegenerate_faces())
    mesh.update_faces(mesh.unique_faces())
    mesh.remove_unreferenced_vertices()
    if not mesh.is_watertight:
        mesh.fill_holes()
    mesh.export(str(stl))
    if not mesh.is_watertight:
        print(f"warning: {stl.name} still has open edges", file=sys.stderr)


def preview(name: str, meshes: list[tuple[str, tuple[float, float, float]]], azimuth: float = 150, elevation: float = -22) -> None:
    """A quiet shaded view on white, rendered with mujoco from the assembly-frame meshes."""
    import mujoco
    import numpy as np
    from PIL import Image

    spec = mujoco.MjSpec()
    spec.visual.global_.offwidth, spec.visual.global_.offheight = 1400, 1100
    body = spec.worldbody.add_body(name="part")
    for i, (path, rgb) in enumerate(meshes):
        spec.add_mesh(name=f"m{i}", file=path, scale=[0.001] * 3)
        body.add_geom(type=mujoco.mjtGeom.mjGEOM_MESH, meshname=f"m{i}", rgba=[*rgb, 1.0])
    spec.worldbody.add_light(pos=[0.3, -0.3, 0.5], dir=[-0.5, 0.5, -0.8], castshadow=False)
    spec.worldbody.add_light(pos=[-0.3, 0.3, 0.3], dir=[0.5, -0.5, -0.5], castshadow=False, diffuse=[0.35, 0.35, 0.35])
    model = spec.compile()
    data = mujoco.MjData(model)
    mujoco.mj_forward(model, data)
    lo, hi = model.geom_aabb[:, :3].min(0) - model.geom_aabb[:, 3:].max(0), model.geom_aabb[:, :3].max(0) + model.geom_aabb[:, 3:].max(0)
    centre = [float(x) for x in (lo + hi) / 2]
    span = float((hi - lo).max())
    renderer = mujoco.Renderer(model, 700, 550)
    cam = mujoco.MjvCamera()
    cam.lookat[:] = centre
    cam.distance = span * 2.8
    cam.azimuth, cam.elevation = azimuth, elevation
    renderer.update_scene(data, cam)
    rgb = renderer.render().copy()
    renderer.enable_depth_rendering()
    renderer.update_scene(data, cam)
    depth = renderer.render()
    rgb[depth >= depth.max() - 1e-6] = 255  # nothing there: white paper
    Image.fromarray(rgb).save(OUT / f"{name}.png", optimize=True)


def main() -> None:
    parts = {"head_housing": head_housing(), "camera_bracket": camera_bracket(), "wrist_adapter": wrist_adapter()}
    asm = OUT / "_asm"
    asm.mkdir(parents=True, exist_ok=True)
    grey = (0.78, 0.78, 0.78)
    for name, part in parts.items():
        export(name, part)
        cq.exporters.export(part, str(asm / f"{name}.stl"), tolerance=0.05, angularTolerance=0.2)
        preview(name, [(str(asm / f"{name}.stl"), grey)])
    # the three parts together with a block where the camera goes, in the robot frame
    cam = cq.Workplane("XY").box(CAM.d, CAM.w, CAM.h, centered=(True, True, False)).translate((DEPTH / 2, 0, HEIGHT + 3.0))
    cq.exporters.export(cam, str(asm / "camera.stl"))
    preview(
        "head_assembly",
        [(str(asm / "head_housing.stl"), grey), (str(asm / "camera_bracket.stl"), (0.70, 0.70, 0.70)),
         (str(asm / "wrist_adapter.stl"), (0.62, 0.62, 0.62)), (str(asm / "camera.stl"), (0.25, 0.25, 0.27))],
    )
    for p in asm.iterdir():
        p.unlink()
    asm.rmdir()
    for p in sorted(OUT.iterdir()):
        print(f"{p.name:24s} {p.stat().st_size / 1024:7.0f} KB")
    print(f"housing {WIDTH:.1f} x {HEIGHT:.1f} x {DEPTH:.1f} mm, plate {PLATE_T:.0f} mm, head total {DEPTH + PLATE_T:.1f} mm deep")


if __name__ == "__main__":
    sys.exit(main())
