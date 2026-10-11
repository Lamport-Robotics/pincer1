"""ROS2 Humble backend for the real Pincer1 chassis. The ONLY module that imports rclpy.

One node, three topics:
* publishes geometry_msgs/Twist on ``cmd_vel_topic`` (gait controller),
* subscribes sensor_msgs/LaserScan on ``scan_topic`` -> ``on_scan(Scan)``,
* subscribes sensor_msgs/Imu on ``imu_topic`` -> ``on_imu(accel_xyz, tilt_deg, ts)``.

Callbacks fire on the spin thread; the fusion loop consumes them through
thread-safe handoffs in main.py.

Verify the topic names on the robot before trusting the defaults::

    ros2 topic list | grep -iE 'cmd_vel|scan|imu'
    ros2 topic echo /imu --once
"""

from __future__ import annotations

import logging
import math
import threading
import time
from collections.abc import Callable

from .motion import RobotInterface, RobotStatus
from .presence.lidar import Scan, laserscan_to_scan

log = logging.getLogger("pincer.ros")

ScanCallback = Callable[[Scan], None]
ImuCallback = Callable[[tuple[float, float, float], float, float], None]


class HexapodRobot(RobotInterface):
    def __init__(
        self,
        *,
        cmd_vel_topic: str = "/cmd_vel",
        scan_topic: str = "/scan",
        imu_topic: str = "/imu",
        on_scan: ScanCallback | None = None,
        on_imu: ImuCallback | None = None,
        node_name: str = "pincer_robot",
    ) -> None:
        import rclpy  # type: ignore[import-not-found]
        from geometry_msgs.msg import Twist  # type: ignore[import-not-found]
        from rclpy.node import Node  # type: ignore[import-not-found]
        from sensor_msgs.msg import Imu, LaserScan  # type: ignore[import-not-found]

        if not rclpy.ok():
            rclpy.init()
        self._rclpy = rclpy
        self._Twist = Twist
        self._node: Node = rclpy.create_node(node_name)
        self._pub = self._node.create_publisher(Twist, cmd_vel_topic, 10)
        self._node.create_subscription(LaserScan, scan_topic, self._on_scan, 10)
        self._node.create_subscription(Imu, imu_topic, self._on_imu, 50)
        self._on_scan_cb = on_scan
        self._on_imu_cb = on_imu
        self._status = RobotStatus(note="hexapod ros2")
        self._spinning = True
        self._thread = threading.Thread(target=self._spin, name="ros-spin", daemon=True)
        self._thread.start()
        log.info(
            "ROS2 node %s up: cmd_vel=%s scan=%s imu=%s",
            node_name,
            cmd_vel_topic,
            scan_topic,
            imu_topic,
        )

    def _spin(self) -> None:
        while self._spinning and self._rclpy.ok():
            self._rclpy.spin_once(self._node, timeout_sec=0.1)

    def _on_imu(self, msg) -> None:  # noqa: ANN001
        x, y, z, w = (
            msg.orientation.x,
            msg.orientation.y,
            msg.orientation.z,
            msg.orientation.w,
        )
        sinr_cosp = 2 * (w * x + y * z)
        cosr_cosp = 1 - 2 * (x * x + y * y)
        roll = math.atan2(sinr_cosp, cosr_cosp)
        sinp = 2 * (w * y - z * x)
        pitch = math.asin(max(-1.0, min(1.0, sinp)))
        tilt = math.degrees(max(abs(roll), abs(pitch)))
        self._status.tilt_deg = tilt
        acc = msg.linear_acceleration
        if self._on_imu_cb is not None:
            self._on_imu_cb((float(acc.x), float(acc.y), float(acc.z)), tilt, time.time())

    def _on_scan(self, msg) -> None:  # noqa: ANN001
        scan = laserscan_to_scan(msg, time.time())
        # Front clearance: narrow cone around angle 0.
        a = scan.angles
        front = scan.valid_mask() & (abs(a) < math.radians(20))
        if front.any():
            self._status.front_clearance_m = float(scan.ranges[front].min())
        if self._on_scan_cb is not None:
            self._on_scan_cb(scan)

    def get_status(self) -> RobotStatus:
        return self._status

    def _drive(self, lin_x: float, ang_z: float, duration_s: float) -> None:
        twist = self._Twist()
        twist.linear.x = float(lin_x)
        twist.angular.z = float(ang_z)
        self._status.is_moving = True
        end = time.monotonic() + duration_s
        while time.monotonic() < end:
            self._pub.publish(twist)
            time.sleep(0.05)
        self.stop()

    def stop(self) -> None:
        self._pub.publish(self._Twist())
        self._status.is_moving = False

    def shutdown(self) -> None:
        self._spinning = False
        self.stop()
        self._node.destroy_node()
        if self._rclpy.ok():
            self._rclpy.shutdown()
