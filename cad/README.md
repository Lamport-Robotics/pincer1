# cad

The head: what rides on the camera arm's wrist and faces the owner. Three printed parts, written as
code so a measured part changes a number, not a drawing. Frame as everywhere else in the repo: x
forward out of the lens, y left, z up; the camera is the highest point, the trusted display sits
behind its window below it, the consent pad is under the shell skin below the window.

```
head.py                 the parts and their parameters; python cad/head.py writes out/
out/head_housing.*      shell, open at the back: display window, standoffs for the OLED, pad pocket, cable slot
out/camera_bracket.*    cradle on the roof for the depth camera, one 1/4"-20 bolt from below
out/wrist_adapter.*     plate that closes the back and bolts to the arm's last servo horn
out/head_assembly.png   the three together with a block where the camera goes
```

STL is in print orientation, STEP in the robot frame for editing. Needs `cadquery`, `trimesh` and
`mujoco` (previews) next to the robot package's python.

Parameters to measure before printing, all at the top of `head.py`: the OLED module's PCB, hole
pitch and glass position (`Oled`), the touch module (`Pad`), the camera body and its socket
(`Camera`), and the servo horn's bolt circle, screw count and boss (`Horn`). The horn numbers are
placeholders until the arm is on the bench. The pad senses through 0.7 mm of plastic at the dimple;
if the module is slow to trigger, thin `skin_t` or raise its sensitivity capacitor.

Print: PETG, 0.2 mm layers, 3 walls, 25 % infill. Housing front face down and the adapter horn side
up, both without supports; the bracket flat. M3 heat-set inserts in the housing's four rear bosses and
the two roof bosses, M2.5 screws for the OLED, M3 countersunk screws through the adapter, M2 through
the horn.

## body

The 21 cm body `docs/BODY.md` describes, and the three brackets that make a 110 / 160 mm leg out of
three bus servos. `body.py` reads the six coxa axes from `robot/pincer_robot/body/geometry.py`, so the
plates and the kinematics cannot disagree.

```
body.py                 the parts and their parameters; python cad/body.py writes out/
out/top_plate.*         210 x 150 x 3: hub holes for the coxa brackets, arm base, pad, standoffs, lead slots
out/bottom_plate.*      the coxa servos bolt to it by their bottom faces; compute module and tray screws
out/front_bezel.*       window for the 4.3" panel, pocket behind it, tabs onto the front standoffs
out/battery_tray.*      open tray under the bottom plate for a flat 3S4P pack, strap slots, lead slot
out/coxa_bracket.*      hub on the coxa horn through the top plate, wall the femur servo bolts to
out/femur_link.*        u-channel over the femur servo, plate to the knee servo it carries
out/tibia_link.*        u-channel over the knee servo, shank to the foot boss
out/*.dxf               cut profiles: the two plates, and plate 1 of each link for sheet
out/body_assembly.png   everything in the neutral stance with the head on its arm
```

The coxa servos stand on the bottom plate, horn up; the coxa bracket's neck passes through a 30 mm hole
in the top plate, so the top plate carries no leg load and lifts off with its wiring. The femur servo
lies along the leg with its bottom face against the bracket's wall, shaft across the leg at 16.6 mm
above the top plate; the femur link bolts to its horn on one side and rides its idler pin on the
other, and carries the knee servo the same way at 110 mm. The tibia link does the same at the knee
and runs 160 mm to a boss a rubber foot cap slips over. The knee horn's angle off the femur line is
`Bracket.knee_offset_deg`; it goes into `Calibration`, never into the geometry.

The battery goes under the bottom plate: a 3S4P of 21700 cells laid flat in one layer is a 212 x 86 x 23
slab, which keeps the plate gap at 45 mm and the pack's weight below the servos. The tray adds 27 mm
under the plates; standing height leaves 47 mm of clearance under it. A brick-shaped pack would need
the gap raised to 80 mm (`Body.gap`) and the centre of mass with it.

Measure before cutting: the servo's body, shaft offset, horn stack and the bottom-face hole pattern
(`Servo`; the 38 x 16 pattern is a placeholder), the panel outline and board thickness (`Display`), the
pack you actually build (`Battery`), the compute carrier's holes (`Body.compute_pattern`), and the horn
pattern (`Horn` in head.py, shared). `Body.printed = True` leaves a 1 mm skin over the pad instead of
the hole an aluminium plate needs.

Material: plates 3 mm 5052 aluminium from the DXF, or 4 mm PETG; brackets 2 mm 5052 bent from the
flat profiles (the u-channels), or printed at 4 mm (`Bracket.wall`). Printed: plates flat, tray open side
up, links web down, coxa bracket foot plate down with supports under the hub. M3 standoffs, M2.5 into
the servos, M2 into the horns, M4 for the tray.
