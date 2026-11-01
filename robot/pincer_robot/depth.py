"""Depth frames for ``terrain``: the head camera's stereo depth, or a drawn scene without one.

``OakDepthSource`` runs the two mono cameras of the OAK through its on-device stereo block and
returns metres; intrinsics come from the device's own calibration. ``MockDepthSource`` draws a floor
with an optional step, wall or drop (``terrain.render_steps``) so the laptop run and the tests see
the same numbers the robot would. ``FloorWatch`` turns frames into what the rest of the service
reads: the clearance ahead and the first riser, with the head's current tilt folded in.
"""

from __future__ import annotations

import math
import time
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any, Protocol

import numpy as np

from .terrain import (
    CameraModel,
    CameraPose,
    FloorGrid,
    StepEdge,
    classify,
    find_step,
    front_clearance,
    height_grid,
    render_steps,
)


class DepthSource(Protocol):
    model: CameraModel

    def read(self) -> np.ndarray | None:
        """Depth in metres, float, 0 where there is no return; None if no frame is ready."""
        ...

    def close(self) -> None: ...


class OakDepthSource:
    """Stereo depth from the head camera. The import is lazy: only the robot has ``depthai``."""

    def __init__(
        self, width: int = 640, height: int = 400, fps: float = 10.0, *, confidence: int = 200
    ):
        import depthai as dai  # type: ignore[import-not-found]

        self._dai = dai
        pipeline = dai.Pipeline()
        left = pipeline.create(dai.node.MonoCamera)
        right = pipeline.create(dai.node.MonoCamera)
        for cam, socket in (
            (left, dai.CameraBoardSocket.CAM_B),
            (right, dai.CameraBoardSocket.CAM_C),
        ):
            cam.setBoardSocket(socket)
            cam.setResolution(dai.MonoCameraProperties.SensorResolution.THE_400_P)
            cam.setFps(fps)
        stereo = pipeline.create(dai.node.StereoDepth)
        stereo.setDefaultProfilePreset(dai.node.StereoDepth.PresetMode.HIGH_DENSITY)
        stereo.initialConfig.setConfidenceThreshold(confidence)
        stereo.setLeftRightCheck(True)
        stereo.setSubpixel(True)
        stereo.setDepthAlign(dai.CameraBoardSocket.CAM_C)  # depth in the right mono camera's frame
        left.out.link(stereo.left)
        right.out.link(stereo.right)
        out = pipeline.create(dai.node.XLinkOut)
        out.setStreamName("depth")
        stereo.depth.link(out.input)
        self._device = dai.Device(pipeline)
        self._queue = self._device.getOutputQueue("depth", maxSize=1, blocking=False)
        m = np.array(
            self._device.readCalibration().getCameraIntrinsics(
                dai.CameraBoardSocket.CAM_C, width, height
            )
        )
        self.model = CameraModel(
            float(m[0, 0]), float(m[1, 1]), float(m[0, 2]), float(m[1, 2]), width, height
        )

    def read(self) -> np.ndarray | None:
        pkt = self._queue.tryGet()
        if pkt is None:
            return None
        return pkt.getFrame().astype(np.float32) / 1000.0  # millimetres on the wire

    def close(self) -> None:
        try:
            self._device.close()
        except Exception:
            pass


class MockDepthSource:
    """A drawn scene: ``scene`` is what ``render_steps`` takes (riser_m, edge_x_m, wall_x_m,
    drop_x_m, …)."""

    def __init__(
        self,
        pose: Callable[[], CameraPose],
        model: CameraModel | None = None,
        *,
        scene: dict[str, Any] | None = None,
        noise_m: float = 0.004,
    ):
        self.model = model or CameraModel.from_fov(320, 200, 73.0)
        self._pose = pose
        self.scene = dict(scene or {})
        self.noise_m = noise_m
        self._seed = 0

    def read(self) -> np.ndarray | None:
        self._seed += 1
        return render_steps(
            self.model, self._pose(), noise_m=self.noise_m, seed=self._seed, **self.scene
        )

    def close(self) -> None:
        pass


def mock_scene(name: str) -> dict[str, Any]:
    """Named scenes for ``PINCER_MOCK_DEPTH``: flat, step, stairs, wall, drop."""
    return {
        "flat": {},
        "step": {"riser_m": 0.12, "edge_x_m": 0.6, "steps": 1},
        "stairs": {"riser_m": 0.17, "edge_x_m": 0.7, "tread_m": 0.28, "steps": 3},
        "wall": {"wall_x_m": 0.5},
        "drop": {"drop_x_m": 0.6},
    }[name]


@dataclass(frozen=True)
class FloorReport:
    ts: float
    clearance_m: float | None  # nearest wall or drop straight ahead; None = corridor clear
    step: StepEdge | None  # first riser ahead, if any
    seen_cells: int

    @property
    def walkable_m(self) -> float:
        """How far forward the floor is known to carry on: the clearance, or the riser, or far."""
        out = math.inf if self.clearance_m is None else self.clearance_m
        if self.step is not None:
            out = min(out, self.step.distance_m)
        return out


class FloorWatch:
    """One grid per frame. ``head_pose`` is called every frame so the gaze's tilt is used as is."""

    def __init__(
        self, source: DepthSource, head_pose: Callable[[], CameraPose], *, stride: int = 2
    ):
        self.source = source
        self.head_pose = head_pose
        self.stride = stride
        self.last: FloorReport | None = None
        self.grid: FloorGrid | None = None

    def update(self, now: float | None = None) -> FloorReport | None:
        depth = self.source.read()
        if depth is None:
            return None
        pose = self.head_pose()
        grid = height_grid(depth, self.source.model, pose, stride=self.stride)
        labels = classify(grid)
        report = FloorReport(
            ts=time.time() if now is None else now,
            clearance_m=front_clearance(grid, labels),
            step=find_step(grid, labels),
            seen_cells=int(np.isfinite(grid.heights).sum()),
        )
        self.grid, self.last = grid, report
        return report
