"""pincer_robot: the robot-side service of Pincer1 (Lamport Robotics).

Runs on the hexapod's Raspberry Pi 5 and answers one question for the daemon,
continuously: *is the owner physically here right now?* It fuses the 2D lidar,
the camera (owner face + liveness), the microphone (passphrase + speaker
verification) and the IMU into the ``presence`` object of PROTOCOL.md section 2,
speaks for the daemon, and moves the body for "attention".

Nothing in here is trusted by the signing path: the consent controller and the
SE050 are. This package stops remote attackers from reaching the touch step and
protects the owner from social engineering; it does not hold the key.
"""

__all__ = ["__version__"]
__version__ = "0.1.0"
