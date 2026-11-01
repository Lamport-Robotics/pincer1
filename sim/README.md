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
walk.py        walks the URDF in MuJoCo with the robot package's gait and records walk.gif
stairs.py      climbs box steps with the body package's stair cycle; --sweep tables the risers, --gif records stairs.gif
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

```
python sim/walk.py --gait tripod --seconds 6
```

drives the same `Body` controller the robot runs: the gait and leg IK produce the joint targets at
50 Hz, MuJoCo position servos hold them, physics does the rest. It prints the distance covered against
the commanded speed, the body height, the largest roll and pitch, and whether anything but a foot
touched the floor, then writes `walk.gif` (one palette, unchanged pixels left transparent, so it stays
small). `--gait ripple` and `wave` run the other two.

```
python sim/stairs.py --riser 0.12 --gif
python sim/stairs.py --sweep --steps 1
```

plays `pincer_robot.body.climb` over one or more box steps and reports whether the body got onto each
tread, the largest roll and pitch, the 99th percentile and peak of the leg joint torque against what a
servo can hold, anything but a foot touching the ground, and when it fell if it did. The gif plays the
climb three times faster than the cycle runs.
