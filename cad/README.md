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
