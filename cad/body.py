"""Pincer1's body, as code: the two plates of the 21 cm chassis, the front bezel, the battery tray
and the three brackets that make a 110 / 160 mm leg out of three bus servos.

Millimetres, robot frame: x forward, y left, z up, z = 0 on the top face of the top plate. The six
coxa axes come from `robot/pincer_robot/body/geometry.py`, so a change there moves the holes here.

    top_plate        210 x 150 x 3: hub holes for the six coxa brackets, arm base, pad, standoffs
    bottom_plate     same outline: the coxa servos stand on it, the compute board hangs under the top
    front_bezel      window for the 4.3" panel, hangs on the front standoffs
    battery_tray     flat 3S4P pack in one layer under the bottom plate
    coxa_bracket     hub on the coxa horn, through the top plate, wall that carries the femur servo
    femur_link       u-channel over the femur servo, 110 mm to the knee servo it carries
    tibia_link       u-channel over the knee servo, 160 mm shank to the foot cap

Every dimension that depends on a bought part sits in the dataclasses below; measure, edit, rerun.
`python cad/body.py` writes STL (print orientation), STEP (robot frame), DXF (flat parts) and PNG
previews to cad/out/, and one assembled view with the head from head.py on its arm.
"""

from __future__ import annotations

import math
import sys
from dataclasses import dataclass
from pathlib import Path

import cadquery as cq

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "robot"))

from head import HORN, _seal, camera_bracket, head_housing, preview, wrist_adapter  # noqa: E402
from head import DEPTH as HEAD_DEPTH  # noqa: E402
from head import HEIGHT as HEAD_HEIGHT  # noqa: E402
from head import OUT  # noqa: E402

try:
    from pincer_robot.body.geometry import BodyGeometry  # noqa: E402

    _G = BodyGeometry()
    MOUNTS = [(mx * 1000, my * 1000, yaw) for mx, my, yaw in _G.mounts]
    COXA, FEMUR, TIBIA = _G.coxa_m * 1000, _G.femur_m * 1000, _G.tibia_m * 1000
except ImportError:  # the robot package is not importable: the same numbers, by hand
    MOUNTS = [(83, -51, -math.pi / 4), (0, -74, -math.pi / 2), (-83, -51, -3 * math.pi / 4),
              (-83, 51, 3 * math.pi / 4), (0, 74, math.pi / 2), (83, 51, math.pi / 4)]
    COXA, FEMUR, TIBIA = 45.0, 110.0, 160.0


@dataclass(frozen=True)
class Servo:
    """STS3215 class bus servo. Horn on the top face, idler pin on the bottom face, both on the
    shaft axis; the shaft sits off centre along the length. Verify every number on a real servo."""

    length: float = 45.2  # along the body, shaft axis across it
    width: float = 24.7
    height: float = 35.0  # bottom (mount) face to the top (horn) face
    shaft_offset: float = 11.5  # shaft axis to the near end, along the length
    horn_h: float = 4.0  # horn stack above the top face
    horn_d: float = 20.0
    idler_d: float = 6.0  # pin on the bottom face
    mount_pitch_l: float = 38.0  # 4 holes on the bottom face, centred on the body
    mount_pitch_w: float = 16.0
    mount_screw_d: float = 2.7  # m2.5 clearance
    clear: float = 0.6

    @property
    def shaft_from_centre(self) -> float:
        return self.length / 2 - self.shaft_offset


@dataclass(frozen=True)
class Body:
    length: float = 210.0  # x, `BODY_L` in sim/build.py and the 21 cm body in docs/BODY.md
    width: float = 150.0  # y
    plate_t: float = 3.0
    gap: float = 45.0  # between the plates, inner faces
    corner_r: float = 10.0
    standoff_d: float = 3.4  # m3 clearance for the standoffs
    hub_hole_d: float = 30.0  # top plate, clearance for the coxa bracket neck
    coxa_axis_z: float = 16.6  # femur axis above the top plate (geometry.py)
    arm_base: tuple[float, float] = (60.0, 0.0)  # sim/build.py ARM_BASE
    arm_pattern: float = 30.0  # 4 x m3 on this square
    arm_cable_d: float = 12.0
    pad_pos: tuple[float, float] = (-20.0, 0.0)  # sim/build.py PAD_POS, under the plate behind the arm
    pad_cut: tuple[float, float] = (14.0, 12.0)  # aluminium: a hole the module senses through
    pad_skin: float = 1.0  # printed plate: this much plastic left over the pad instead of a hole
    printed: bool = False  # False = aluminium plates (pad hole), True = printed plates (pad skin)
    compute_pos: tuple[float, float] = (-40.0, 0.0)  # compute module carrier centre, on the bottom plate
    compute_pattern: tuple[float, float] = (86.0, 58.0)  # 4 x m3
    cable_slot: tuple[float, float] = (30.0, 10.0)  # lead pass-throughs


@dataclass(frozen=True)
class Display:
    """4.3" HDMI panel in the front bezel: outline, active area, board behind it."""

    outline_w: float = 121.0
    outline_h: float = 78.0
    active_w: float = 95.0
    active_h: float = 54.0
    panel_t: float = 4.0  # glass and board behind the bezel face
    bezel_t: float = 3.0
    margin: float = 6.0  # bezel beyond the panel outline
    screw_d: float = 3.4


@dataclass(frozen=True)
class Battery:
    """3S4P of 21700 cells laid flat in one layer, 3 long by 4 wide: a slab, not a brick, so it goes
    under the bottom plate and the body keeps its 45 mm plate gap and a low centre of mass."""

    length: float = 212.0  # 3 x 70 mm cells plus wrap
    width: float = 86.0  # 4 x 21.2 mm plus wrap
    height: float = 23.0
    wall: float = 3.0
    clear: float = 1.0
    screw_d: float = 4.4  # m4 to the bottom plate
    strap_slot: tuple[float, float] = (26.0, 4.0)  # velcro strap through the floor


@dataclass(frozen=True)
class Bracket:
    """Leg brackets: 2 mm aluminium bent, or 4 mm printed; wall picks the material."""

    wall: float = 2.0
    plate_w: float = 30.0  # side plate height across the servo width
    end_r: float = 15.0
    fillet: float = 2.0
    pin_d: float = 6.4  # idler pin clearance in the opposite plate
    knee_offset_deg: float = 0.0  # knee horn angle off the femur line; feeds Calibration, not the geometry
    foot_d: float = 14.0  # boss the rubber foot cap slips over
    foot_h: float = 12.0
    shank_w: float = 16.0
    shank_h: float = 20.0


SV, BODY, DISP, BAT, BR = Servo(), Body(), Display(), Battery(), Bracket()

# derived z levels (robot frame): top plate top face at 0
Z_TOP_PLATE = (-BODY.plate_t, 0.0)
Z_BOTTOM_PLATE = (-BODY.plate_t - BODY.gap - BODY.plate_t, -BODY.plate_t - BODY.gap)
Z_COXA_SERVO = (Z_BOTTOM_PLATE[1], Z_BOTTOM_PLATE[1] + SV.height)  # stands on the bottom plate
Z_COXA_HORN_TOP = Z_COXA_SERVO[1] + SV.horn_h
# the femur servo lies along the leg, shaft across it, bottom face against the coxa bracket's wall
LAT_WALL = (SV.height / 2, SV.height / 2 + BR.wall)  # lateral band of that wall, + side of the leg plane
LAT_PLATE1 = (-SV.height / 2 - SV.horn_h - 1.0 - BR.wall, -SV.height / 2 - SV.horn_h - 1.0)  # horn side
LAT_PLATE2 = (LAT_WALL[1], LAT_WALL[1] + BR.wall)  # idler side, beyond the wall
STANDOFFS = [(sx * (BODY.length / 2 - 8), sy * (BODY.width / 2 - 8)) for sx in (-1, 1) for sy in (-1, 1)] + [
    (0.0, sy * (BODY.width / 2 - 8)) for sy in (-1, 1)] + [(sx * 50.0, 0.0) for sx in (-1, 1)]


def plate_outline() -> cq.Workplane:
    return cq.Workplane("XY").rect(BODY.length, BODY.width).extrude(BODY.plate_t).edges("|Z").fillet(BODY.corner_r)


def holes(part: cq.Workplane, points: list[tuple[float, float]], d: float, z0: float, depth: float) -> cq.Workplane:
    for x, y in points:
        part = part.cut(cq.Workplane("XY").workplane(offset=z0 - 0.01).center(x, y).circle(d / 2).extrude(depth + 0.02))
    return part


def pattern(cx: float, cy: float, yaw: float, pitch_l: float, pitch_w: float, along: float = 0.0) -> list[tuple[float, float]]:
    """Four holes around a centre, the long pitch along `yaw`; `along` shifts the centre along it."""
    c, s = math.cos(yaw), math.sin(yaw)
    out = []
    for a in (-1, 1):
        for b in (-1, 1):
            u, v = along + a * pitch_l / 2, b * pitch_w / 2
            out.append((cx + u * c - v * s, cy + u * s + v * c))
    return out


def top_plate() -> cq.Workplane:
    """Hub holes for the coxa brackets, the arm base, the pad, standoffs, two cable slots."""
    p = plate_outline().translate((0, 0, Z_TOP_PLATE[0]))
    z0, t = Z_TOP_PLATE[0], BODY.plate_t
    p = holes(p, [(mx, my) for mx, my, _ in MOUNTS], BODY.hub_hole_d, z0, t)
    p = holes(p, STANDOFFS, BODY.standoff_d, z0, t)
    ax, ay = BODY.arm_base
    p = holes(p, [(ax + sx * BODY.arm_pattern / 2, ay + sy * BODY.arm_pattern / 2) for sx in (-1, 1) for sy in (-1, 1)], 3.4, z0, t)
    p = holes(p, [(ax - BODY.arm_pattern / 2 - 10, ay)], BODY.arm_cable_d, z0, t)
    px, py = BODY.pad_pos
    if BODY.printed:
        pocket = cq.Workplane("XY").workplane(offset=z0 - 0.01).center(px, py).rect(*BODY.pad_cut).extrude(t - BODY.pad_skin + 0.01)
    else:
        pocket = cq.Workplane("XY").workplane(offset=z0 - 0.01).center(px, py).rect(*BODY.pad_cut).extrude(t + 0.02)
    p = p.cut(pocket)
    # the consent module's board screws, next to the pad, and two lead slots towards the sides
    p = holes(p, [(px - 12, py + 20), (px - 12, py - 20)], 2.7, z0, t)
    for sy in (-1, 1):
        p = p.cut(cq.Workplane("XY").workplane(offset=z0 - 0.01).center(-60, sy * 35).rect(*BODY.cable_slot).extrude(t + 0.02))
    return p


def bottom_plate() -> cq.Workplane:
    """The six coxa servos bolt down by their bottom faces, idler pins through; the compute module
    on standoffs; the battery tray below; vents between the mounts."""
    p = plate_outline().translate((0, 0, Z_BOTTOM_PLATE[0]))
    z0, t = Z_BOTTOM_PLATE[0], BODY.plate_t
    for mx, my, yaw in MOUNTS:
        # body centre sits inboard of the shaft along the mount direction, so the servo stays under the plate
        p = holes(p, pattern(mx, my, yaw, SV.mount_pitch_l, SV.mount_pitch_w, along=-SV.shaft_from_centre), SV.mount_screw_d, z0, t)
        p = holes(p, [(mx, my)], SV.idler_d + 1.0, z0, t)
    p = holes(p, STANDOFFS, BODY.standoff_d, z0, t)
    cx, cy = BODY.compute_pos
    pw, ph = BODY.compute_pattern
    p = holes(p, [(cx + sx * pw / 2, cy + sy * ph / 2) for sx in (-1, 1) for sy in (-1, 1)], 3.4, z0, t)
    # battery tray screws and its lead hole
    p = holes(p, tray_screws(), BAT.screw_d, z0, t)
    p = holes(p, [(BAT.length / 2 - 25, 0.0)], 14.0, z0, t)
    # vents along the middle, clear of the compute pattern
    for i in range(-3, 4):
        p = p.cut(cq.Workplane("XY").workplane(offset=z0 - 0.01).center(45 + 0 * i, i * 12.0).rect(28.0, 5.0).extrude(t + 0.02))
    return p


def tray_screws() -> list[tuple[float, float]]:
    return [(sx * (BAT.length / 2 - 12), sy * (BAT.width / 2 + BAT.wall + 5)) for sx in (-1, 1) for sy in (-1, 1)]


def battery_tray() -> cq.Workplane:
    """Open tray under the bottom plate, flanges either side with the four screws, strap slots in the
    floor, a lead slot at the front end."""
    iw, il = BAT.width + 2 * BAT.clear, BAT.length + 2 * BAT.clear
    ow, ol = iw + 2 * BAT.wall, il + 2 * BAT.wall
    h = BAT.height + BAT.wall + 1.0
    zt = Z_BOTTOM_PLATE[0]  # tray top meets the bottom plate's underside
    tray = cq.Workplane("XY").box(ol, ow, h, centered=(True, True, False)).translate((0, 0, zt - h)).edges("|Z").fillet(4.0)
    cavity = cq.Workplane("XY").box(il, iw, h, centered=(True, True, False)).translate((0, 0, zt - h + BAT.wall))
    tray = tray.cut(cavity)
    # side flanges at the top with the screw holes
    for sy in (-1, 1):
        fl = cq.Workplane("XY").box(ol, 14.0, BAT.wall, centered=(True, True, False)).translate((0, sy * (ow / 2 + 7.0 - 1.0), zt - BAT.wall))
        tray = tray.union(fl)
    tray = holes(tray, tray_screws(), BAT.screw_d, zt - BAT.wall, BAT.wall)
    # strap slots through the floor, two straps
    for sx in (-1, 1):
        for sy in (-1, 1):
            tray = tray.cut(cq.Workplane("XY").workplane(offset=zt - h - 0.01).center(sx * il / 4, sy * (iw / 2 - 10)).rect(*BAT.strap_slot).extrude(BAT.wall + 0.02))
    # leads leave through the end wall at the top, under the plate's lead hole
    tray = tray.cut(cq.Workplane("XY").box(BAT.wall + 0.02, 14.0, 10.0, centered=(True, True, False)).translate((ol / 2 - BAT.wall / 2, 0, zt - 10.0)))
    return tray


def front_bezel() -> cq.Workplane:
    """Plate in front of the body with the display window; the panel sits in a pocket behind the
    face and the whole thing hangs on the two front standoffs, overhanging the plates top and bottom."""
    w = DISP.outline_w + 2 * DISP.margin
    h = DISP.outline_h + 2 * DISP.margin
    t = DISP.bezel_t + DISP.panel_t
    zc = (Z_TOP_PLATE[1] + Z_BOTTOM_PLATE[0]) / 2  # centred on the body's height
    x0 = BODY.length / 2  # back face of the bezel against the plates' front edges
    bezel = cq.Workplane("YZ").workplane(offset=x0).center(0, zc).rect(w, h).extrude(t).edges("|X").fillet(6.0)
    window = cq.Workplane("YZ").workplane(offset=x0 - 0.01).center(0, zc).rect(DISP.active_w, DISP.active_h).extrude(t + 0.02)
    pocket = cq.Workplane("YZ").workplane(offset=x0 - 0.01).center(0, zc).rect(DISP.outline_w + 1.0, DISP.outline_h + 1.0).extrude(DISP.panel_t + 0.01)
    bezel = bezel.cut(window).cut(pocket)
    # tabs that reach back along the plates' front edge, one screw into each front standoff
    for sy in (-1, 1):
        y = sy * (BODY.width / 2 - 8)
        tab = cq.Workplane("XY").box(16.0, 12.0, BODY.gap, centered=(False, True, False)).translate((x0 - 16.0, y, Z_BOTTOM_PLATE[1]))
        tab = tab.cut(cq.Workplane("XY").workplane(offset=Z_BOTTOM_PLATE[1] - 0.01).center(x0 - 8.0, y).circle(BODY.standoff_d / 2).extrude(BODY.gap + 0.02))
        bezel = bezel.union(tab)
    # a slot for the panel's hdmi and usb leads, bottom edge of the pocket
    bezel = bezel.cut(cq.Workplane("YZ").workplane(offset=x0 - 0.01).center(0, zc - DISP.outline_h / 2 - 2).rect(40.0, 8.0).extrude(DISP.panel_t + 0.01))
    return bezel


# ----------------------------------------------------------------------------------------------
# leg brackets, each in its own leg frame: origin on the joint axis it is driven by, x outward along
# the leg, y lateral (+ towards the idler side), z up. `place_leg` puts them on the body.


def coxa_bracket() -> cq.Workplane:
    """Hub on the coxa horn, neck through the top plate, a foot plate above it and a wall standing in
    the leg plane that the femur servo bolts to by its bottom face; its shaft axis lands at
    (COXA, 0, coxa_axis_z). Origin here is the coxa axis at the top plate's top face."""
    neck_d = BODY.hub_hole_d - 4.0
    hub = cq.Workplane("XY").workplane(offset=Z_COXA_HORN_TOP).circle(HORN.bolt_circle_d / 2 + 6.0).extrude(3.0)
    neck = cq.Workplane("XY").workplane(offset=Z_COXA_HORN_TOP + 3.0).circle(neck_d / 2).extrude(-(Z_COXA_HORN_TOP + 3.0) + 2.0)
    foot_x0, foot_x1 = -neck_d / 2 - 2.0, COXA + SV.shaft_offset + 6.0  # hub to just past the femur servo's far end
    foot = (
        cq.Workplane("XY").workplane(offset=2.0)
        .center((foot_x0 + foot_x1) / 2, (LAT_WALL[1] + LAT_PLATE1[1]) / 2)
        .rect(foot_x1 - foot_x0, LAT_WALL[1] - LAT_PLATE1[1]).extrude(BR.wall + 1.0)
        .edges("|Z").fillet(4.0)
    )
    part = hub.union(neck).union(foot)
    # the wall: from the foot plate up past the servo's mounting holes
    wall_top = BODY.coxa_axis_z + SV.width / 2 + 4.0
    wall_x0 = COXA - SV.shaft_from_centre - SV.length / 2 - 2.0
    wall_x1 = COXA + SV.shaft_offset + 4.0
    wall = (
        cq.Workplane("XY").box(wall_x1 - wall_x0, BR.wall, wall_top - 2.0, centered=(False, True, False))
        .translate((wall_x0, (LAT_WALL[0] + LAT_WALL[1]) / 2, 2.0))
    )
    part = part.union(wall)
    # gusset behind the wall on the idler side
    gusset = (
        cq.Workplane("XZ").polyline([(wall_x0, 2.0 + BR.wall), (wall_x0, wall_top - 6.0), (wall_x0 + 22.0, 2.0 + BR.wall)]).close()
        .extrude(-6.0).translate((0, LAT_WALL[1], 0))
    )
    part = part.union(gusset)
    # femur servo: 4 mounting holes and the idler pin through the wall
    cx = COXA - SV.shaft_from_centre
    for a in (-1, 1):
        for b in (-1, 1):
            part = part.cut(cq.Workplane("XZ").workplane(offset=-LAT_WALL[1] - 0.01).center(cx + a * SV.mount_pitch_l / 2, BODY.coxa_axis_z + b * SV.mount_pitch_w / 2).circle(SV.mount_screw_d / 2).extrude(BR.wall + 0.02))
    part = part.cut(cq.Workplane("XZ").workplane(offset=-LAT_WALL[1] - 0.01).center(COXA, BODY.coxa_axis_z).circle(BR.pin_d / 2).extrude(BR.wall + 0.02))
    # coxa horn: boss pocket under the hub and the screw pattern through it
    part = part.cut(cq.Workplane("XY").workplane(offset=Z_COXA_HORN_TOP - 0.01).circle(HORN.boss_d / 2 + 0.2).extrude(HORN.boss_depth + 0.01))
    for i in range(HORN.screws):
        a = 2 * math.pi * i / HORN.screws + math.pi / HORN.screws
        part = part.cut(cq.Workplane("XY").workplane(offset=Z_COXA_HORN_TOP - 0.01).center(HORN.bolt_circle_d / 2 * math.cos(a), HORN.bolt_circle_d / 2 * math.sin(a)).circle(HORN.screw_d / 2).extrude(3.02))
    return part


def side_plate(x0: float, x1: float, y_band: tuple[float, float], z_half: float = BR.plate_w / 2) -> cq.Workplane:
    """A flat plate in the leg plane from x0 to x1 with rounded ends, at the lateral band given."""
    length = x1 - x0
    pl = (
        cq.Workplane("XZ").center((x0 + x1) / 2, 0).rect(length, 2 * z_half).extrude(-(y_band[1] - y_band[0]))
        .edges("|Y").fillet(min(BR.end_r, z_half - 0.1))
        .translate((0, y_band[0], 0))
    )
    return pl


def horn_cut(part: cq.Workplane, x: float, y_band: tuple[float, float], angle: float = 0.0) -> cq.Workplane:
    """The round horn's boss pocket and screw pattern through a side plate at axis x."""
    y0 = y_band[0]
    part = part.cut(cq.Workplane("XZ").workplane(offset=-y0 - 0.01).center(x, 0).circle(HORN.boss_d / 2 + 0.2).extrude(-(HORN.boss_depth + 0.01)))
    for i in range(HORN.screws):
        a = 2 * math.pi * i / HORN.screws + math.pi / HORN.screws + angle
        part = part.cut(cq.Workplane("XZ").workplane(offset=-y0 - 0.01).center(x + HORN.bolt_circle_d / 2 * math.cos(a), HORN.bolt_circle_d / 2 * math.sin(a)).circle(HORN.screw_d / 2).extrude(-(y_band[1] - y_band[0] + 0.02)))
    return part


def pin_cut(part: cq.Workplane, x: float, y_band: tuple[float, float]) -> cq.Workplane:
    return part.cut(cq.Workplane("XZ").workplane(offset=-y_band[0] - 0.01).center(x, 0).circle(BR.pin_d / 2).extrude(-(y_band[1] - y_band[0] + 0.02)))


def femur_link() -> cq.Workplane:
    """Plate 1 on the femur horn runs to the knee, where the knee servo bolts to it by its bottom face
    (idler pin through the plate); plate 2 on the femur idler is short; a web over the top joins them."""
    knee_body_c = FEMUR - SV.shaft_from_centre
    p1 = side_plate(-BR.end_r, knee_body_c + SV.length / 2 + 4.0, LAT_PLATE1)
    p2 = side_plate(-BR.end_r, 32.0, LAT_PLATE2)
    web_z = BR.plate_w / 2
    web = (
        cq.Workplane("XY").box(32.0 + BR.end_r - 6.0, LAT_PLATE2[1] - LAT_PLATE1[0], BR.wall, centered=(False, False, False))
        .translate((-BR.end_r + 3.0, LAT_PLATE1[0], web_z - BR.wall))
    )
    part = p1.union(p2).union(web)
    part = horn_cut(part, 0.0, LAT_PLATE1)
    part = pin_cut(part, 0.0, LAT_PLATE2)
    # knee servo on plate 1: four mounting screws, the idler pin through
    for a in (-1, 1):
        for b in (-1, 1):
            part = part.cut(cq.Workplane("XZ").workplane(offset=-LAT_PLATE1[0] - 0.01).center(knee_body_c + a * SV.mount_pitch_l / 2, b * SV.mount_pitch_w / 2).circle(SV.mount_screw_d / 2).extrude(-(BR.wall + 0.02)))
    part = pin_cut(part, FEMUR, LAT_PLATE1)
    # lightening slots along the long plate
    for x in (45.0, 70.0):
        part = part.cut(cq.Workplane("XZ").workplane(offset=-LAT_PLATE1[0] - 0.01).center(x, 0).slot2D(16.0, 6.0).extrude(-(BR.wall + 0.02)))
    return part


def tibia_link() -> cq.Workplane:
    """U over the knee servo (plate 1 on the horn, plate 2 on the idler), then a shank 160 mm to the
    foot, ending in a boss for a rubber cap. Built along +x; `place_leg` turns it to hang down."""
    # knee servo horn is on the + lateral side of the femur's plate 1, its idler pin comes out the - side
    horn_band = (LAT_PLATE1[1] + SV.height + SV.horn_h + 1.0, LAT_PLATE1[1] + SV.height + SV.horn_h + 1.0 + BR.wall)
    idler_band = (LAT_PLATE1[0] - BR.wall, LAT_PLATE1[0])
    p1 = side_plate(-BR.end_r, 36.0, horn_band)
    p2 = side_plate(-BR.end_r, 36.0, idler_band)
    web_z = BR.plate_w / 2
    web = (
        cq.Workplane("XY").box(36.0 + BR.end_r - 6.0, horn_band[1] - idler_band[0], BR.wall, centered=(False, False, False))
        .translate((-BR.end_r + 3.0, idler_band[0], web_z - BR.wall))
    )
    part = p1.union(p2).union(web)
    part = horn_cut(part, 0.0, horn_band, math.radians(BR.knee_offset_deg))
    part = pin_cut(part, 0.0, idler_band)
    # shank: a bar from the web down to the foot, centred between the two plates
    yc = (horn_band[1] + idler_band[0]) / 2
    shank = (
        cq.Workplane("XY").box(TIBIA - 30.0 - BR.foot_h, BR.shank_w, BR.shank_h, centered=(False, True, True))
        .translate((30.0, yc, web_z - BR.shank_h / 2))
        .edges("|X").fillet(3.0)
    )
    # taper web: a wedge from the full u width down to the shank
    wedge = (
        cq.Workplane("XY").polyline([(26.0, idler_band[0]), (26.0, horn_band[1]), (46.0, yc + BR.shank_w / 2), (46.0, yc - BR.shank_w / 2)]).close()
        .extrude(BR.wall).translate((0, 0, web_z - BR.wall))
    )
    part = part.union(shank).union(wedge)
    foot = cq.Workplane("YZ").workplane(offset=TIBIA - BR.foot_h).center(yc, web_z - BR.shank_h / 2).circle(BR.foot_d / 2).extrude(BR.foot_h)
    part = part.union(foot)
    return part


# ----------------------------------------------------------------------------------------------
# placement on the body for the assembly view


def place_leg(part: cq.Workplane, leg: int, joint: str) -> cq.Workplane:
    """Leg-frame part -> robot frame for the neutral stance: coxa along its mount, femur level,
    tibia straight down."""
    mx, my, yaw = MOUNTS[leg]
    if joint == "femur":
        part = part.translate((COXA, 0, BODY.coxa_axis_z))
    elif joint == "tibia":
        part = part.rotate((0, 0, 0), (0, 1, 0), 90).translate((COXA + FEMUR, 0, BODY.coxa_axis_z))
    return part.rotate((0, 0, 0), (0, 0, 1), math.degrees(yaw)).translate((mx, my, 0))


def servo_box(cx: float, cy: float, cz: float, length_axis: str) -> cq.Workplane:
    """A servo body as a block, for the pictures only."""
    dims = {"x": (SV.length, SV.width, SV.height), "y": (SV.width, SV.length, SV.height), "z": (SV.width, SV.height, SV.length)}[length_axis]
    return cq.Workplane("XY").box(*dims).translate((cx, cy, cz))


def arm_and_head() -> list[tuple[str, cq.Workplane]]:
    """The arm upright at its base with the head parts on the wrist, from head.py."""
    ax, ay = BODY.arm_base
    segs = (43.0, 80.0, 80.0)  # sim/build.py ARM_SEG, metres there
    z = 0.0
    parts = []
    for i, L in enumerate(segs):
        parts.append((f"arm{i}", cq.Workplane("XY").workplane(offset=z).center(ax, ay).circle(12.0).extrude(L)))
        z += L
    # head frames: housing back face at x=0 in head.py; stand it on the wrist, display facing forward
    for name, part in (("head_housing", head_housing()), ("camera_bracket", camera_bracket()), ("wrist_adapter", wrist_adapter())):
        parts.append((name, part.translate((ax - HEAD_DEPTH / 2, ay, z))))
    return parts


# ----------------------------------------------------------------------------------------------
# export

PRINT_ORIENTATION = {
    "top_plate": None,
    "bottom_plate": None,
    "front_bezel": ((0, 0, 0), (0, 1, 0), -90),  # face down
    "battery_tray": ((0, 0, 0), (1, 0, 0), 180),  # open side up
    "coxa_bracket": ((0, 0, 0), (1, 0, 0), 180),  # hub up, foot plate on the bed: supports under the hub only
    "femur_link": ((0, 0, 0), (1, 0, 0), 180),  # web on the bed, plates up
    "tibia_link": ((0, 0, 0), (1, 0, 0), 180),
}
FLAT = {"top_plate", "bottom_plate", "front_bezel"}


def export(name: str, part: cq.Workplane) -> None:
    OUT.mkdir(exist_ok=True)
    rot = PRINT_ORIENTATION.get(name)
    printed = part.rotate(*rot) if rot else part
    bb = printed.val().BoundingBox()
    printed = printed.translate((-bb.xmin, -bb.ymin, -bb.zmin))
    stl = OUT / f"{name}.stl"
    cq.exporters.export(printed, str(stl), tolerance=0.08, angularTolerance=0.25)
    cq.exporters.export(part, str(OUT / f"{name}.step"))
    _seal(stl)


def export_dxf(name: str, part: cq.Workplane, plane: str, offset: float) -> None:
    """A cut profile for the laser or the water jet: the section through the plate's middle."""
    sec = part.section(offset) if plane == "XY" else part.rotate((0, 0, 0), (0, 1, 0), 90).section(offset)
    cq.exporters.export(sec, str(OUT / f"{name}.dxf"))


def main() -> None:
    parts = {
        "top_plate": top_plate(), "bottom_plate": bottom_plate(), "front_bezel": front_bezel(),
        "battery_tray": battery_tray(), "coxa_bracket": coxa_bracket(), "femur_link": femur_link(), "tibia_link": tibia_link(),
    }
    asm = OUT / "_asm"
    asm.mkdir(parents=True, exist_ok=True)
    grey, dark, mid = (0.78, 0.78, 0.78), (0.25, 0.25, 0.27), (0.62, 0.62, 0.62)
    for name, part in parts.items():
        export(name, part)
        cq.exporters.export(part, str(asm / f"{name}.stl"), tolerance=0.08, angularTolerance=0.25)
        preview(name, [(str(asm / f"{name}.stl"), grey)], azimuth=150 if name != "front_bezel" else 30)
    export_dxf("top_plate", parts["top_plate"], "XY", Z_TOP_PLATE[0] + BODY.plate_t / 2)
    export_dxf("bottom_plate", parts["bottom_plate"], "XY", Z_BOTTOM_PLATE[0] + BODY.plate_t / 2)
    # bracket side plates as flat profiles, for sheet: plate 1 of each link
    for name, band in (("femur_link", LAT_PLATE1), ("tibia_link", (LAT_PLATE1[0] - BR.wall, LAT_PLATE1[0]))):
        sec = parts[name].rotate((0, 0, 0), (1, 0, 0), 90).section(-(band[0] + band[1]) / 2)
        cq.exporters.export(sec, str(OUT / f"{name}_plate.dxf"))
    # assembly: plates, standoffs, coxa servos, six legs with their servos, tray, bezel, arm and head
    meshes: list[tuple[str, tuple[float, float, float]]] = []

    def add(name: str, part: cq.Workplane, rgb: tuple[float, float, float]) -> None:
        path = asm / f"a_{name}.stl"
        cq.exporters.export(part, str(path), tolerance=0.15, angularTolerance=0.4)
        meshes.append((str(path), rgb))

    add("top", parts["top_plate"], grey)
    add("bottom", parts["bottom_plate"], grey)
    add("bezel", parts["front_bezel"], mid)
    add("tray", parts["battery_tray"], mid)
    for i, (x, y) in enumerate(STANDOFFS):
        add(f"so{i}", cq.Workplane("XY").workplane(offset=Z_BOTTOM_PLATE[1]).center(x, y).circle(2.5).extrude(BODY.gap), mid)
    for leg, (mx, my, yaw) in enumerate(MOUNTS):
        c, s = math.cos(yaw), math.sin(yaw)
        off = -SV.shaft_from_centre
        add(f"cs{leg}", servo_box(0, 0, 0, "x").translate((off, 0, 0)).rotate((0, 0, 0), (0, 0, 1), math.degrees(yaw)).translate((mx, my, (Z_COXA_SERVO[0] + Z_COXA_SERVO[1]) / 2)), dark)
        add(f"cb{leg}", place_leg(coxa_bracket(), leg, "coxa"), grey)
        # femur servo: along the leg, bottom face on the wall, centred laterally at the leg plane
        fs = cq.Workplane("XY").box(SV.length, SV.height, SV.width).translate((COXA - SV.shaft_from_centre, 0, BODY.coxa_axis_z))
        add(f"fs{leg}", place_leg(fs, leg, "coxa"), dark)
        add(f"fl{leg}", place_leg(parts["femur_link"], leg, "femur"), grey)
        ks = cq.Workplane("XY").box(SV.length, SV.height, SV.width).translate((FEMUR - SV.shaft_from_centre, LAT_PLATE1[1] + SV.height / 2, 0))
        add(f"ks{leg}", place_leg(ks, leg, "femur"), dark)
        add(f"tl{leg}", place_leg(parts["tibia_link"], leg, "tibia"), grey)
    for name, part in arm_and_head():
        add(name, part, mid if name.startswith("arm") else grey)
    preview("body_assembly", meshes, azimuth=140, elevation=-20)
    for p in asm.iterdir():
        p.unlink()
    asm.rmdir()
    for p in sorted(OUT.iterdir()):
        print(f"{p.name:26s} {p.stat().st_size / 1024:7.0f} KB")
    print(f"plates {BODY.length:.0f} x {BODY.width:.0f} x {BODY.plate_t:.0f}, gap {BODY.gap:.0f}; tray {BAT.length + 2 * (BAT.wall + BAT.clear):.0f} x {BAT.width + 2 * (BAT.wall + BAT.clear):.0f} x {BAT.height + BAT.wall + 1:.0f} under the bottom plate")


if __name__ == "__main__":
    sys.exit(main())
