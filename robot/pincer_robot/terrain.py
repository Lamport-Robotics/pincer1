"""The floor ahead, from the depth camera on the head.

A depth image and the camera's pose become a height grid on the ground in front of the body (cells of
5 cm, body frame on the floor: x forward, y left, z up). Cells are labelled floor, step, wall or drop,
and ``find_step`` reads the first riser out of it: how far, how tall, how it is angled to the body. That
is what the climb cycle needs to line up and what the motion layer needs to refuse a walk into a wall or
off an edge. The lidar sees a slice at one height; this sees the ground.

Pure numpy. The camera is the ROS optical convention (z forward out of the lens, x right, y down) and its
pose is where the head is in the body frame: height above the floor, elevation (positive up, the same
``tilt`` the gaze module drives) and yaw. ``render_steps`` draws a synthetic depth image of a staircase for
tests and the mock robot.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class CameraModel:
    fx: float
    fy: float
    cx: float
    cy: float
    width: int
    height: int

    @classmethod
    def from_fov(cls, width: int, height: int, hfov_deg: float) -> CameraModel:
        fx = (width / 2.0) / math.tan(math.radians(hfov_deg) / 2.0)
        return cls(fx, fx, (width - 1) / 2.0, (height - 1) / 2.0, width, height)


@dataclass(frozen=True)
class CameraPose:
    """The lens in the body-on-floor frame: metres and radians."""

    x: float = 0.07
    y: float = 0.0
    z: float = 0.40
    tilt: float = 0.0  # elevation, positive up
    yaw: float = 0.0  # positive left

    def rotation(self) -> np.ndarray:
        """Optical frame (x right, y down, z forward) -> body frame (x forward, y left, z up)."""
        optical_to_body = np.array([[0.0, 0.0, 1.0], [-1.0, 0.0, 0.0], [0.0, -1.0, 0.0]])
        cp, sp = math.cos(self.tilt), math.sin(self.tilt)
        pitch = np.array([[cp, 0.0, -sp], [0.0, 1.0, 0.0], [sp, 0.0, cp]])  # nose up for +tilt
        cy, sy = math.cos(self.yaw), math.sin(self.yaw)
        yaw = np.array([[cy, -sy, 0.0], [sy, cy, 0.0], [0.0, 0.0, 1.0]])
        return yaw @ pitch @ optical_to_body

    def origin(self) -> np.ndarray:
        return np.array([self.x, self.y, self.z])


@dataclass
class FloorGrid:
    heights: np.ndarray  # [nx, ny] metres above the floor, nan where nothing was seen
    counts: np.ndarray
    cell: float
    x0: float  # x of the first cell's near edge
    y0: float  # y of the first cell's right edge (y grows left)
    floor_far: float = math.inf  # how far along x the camera can see the floor plane at all

    def x_of(self, i: int) -> float:
        return self.x0 + (i + 0.5) * self.cell

    def y_of(self, j: int) -> float:
        return self.y0 + (j + 0.5) * self.cell

    @property
    def nx(self) -> int:
        return int(self.heights.shape[0])

    @property
    def ny(self) -> int:
        return int(self.heights.shape[1])


FLOOR, STEP, WALL, DROP, UNKNOWN = 0, 1, 2, 3, 4


def unproject(depth_m: np.ndarray, cam: CameraModel, pose: CameraPose, *, stride: int = 2,
              max_depth_m: float = 3.0) -> np.ndarray:
    """Depth image (metres, 0 or nan = no return) -> points in the body frame, shape [n, 3]."""
    d = np.asarray(depth_m, dtype=np.float64)[::stride, ::stride]
    v, u = np.mgrid[0 : cam.height : stride, 0 : cam.width : stride]
    ok = np.isfinite(d) & (d > 0.05) & (d < max_depth_m)
    z = d[ok]
    x = (u[ok] - cam.cx) / cam.fx * z
    y = (v[ok] - cam.cy) / cam.fy * z
    pts = np.stack([x, y, z], axis=1)
    return pts @ pose.rotation().T + pose.origin()


def height_grid(depth_m: np.ndarray, cam: CameraModel, pose: CameraPose, *, cell: float = 0.05,
                x_range: tuple[float, float] = (0.05, 1.20), y_range: tuple[float, float] = (-0.40, 0.40),
                stride: int = 2, min_points: int = 3) -> FloorGrid:
    """Bin the points onto the ground; a cell's height is the median of what landed in it."""
    pts = unproject(depth_m, cam, pose, stride=stride)
    nx = int(round((x_range[1] - x_range[0]) / cell))
    ny = int(round((y_range[1] - y_range[0]) / cell))
    heights = np.full((nx, ny), np.nan)
    counts = np.zeros((nx, ny), dtype=int)
    if len(pts):
        i = np.floor((pts[:, 0] - x_range[0]) / cell).astype(int)
        j = np.floor((pts[:, 1] - y_range[0]) / cell).astype(int)
        keep = (i >= 0) & (i < nx) & (j >= 0) & (j < ny)
        i, j, z = i[keep], j[keep], pts[keep, 2]
        order = np.lexsort((z, j, i))
        i, j, z = i[order], j[order], z[order]
        key = i * ny + j
        starts = np.flatnonzero(np.r_[True, key[1:] != key[:-1]])
        ends = np.r_[starts[1:], len(key)]
        for s, e in zip(starts, ends):
            if e - s >= min_points:
                # sorted within the cell: take the 90th percentile, so a cell that holds a riser
                # face reads as the top of that face and a wall reads as a wall, while the odd
                # high outlier is still dropped
                heights[i[s], j[s]] = z[s + int(0.9 * (e - s - 1))]
                counts[i[s], j[s]] = e - s
    return FloorGrid(heights, counts, cell, x_range[0], y_range[0], floor_far=floor_reach(cam, pose))


def floor_reach(cam: CameraModel, pose: CameraPose) -> float:
    """x where the top image row's centre ray meets the floor: beyond it nothing of the floor is in view."""
    d = pose.rotation() @ np.array([0.0, -cam.cy / cam.fy, 1.0])
    if d[2] >= 0:
        return math.inf
    t = -pose.z / d[2]
    return float(pose.x + t * d[0])


def classify(grid: FloorGrid, *, floor_tol_m: float = 0.025, step_max_m: float = 0.22) -> np.ndarray:
    h = grid.heights
    labels = np.full(h.shape, UNKNOWN, dtype=int)
    seen = np.isfinite(h)
    labels[seen & (np.abs(h) <= floor_tol_m)] = FLOOR
    labels[seen & (h > floor_tol_m) & (h <= step_max_m)] = STEP
    labels[seen & (h > step_max_m)] = WALL
    labels[seen & (h < -floor_tol_m)] = DROP
    # floor that should be in view but returned nothing: the floor ends there (a drop, or a
    # surface the camera cannot see, such as glass); either way not somewhere to walk
    limit = grid.floor_far - 2 * grid.cell
    for j in range(grid.ny):
        col = labels[:, j]
        floor_rows = np.flatnonzero(col == FLOOR)
        if not len(floor_rows):
            continue
        i = floor_rows[0]
        while i < grid.nx and grid.x_of(i) < limit:
            if col[i] == UNKNOWN and i + 1 < grid.nx and col[i + 1] == UNKNOWN:
                col[i:] = np.where(col[i:] == UNKNOWN, DROP, col[i:])
                break
            i += 1
    return labels


@dataclass(frozen=True)
class StepEdge:
    distance_m: float  # along x from the body centre to the riser, at y = 0
    riser_m: float  # height of the tread above the floor the body stands on
    yaw: float  # angle of the edge relative to the body's y axis; 0 = square on
    tread_m: float | None  # depth of the tread before the next rise, if seen
    rows: int  # how many grid rows the edge was found in


def find_step(grid: FloorGrid, labels: np.ndarray | None = None, *, corridor_m: float = 0.30,
              min_rows: int = 3) -> StepEdge | None:
    """The first riser ahead inside a corridor of ±corridor_m: floor cells, then step cells."""
    if labels is None:
        labels = classify(grid)
    edges: list[tuple[float, float]] = []  # (y, x of the edge)
    risers: list[float] = []
    treads: list[float] = []
    for j in range(grid.ny):
        y = grid.y_of(j)
        if abs(y) > corridor_m:
            continue
        col = labels[:, j]
        seen_floor = False
        for i in range(grid.nx):
            if col[i] == FLOOR:
                seen_floor = True
            elif col[i] == STEP and seen_floor:
                edges.append((y, grid.x_of(i)))
                # the edge cell holds the riser face; the tread height is read from it and the
                # cell behind it, whichever is taller, so a face that only half fills its cell
                # does not read short
                top = float(grid.heights[i, j])
                if i + 1 < grid.nx and labels[i + 1, j] == STEP:
                    top = max(top, float(grid.heights[i + 1, j]))
                risers.append(top)
                # the tread runs until the height rises again or the view ends
                k = i
                while k + 1 < grid.nx and labels[k + 1, j] == STEP and grid.heights[k + 1, j] - top < 0.03:
                    k += 1
                if k + 1 < grid.nx and labels[k + 1, j] in (STEP, WALL):
                    treads.append((k + 1 - i) * grid.cell)
                break
            elif col[i] in (WALL, DROP):
                break
    if len(edges) < min_rows:
        return None
    ys = np.array([e[0] for e in edges])
    xs = np.array([e[1] for e in edges])
    # x = a + b*y; the edge at y=0 is a, and an edge turned left (counter-clockwise, positive
    # yaw) comes closer on the left, so its x falls with y
    b, a = np.polyfit(ys, xs, 1) if len(edges) > 1 else (0.0, float(xs[0]))
    return StepEdge(
        distance_m=float(a),
        riser_m=float(np.median(risers)),
        yaw=float(-math.atan(b)),
        tread_m=float(np.median(treads)) if len(treads) >= min_rows else None,
        rows=len(edges),
    )


def front_clearance(grid: FloorGrid, labels: np.ndarray | None = None, *, corridor_m: float = 0.15) -> float | None:
    """Distance to the nearest wall or drop straight ahead, or None if the corridor is clear."""
    if labels is None:
        labels = classify(grid)
    best: float | None = None
    for j in range(grid.ny):
        if abs(grid.y_of(j)) > corridor_m:
            continue
        hits = np.flatnonzero(np.isin(labels[:, j], (WALL, DROP)))
        if len(hits):
            x = grid.x_of(hits[0])
            best = x if best is None else min(best, x)
    return best


def render_steps(cam: CameraModel, pose: CameraPose, *, riser_m: float = 0.0, edge_x_m: float = 0.5,
                 tread_m: float = 0.28, steps: int = 3, edge_yaw: float = 0.0, wall_x_m: float | None = None,
                 drop_x_m: float | None = None, drop_m: float = 0.3, noise_m: float = 0.0,
                 seed: int = 0) -> np.ndarray:
    """A depth image of a flat floor with, optionally, a staircase, a wall or a drop ahead.

    Risers are vertical planes, treads horizontal, all axis aligned after rotating the scene by
    ``edge_yaw`` about z. Returns metres along the optical axis, 0 where a ray never hits anything.
    """
    v, u = np.mgrid[0 : cam.height, 0 : cam.width]
    dirs = np.stack([(u - cam.cx) / cam.fx, (v - cam.cy) / cam.fy, np.ones_like(u, dtype=float)], axis=-1)
    R = pose.rotation()
    d = dirs @ R.T  # ray directions in the body frame
    o = pose.origin()
    # rotate the scene so the stair edge is square: rotate rays and origin by -edge_yaw
    cz, sz = math.cos(-edge_yaw), math.sin(-edge_yaw)
    Rz = np.array([[cz, -sz, 0.0], [sz, cz, 0.0], [0.0, 0.0, 1.0]])
    d = d @ Rz.T
    o = Rz @ o
    best = np.full(d.shape[:2], np.inf)

    def hit_plane_z(z0: float, x_lo: float, x_hi: float) -> None:
        with np.errstate(divide="ignore", invalid="ignore"):
            t = (z0 - o[2]) / d[..., 2]
        x = o[0] + t * d[..., 0]
        ok = (t > 0) & (x >= x_lo) & (x < x_hi)
        best[ok] = np.minimum(best[ok], t[ok])

    def hit_plane_x(x0: float, z_lo: float, z_hi: float) -> None:
        with np.errstate(divide="ignore", invalid="ignore"):
            t = (x0 - o[0]) / d[..., 0]
        z = o[2] + t * d[..., 2]
        ok = (t > 0) & (z >= z_lo) & (z < z_hi)
        best[ok] = np.minimum(best[ok], t[ok])

    floor_end = edge_x_m if riser_m > 0 else (drop_x_m if drop_x_m is not None else (wall_x_m if wall_x_m is not None else np.inf))
    hit_plane_z(0.0, -np.inf, floor_end)
    if riser_m > 0:
        for k in range(steps):
            x_r = edge_x_m + k * tread_m
            hit_plane_x(x_r, k * riser_m, (k + 1) * riser_m)
            hit_plane_z((k + 1) * riser_m, x_r, x_r + tread_m if k < steps - 1 else np.inf)
    if wall_x_m is not None:
        hit_plane_x(wall_x_m, 0.0, 10.0)
    if drop_x_m is not None:
        hit_plane_z(-drop_m, drop_x_m, np.inf)
        hit_plane_x(drop_x_m, -drop_m, 0.0)
    # depth along the optical axis is the ray parameter times the forward component in optical units,
    # and dirs has z = 1 there, so t is already the optical depth
    depth = np.where(np.isfinite(best), best, 0.0)
    if noise_m > 0:
        rng = np.random.default_rng(seed)
        depth = np.where(depth > 0, depth + rng.normal(0.0, noise_m, depth.shape) * depth, 0.0)
    return depth
