# Body

The legs, in software. `robot/pincer_robot/body/`, no ROS, no hardware needed to run it.

```
geometry.py    where the six coxa axes sit, segment lengths, joint limits, neutral stance, servo ids
kinematics.py  3-DOF leg FK/IK in closed form; a body pose (xyz, roll/pitch/yaw) moves every foot target
gait.py        tripod / ripple / wave: phase per leg, stance slides, swing lifts on a half sine
servo.py       serial-bus servo protocol: ping, read, write, sync write; calibration (sign, zero)
sim.py         forward kinematics + support polygon: which feet are down, how much margin the centre has
controller.py  Body (velocity in, joint angles out, 50 Hz) and BodyBackend for the motion layer
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

## calibration

Each servo has a sign and a zero offset in `Calibration`; `to_ticks` clamps to the 12-bit range.
Bring a leg to the neutral stance by hand with torque off, read the positions, and store the
difference from 2048 as that joint's offset. Tripod on a bench with the body on a stand first;
feet on the ground only after the limits in `JointLimits` have been checked against the real
brackets.
