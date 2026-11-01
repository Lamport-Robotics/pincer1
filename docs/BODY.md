# Body

The legs, in software. `robot/pincer_robot/body/`, no ROS, no hardware needed to run it.

```
geometry.py    where the six coxa axes sit, segment lengths, joint limits, neutral stance, servo ids
kinematics.py  3-DOF leg FK/IK in closed form; a body pose (xyz, roll/pitch/yaw) moves every foot target
gait.py        tripod / ripple / wave: phase per leg, stance slides, swing lifts on a half sine
servo.py       serial-bus servo protocol: ping, read, write, sync write; calibration (sign, zero)
sim.py         forward kinematics + support polygon: which feet are down, how much margin the centre has
controller.py  Body (velocity in, joint angles out, 50 Hz) and BodyBackend for the motion layer
gaze.py        the camera head on the arm: face position in, pan / tilt out, hold and scan when it is gone
```

Frames: body x forward, y left, z up; legs numbered clockwise from front right (FR MR RR RL ML FL).
Joint angles are radians: coxa yaw, femur pitch from horizontal, tibia interior knee angle.

## walking

```python
from pincer_robot.body import Body, Bus, BusActuator, BodyGeometry, GaitParams, SerialTransport

bus = Bus(SerialTransport("/dev/ttyUSB0"))
body = Body(gait=GaitParams("tripod"), actuator=BusActuator(bus, BodyGeometry()))
body.actuator.torque(True)
body.stand()
body.set_velocity(0.06, 0.0, 0.2)   # m/s forward, m/s left, rad/s
for _ in range(100):
    body.tick(0.02)
body.stop()
```

A command whose stride would exceed `step_length_m` is scaled down; `body.state.actual` says what the
body is really doing. Stopping freezes the phase: feet in the air finish their swing and land at the
neutral point, feet on the ground hold. `sit()` / `stand()` change the height.

The service picks the backend from `PINCER_BODY_BACKEND`: `twist` hands velocities to an external gait
controller on `cmd_vel_topic` (the demo chassis), `bus` drives the servos on `PINCER_SERVO_PORT` with
the gait above (the v1 body). `PINCER_GAIT` selects tripod, ripple or wave.

## without a body

```python
from pincer_robot.body import simulated_body
body, sim = simulated_body()
body.stand(); body.set_velocity(0.05)
body.tick(0.02); print(sim.report())   # feet down, support margin, swing clearance
```

The tests walk every gait through full cycles and require three feet down and a positive support
margin at every tick, round-trip IK/FK, and check the bus packets byte for byte.

## head

The camera rides the four-axis arm (servo ids 19 yaw, 20 shoulder, 21 elbow, 22 wrist) on the same bus.
`Gaze` takes the face centre in image coordinates (-1..1, right and down positive) and moves pan and
tilt toward it: a fraction of the error per frame, rate limited, inside the arm's limits. Lost face:
hold the bearing for two seconds, drift back to centre, then sweep slowly until someone is there.
`GazeHead` wires it to the bus and parks the shoulder and elbow upright so the camera sits as high as the
arm allows.

```python
from pincer_robot.body import Bus, Gaze, GazeConfig, GazeHead, SerialTransport

head = GazeHead(Bus(SerialTransport("/dev/ttyUSB0")), Gaze(GazeConfig(hfov_deg=62)))
head.raise_head()                          # arm up, camera level
head.observe(faces, ts)                    # every camera frame; faces may be empty
```

The service does this on the `bus` backend from the presence loop: the biggest face the recogniser
returns steers the head, so the robot is looking at the person it is about to talk to. The arm's pitch
joints turn about +y, so a positive wrist angle tips the camera down; `tilt` is camera elevation and is
negated on the way out, the same convention as `sim/pincer1.urdf`.

## calibration

`Calibration` holds a sign and a zero offset per servo; `to_ticks` clamps to the 12-bit range. The
geometry in `geometry.py` is what the kinematics see: straight segments, axis to axis. The brackets are
not straight, and the difference goes into the zero, not the maths.

The v1 legs are coxa 45 mm, femur 110 mm, tibia 160 mm axis to axis: long enough for a 17 cm stair
(see "stairs"). Mount positions come from the first test chassis: front and rear coxa axes at
(±93, ±51) mm from the plate centre on 45° mounts, middle at (0, ±74) mm on 90°, the coxa axis 16.6 mm
above the top plate. On that chassis (femur 77, tibia 116) the knee servo sits 22° off the femur line
and the foot tip 5° off the tibia line, so a femur reading of zero is not a horizontal femur; the v1
brackets have their own two angles. Either way they are the offsets of every femur and tibia servo
once the legs are measured, and `BodyGeometry(femur_m=0.077, tibia_m=0.116)` runs the short legs.

Procedure, body on a stand, feet in the air:

1. Torque off (`bus.torque(ids, False)`). Bring one leg to the neutral stance by hand: coxa along its
   mount, femur horizontal, tibia vertical, foot under the knee. A square against the plate is enough.
2. Read every joint (`bus.position` with an empty `Calibration` returns radians from 2048). Store
   `offset_ticks[id] = ticks - 2048` for each servo of that leg; repeat for the six legs.
3. Signs: command +0.2 rad on each joint in turn. Coxa must swing the foot toward the front of the body,
   femur must lift the knee, tibia must lift the foot. A joint that goes the other way gets `sign = -1`.
4. Check `JointLimits` against the brackets with torque off: move each joint to its limit by hand and
   read it; the limits in code are the inside of what the metal allows, never the metal itself.
5. `body.stand()` on the stand, watch the six feet meet one plane. Then `walk` in tripod, slowly, still
   on the stand, before the feet touch the floor.

The arm takes the same treatment with its own neutral: arm straight up (shoulder and elbow zero) and the
camera level (wrist zero). That pose is the `rest` in `GazeConfig`; the head's limits are the pan and tilt
ranges the shell window and the cable loop allow, and are narrower than the servos'.

## stairs

`climb.py` crawls over steps with keyframes generated from the same kinematics: the body advances
11 cm per cycle in three pulls, and between the pulls each pair of legs steps to where it is needed
next, its foot on whatever level is there and never within 3 cm of an edge. The front and rear feet
stand 14 cm from their coxa while climbing, well under the body, which is what keeps the rear legs
within reach when the body is a riser higher than they are. A foot in the air goes
out to 22 cm from its coxa while it lifts, because that is where the leg reaches highest, crosses the
riser there and comes back in as it lands; the middle feet land wide for the same reason. Body height
and pitch follow the feet (nose up to 30°). Every keyframe must solve inside the joint limits with
three feet down, the centre 3.5 cm inside their support polygon (the head on its arm moves the real
centre of mass when the body is pitched) and the underside of the body 1.5 cm clear of the step below
it, or the riser is refused with the phase that failed. On the landing the body comes level and down
to walking height first, then the feet walk out to the walking stance one at a time. `interpolate`
turns the keyframes into the 50 Hz samples the simulator and the robot play; a keyframe that moves
the body far takes longer (30°/s of pitch, 6 cm/s of height, 10 cm/s of travel), so the body is never
thrown from one pose to the next.

The v1 body is 21 cm long with the front and rear leg mounts 83 mm from its centre, and that length
comes from the stairs: on the 23 cm plate of the first test chassis the nose reached the next riser
before the rear feet were up on the current one, so runs of steps stopped at 13 cm whatever the legs
did. With the 110 / 160 legs on the 21 cm body the generator allows single steps and runs of steps up
to 17 cm at a 28 cm tread (18 cm, and 17 cm on a 25 cm tread, are refused). In MuJoCo (`sim/stairs.py`,
position servos at the walking gains) the body climbs three 17 cm steps in 94 s and stands level on
the landing within 3° of roll, nose up to 32° on the way; joint torque stays at 1.5 N m through the
climb against the 3 N m the servos have, with one brush of the underside on a step edge. Twelve and
thirteen centimetre runs climb the same way.

On the robot the cycle is `BodyBackend.climb`: it squares up to the riser the floor watch reported
(turn by its angle, walk to the cycle's standoff), then plays the keyframes at the gait tick. The
daemon asks for it with the `climb` message (`docs/PROTOCOL.md` §2), with or without naming the riser.

## the floor ahead

`robot/pincer_robot/terrain.py` is what the climb and the walk look at before moving. The depth
camera on the head, with the head's height and tilt, gives a height grid of the ground in front of the
body (5 cm cells, 1.2 m out, ±40 cm wide); each cell is floor, step, wall or drop, and floor the camera
should see but does not is a drop too. `find_step` reads the first riser out of the grid: distance,
height, the angle of its edge against the body (so the body can square up before `climb.py` starts),
and the tread depth when the next riser is in view. `front_clearance` is the distance to the nearest
wall or drop in the walking corridor, the number the motion layer refuses to walk into. `render_steps`
draws the same scenes synthetically for the tests and the mock robot.

## on the first chassis

The test chassis keeps its servos behind its own ROS 2 stack, which takes positions in pulses (0 to
1000 over 240°) and reports them back. `body/pulse.py` puts the body package on it without rclpy:
`RosbridgeServoBus` speaks to that stack over rosbridge's websocket with the same `move` / `position`
surface as the serial `Bus`, so the gait, the gaze and the climb cycle run unchanged, from the robot or
from a laptop on the same network. `PINCER_BODY_BACKEND=joint` and `PINCER_ROSBRIDGE_URL` select it;
the lidar and the IMU come over the same socket. Torque, temperature and load are the chassis
controller's business on that path, so the health poll is off.

The radians-to-pulses map is measured once:

```
pincer-calibrate --rosbridge ws://robot:9090 --show     # live pulses
pincer-calibrate --rosbridge ws://robot:9090            # jog each joint to its neutral, answer the direction questions
pincer-calibrate --rosbridge ws://robot:9090 --arm      # the four arm servos only
```

For every joint you drive the part to its neutral with the keyboard (coxa along the mount, femur
horizontal, tibia vertical, arm straight up with the camera level), press enter, then say whether a
ten-degree nudge went the positive way. The file lands in `~/.pincer1/servo_calibration.json` and the
`joint` backend refuses to start without it.
