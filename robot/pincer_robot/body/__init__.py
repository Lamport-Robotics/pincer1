"""The legs: geometry, kinematics, gait, the servo bus and the controller tying them together."""

from .controller import Body, BodyBackend, BusActuator, HealthReport, ServoHealth, simulated_body
from .gait import GAITS, GaitParams, GaitState, Velocity
from .geometry import LEG_NAMES, BodyGeometry, JointLimits
from .kinematics import BodyPose, JointAngles, ReachError, forward, inverse, solve_body, solve_leg
from .servo import Bus, Calibration, LoopbackTransport, SerialTransport, ServoError
from .sim import Simulation, StabilityReport

__all__ = [
    "GAITS",
    "LEG_NAMES",
    "Body",
    "BodyBackend",
    "BodyGeometry",
    "BodyPose",
    "Bus",
    "BusActuator",
    "Calibration",
    "GaitParams",
    "GaitState",
    "HealthReport",
    "JointAngles",
    "JointLimits",
    "LoopbackTransport",
    "ReachError",
    "SerialTransport",
    "ServoError",
    "ServoHealth",
    "Simulation",
    "StabilityReport",
    "Velocity",
    "forward",
    "inverse",
    "simulated_body",
    "solve_body",
    "solve_leg",
]
