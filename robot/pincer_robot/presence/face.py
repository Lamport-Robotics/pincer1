"""Owner face recognition + liveness.

Two layers:

* ``FaceRecognizer`` turns a camera frame into ``FaceObservation``s (embedding, box,
  head pose). ``InsightFaceRecognizer`` uses ArcFace through onnxruntime: the
  ``CUDAExecutionProvider`` on the Jetson (``onnxruntime-gpu`` from NVIDIA's Jetson
  wheel index, ``buffalo_l`` pack), the CPU provider on a Pi 5 / laptop (``buffalo_sc``);
  no torch either way. ``MockFaceRecognizer`` replays scripted observations.
* ``OwnerFaceMatcher`` is pure logic: cosine-matches the best face against the enrolled
  embeddings and runs ``LivenessGate``. Liveness requires, inside a short window, several
  frames whose embeddings agree (same person) *and* whose head pose varies (a printed
  photo or a phone held still gives identical pose every frame). Without liveness the
  owner confidence is scaled by ``not_live_factor`` so it cannot cross the fusion's
  ``owner_on`` threshold.

Several people can be enrolled (``people`` = label -> embeddings). ``owner`` /
``owner_conf`` always refer to the *primary* owner (the first label); ``owners`` lists
every enrolled label whose face matched strongly within the liveness window, which is
what the two-person rule reads. Secondary people get no liveness gate of their own: the
rule is "two enrolled faces in front of a robot that already sees a live primary owner".

Known limit: a replayed video of the owner moving their head passes this gate. The
passphrase + speaker check and the physical touch exist for that reason.
"""

from __future__ import annotations

import math
from collections import deque
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any, Protocol

import numpy as np


@dataclass(frozen=True)
class FaceObservation:
    embedding: np.ndarray  # L2-normalised
    bbox: tuple[float, float, float, float]  # x1, y1, x2, y2 pixels
    pose: tuple[float, float, float]  # yaw, pitch, roll in degrees
    frame_width: int
    ts: float
    det_score: float = 1.0

    @property
    def x_norm(self) -> float:
        """Horizontal position of the face centre, -1 (left edge) .. +1 (right edge)."""
        cx = (self.bbox[0] + self.bbox[2]) / 2.0
        return float(2.0 * cx / max(1, self.frame_width) - 1.0)


class FaceRecognizer(Protocol):
    def observe(self, frame: Any, ts: float) -> list[FaceObservation]: ...


def l2norm(v: np.ndarray) -> np.ndarray:
    v = np.asarray(v, dtype=np.float64)
    n = float(np.linalg.norm(v))
    return v / n if n > 0 else v


def cosine(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.dot(l2norm(a), l2norm(b)))


def pose_from_kps(kps: np.ndarray) -> tuple[float, float, float]:
    """Rough yaw/pitch/roll (deg) from the 5 ArcFace keypoints.

    kps rows: left eye, right eye, nose, left mouth, right mouth (image coords).
    Absolute accuracy is poor; frame-to-frame *variation* is what liveness needs.
    """
    le, re, nose, lm, rm = (np.asarray(kps[i], dtype=np.float64) for i in range(5))
    eye_mid = (le + re) / 2.0
    mouth_mid = (lm + rm) / 2.0
    inter_eye = float(np.linalg.norm(re - le)) or 1.0
    yaw = float(np.clip((nose[0] - eye_mid[0]) / inter_eye, -1, 1)) * 60.0
    face_h = float(np.linalg.norm(mouth_mid - eye_mid)) or 1.0
    pitch = (float((nose[1] - eye_mid[1]) / face_h) - 0.55) * 120.0
    roll = math.degrees(math.atan2(re[1] - le[1], re[0] - le[0]))
    return yaw, pitch, roll


@dataclass
class FaceConfig:
    sim_low: float = 0.20
    sim_high: float = 0.60
    not_live_factor: float = 0.25
    window_s: float = 3.0
    min_frames: int = 4
    min_pose_std_deg: float = 3.0
    min_embed_sim: float = 0.70
    owners_match_conf: float = 0.70  # confidence a label needs to be listed in ``owners``


@dataclass(frozen=True)
class FaceResult:
    owner_conf: float
    present: bool
    live: bool
    x_norm: float | None
    match_sim: float
    reason: str
    owners: tuple[str, ...] = ()  # enrolled labels matched within the window
    faces: int = 0


class LivenessGate:
    """Embedding-stable but pose-varying frames within a time window."""

    def __init__(self, cfg: FaceConfig) -> None:
        self.cfg = cfg
        self._win: deque[FaceObservation] = deque()

    def reset(self) -> None:
        self._win.clear()

    def push(self, obs: FaceObservation) -> None:
        if self._win and obs.ts - self._win[-1].ts > self.cfg.window_s:
            self._win.clear()
        self._win.append(obs)
        while self._win and obs.ts - self._win[0].ts > self.cfg.window_s:
            self._win.popleft()

    def assess(self) -> tuple[bool, str]:
        cfg = self.cfg
        if len(self._win) < cfg.min_frames:
            return False, "too_few_frames"
        embs = np.stack([o.embedding for o in self._win])
        mean = l2norm(embs.mean(axis=0))
        sims = embs @ mean
        if float(sims.min()) < cfg.min_embed_sim:
            return False, "identity_unstable"
        poses = np.array([o.pose for o in self._win], dtype=np.float64)
        std = poses.std(axis=0)
        if float(max(std[0], std[1])) < cfg.min_pose_std_deg:
            return False, "static_pose"
        return True, "ok"


class OwnerFaceMatcher:
    """Matches faces against one primary owner and any number of other enrolled people.

    ``enrolled`` is the primary owner's embedding matrix (kept positional for the
    single-owner callers); ``people`` adds ``{label: matrix}`` for everyone else.
    """

    def __init__(
        self,
        enrolled: np.ndarray,
        cfg: FaceConfig | None = None,
        *,
        label: str = "owner",
        people: dict[str, np.ndarray] | None = None,
    ) -> None:
        self.cfg = cfg or FaceConfig()
        self.label = label
        self.enrolled = self._matrix(enrolled)
        self.people: dict[str, np.ndarray] = {label: self.enrolled}
        for name, m in (people or {}).items():
            if name != label:
                self.people[name] = self._matrix(m)
        self._gate = LivenessGate(self.cfg)
        self._seen: dict[str, float] = {}  # label -> last strong match ts
        self.last: FaceResult | None = None

    @staticmethod
    def _matrix(m: np.ndarray) -> np.ndarray:
        e = np.asarray(m, dtype=np.float64)
        if e.ndim == 1:
            e = e[None, :]
        return np.stack([l2norm(v) for v in e]) if len(e) else e

    def match_similarity(self, emb: np.ndarray, label: str | None = None) -> float:
        m = self.people.get(label or self.label)
        if m is None or len(m) == 0:
            return -1.0
        return float((m @ l2norm(emb)).max())

    def sim_to_conf(self, sim: float) -> float:
        cfg = self.cfg
        return float(np.clip((sim - cfg.sim_low) / (cfg.sim_high - cfg.sim_low), 0.0, 1.0))

    def _owners_in_window(self, observations: list[FaceObservation]) -> tuple[str, ...]:
        """Labels whose best observation this frame is a strong match, remembered for
        ``window_s`` so two people need not be in the same frame."""
        cfg = self.cfg
        now = max(o.ts for o in observations)
        for name in self.people:
            best = max(self.match_similarity(o.embedding, name) for o in observations)
            if self.sim_to_conf(best) >= cfg.owners_match_conf:
                self._seen[name] = now
        self._seen = {k: t for k, t in self._seen.items() if now - t <= cfg.window_s}
        return tuple(k for k in self.people if k in self._seen)

    def update(self, observations: list[FaceObservation]) -> FaceResult:
        if not observations:
            res = FaceResult(0.0, False, False, None, -1.0, "no_face", (), 0)
            self.last = res
            return res
        sims = [self.match_similarity(o.embedding) for o in observations]
        k = int(np.argmax(sims))
        best, sim = observations[k], sims[k]
        self._gate.push(best)
        live, reason = self._gate.assess()
        conf = self.sim_to_conf(sim)
        if not live:
            conf *= self.cfg.not_live_factor
        owners = self._owners_in_window(observations)
        res = FaceResult(conf, True, live, best.x_norm, sim, reason, owners, len(observations))
        self.last = res
        return res


class MockFaceRecognizer:
    """Replays a scripted sequence of observation lists (cycles when exhausted)."""

    def __init__(self, script: list[list[FaceObservation]] | None = None) -> None:
        self.script = script or []
        self._i = 0

    def observe(self, frame: Any, ts: float) -> list[FaceObservation]:
        if not self.script:
            return []
        obs = self.script[self._i % len(self.script)]
        self._i += 1
        return [
            FaceObservation(o.embedding, o.bbox, o.pose, o.frame_width, ts, o.det_score)
            for o in obs
        ]


def make_mock_observation(
    embedding: np.ndarray,
    ts: float,
    *,
    yaw: float = 0.0,
    pitch: float = 0.0,
    x_norm: float = 0.0,
    frame_width: int = 640,
) -> FaceObservation:
    cx = (x_norm + 1.0) / 2.0 * frame_width
    return FaceObservation(
        embedding=l2norm(embedding),
        bbox=(cx - 60, 100, cx + 60, 260),
        pose=(yaw, pitch, 0.0),
        frame_width=frame_width,
        ts=ts,
    )


class InsightFaceRecognizer:
    """ArcFace via insightface + onnxruntime. Heavy imports happen in __init__.

    ``providers`` is the onnxruntime provider list in priority order; the default is
    ``["CUDAExecutionProvider", "CPUExecutionProvider"]`` when ``ctx_id >= 0`` (GPU) and
    CPU-only otherwise, so a missing CUDA provider falls through to CPU rather than
    failing. On the Jetson the CUDA provider only exists in the ``onnxruntime-gpu`` wheel
    from NVIDIA's Jetson index (the PyPI ``onnxruntime-gpu`` has no aarch64/Tegra build);
    ``compute.ComputePlan`` supplies ``model``/``ctx_id``/``providers`` for the detected
    device.
    """

    def __init__(
        self,
        model: str = "buffalo_sc",
        det_size: int = 320,
        ctx_id: int = -1,
        providers: Sequence[str] | None = None,
    ) -> None:
        from insightface.app import FaceAnalysis  # type: ignore[import-not-found]

        if providers is None:
            providers = (
                ["CUDAExecutionProvider", "CPUExecutionProvider"]
                if ctx_id >= 0
                else ["CPUExecutionProvider"]
            )
        self.model = model
        self.providers = list(providers)
        self._app = FaceAnalysis(name=model, providers=self.providers)
        self._app.prepare(ctx_id=ctx_id, det_size=(det_size, det_size))

    @classmethod
    def from_plan(cls, plan: Any, det_size: int = 320) -> InsightFaceRecognizer:
        """Build from a ``compute.ComputePlan`` (model, ctx_id and providers per device)."""
        return cls(plan.face_model, det_size, plan.face_ctx_id, plan.ort_providers)

    def observe(self, frame: Any, ts: float) -> list[FaceObservation]:
        faces = self._app.get(frame)
        width = int(frame.shape[1])
        out: list[FaceObservation] = []
        for f in faces:
            emb = getattr(f, "normed_embedding", None)
            if emb is None:
                emb = l2norm(f.embedding)
            pose = getattr(f, "pose", None)
            if pose is not None and len(pose) == 3:
                # insightface reports (pitch, yaw, roll)
                p, y, r = (float(v) for v in pose)
                pose_t = (y, p, r)
            else:
                pose_t = pose_from_kps(f.kps)
            x1, y1, x2, y2 = (float(v) for v in f.bbox)
            out.append(
                FaceObservation(
                    embedding=np.asarray(emb, dtype=np.float64),
                    bbox=(x1, y1, x2, y2),
                    pose=pose_t,
                    frame_width=width,
                    ts=ts,
                    det_score=float(getattr(f, "det_score", 1.0)),
                )
            )
        return out


class CameraSource:
    """OpenCV camera frames. Imported lazily; absent in mock mode."""

    def __init__(self, index: int = 0, width: int = 640, height: int = 480) -> None:
        import cv2  # type: ignore[import-not-found]

        self._cv2 = cv2
        self._cap = cv2.VideoCapture(index)
        self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        if not self._cap.isOpened():
            raise RuntimeError(f"camera {index} could not be opened")

    def read(self) -> Any | None:
        ok, frame = self._cap.read()
        return frame if ok else None

    def close(self) -> None:
        self._cap.release()
