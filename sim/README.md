# sim

Pincer1 as a robot description: six legs of coxa / femur / tibia, the four-axis arm with the camera head
on its wrist, the consent pad and the front display. 22 revolute joints, the same names the robot
package uses, the same frames as `robot/pincer_robot/body/geometry.py` (body x forward, y left, z up,
origin at the top plate under the arm base).

```
pincer1.urdf   the description; plain cylinders and boxes, no meshes to fetch
pincer1.xml    the same robot as MuJoCo MJCF
pincer1.png    what MuJoCo shows after the stance check
build.py       writes the two files from geometry.py; --check loads the URDF in MuJoCo and stands it up
```

Joint zero is a straight leg pointing out along its mount; the tibia joint is the knee angle minus π, so
the robot package's interior knee angle maps onto it without a sign flip. Positive femur lifts the
knee, positive tibia lifts the foot. Arm joints are yaw, shoulder, elbow, wrist; `camera` is the optical
frame on the head's front face and `trusted_display` the window below it.

```
python sim/build.py --check
```

needs `mujoco` and `numpy` next to the robot package; it writes the URDF and MJCF, drops the body on
a floor in the neutral stance from the body's own IK and prints the height, tilt and foot contact.
