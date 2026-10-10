"""Owner enrolment: ``pincer-enroll``.

Captures, into ``~/.pincer1/enrol.json`` (mode 0600):

* face embeddings - 10 frames, each accepted only if the head pose differs from
  every frame already kept, so the set spans yaw/pitch (the live matcher benefits and
  it doubles as a liveness rehearsal);
* voice embedding - the mean resemblyzer d-vector of three recordings of the passphrase;
* passphrase and duress word - stored ONLY as scrypt hashes (exact + phonetic) plus
  their word counts. The enrolment also stores hashes of what Whisper actually heard
  when you said the passphrase, so the live check tolerates your STT's habits.

The embeddings are stored raw. They are not secrets (you cannot reconstruct a face
or a voice from them well enough to fool the live checks, and the file is 0600 on
a device only the owner holds), but they are personal data: treat the file like a
password manager export.
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np
from pydantic import BaseModel, Field

from .config import Settings, load_settings
from .hashing import hash_phrase_pair, normalize_phrase

ENROL_VERSION = 1


class EnrolledPerson(BaseModel):
    """A second (third, ...) enrolled face for the two-person rule. Face only: the
    passphrase, duress word and voice stay the primary owner's."""

    label: str
    face_model: str = "mock"
    face_embeddings: list[list[float]] = Field(default_factory=list)
    created_ts: float = Field(default_factory=time.time)

    def face_matrix(self) -> np.ndarray:
        if not self.face_embeddings:
            return np.zeros((0, 0))
        return np.asarray(self.face_embeddings, dtype=np.float64)


class Enrolment(BaseModel):
    version: int = ENROL_VERSION
    owner_name: str
    created_ts: float = Field(default_factory=time.time)
    passphrase_hashes: list[str]
    passphrase_words: int
    duress_hashes: list[str] = Field(default_factory=list)
    duress_words: int = 1
    face_model: str = "mock"
    face_embeddings: list[list[float]] = Field(default_factory=list)
    voice_model: str = "mock"
    voice_embedding: list[float] | None = None
    people: list[EnrolledPerson] = Field(default_factory=list)
    notes: str = ""

    def face_matrix(self) -> np.ndarray:
        if not self.face_embeddings:
            return np.zeros((0, 0))
        return np.asarray(self.face_embeddings, dtype=np.float64)

    def people_faces(self) -> dict[str, np.ndarray]:
        """``{label: embeddings}`` for everyone *other than* the primary owner."""
        return {p.label: p.face_matrix() for p in self.people if p.face_embeddings}

    def labels(self) -> list[str]:
        return [self.owner_name] + [p.label for p in self.people]

    def add_person(self, person: EnrolledPerson) -> None:
        if person.label == self.owner_name:
            raise ValueError("that label is the primary owner")
        self.people = [p for p in self.people if p.label != person.label] + [person]

    def voice_vector(self) -> np.ndarray | None:
        if self.voice_embedding is None:
            return None
        return np.asarray(self.voice_embedding, dtype=np.float64)


def save_enrolment(enrol: Enrolment, path: Path) -> None:
    path = path.expanduser()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(enrol.model_dump_json(indent=2))
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def load_enrolment(path: Path) -> Enrolment:
    data = json.loads(path.expanduser().read_text())
    return Enrolment.model_validate(data)


def build_mock_enrolment(cfg: Settings, rng: np.random.Generator | None = None) -> Enrolment:
    """A fully synthetic enrolment for ``--mock`` runs and tests."""
    g = rng or np.random.default_rng(42)
    face = g.normal(size=(3, 512))
    face /= np.linalg.norm(face, axis=1, keepdims=True)
    voice = g.normal(size=256)
    voice /= np.linalg.norm(voice)
    return Enrolment(
        owner_name=cfg.owner_name,
        passphrase_hashes=hash_phrase_pair(cfg.mock_passphrase),
        passphrase_words=len(normalize_phrase(cfg.mock_passphrase).split()),
        duress_hashes=hash_phrase_pair(cfg.mock_duress_word),
        duress_words=len(normalize_phrase(cfg.mock_duress_word).split()),
        face_model="mock",
        face_embeddings=face.tolist(),
        voice_model="mock",
        voice_embedding=voice.tolist(),
        notes="synthetic mock enrolment - never use on a real robot",
    )


# --- interactive capture -----------------------------------------------------------------


def _ask_secret(label: str, min_words: int = 1) -> str:
    while True:
        a = getpass.getpass(f"{label} (not echoed): ")
        if len(normalize_phrase(a).split()) < min_words:
            print(f"  need at least {min_words} word(s); try again")
            continue
        b = getpass.getpass("repeat: ")
        if normalize_phrase(a) != normalize_phrase(b):
            print("  they differ; try again")
            continue
        return a


def _pose_distance(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def capture_face(cfg: Settings, *, frames: int = 10, min_pose_delta_deg: float = 8.0) -> Any:
    """Collect ``frames`` embeddings with pose variety. Returns (model_name, embeddings)."""
    from .presence.face import CameraSource, InsightFaceRecognizer

    plan = cfg.compute_plan()
    cam = CameraSource(cfg.camera_index)
    rec = InsightFaceRecognizer.from_plan(plan, cfg.face_det_size)
    kept: list[tuple[np.ndarray, tuple[float, float, float]]] = []
    prompts = [
        "look straight at the camera",
        "turn your head a little to the left",
        "a little to the right",
        "tilt your chin up",
        "chin down",
        "look straight again",
    ]
    print("Face: keep your face in frame and follow the prompts.")
    deadline = time.time() + 90
    try:
        while len(kept) < frames and time.time() < deadline:
            print(f"  [{len(kept)}/{frames}] {prompts[len(kept) % len(prompts)]}")
            time.sleep(0.8)
            frame = cam.read()
            if frame is None:
                continue
            obs = rec.observe(frame, time.time())
            if len(obs) != 1:
                print("    need exactly one face in view")
                continue
            o = obs[0]
            if any(_pose_distance(o.pose, p) < min_pose_delta_deg for _, p in kept):
                print("    move your head a bit more")
                continue
            kept.append((o.embedding, o.pose))
    finally:
        cam.close()
    if len(kept) < max(3, frames // 2):
        raise RuntimeError(f"only {len(kept)} usable face frames captured")
    return rec.model, [e.tolist() for e, _ in kept]


def capture_voice(cfg: Settings, passphrase: str) -> tuple[str, list[float], list[str]]:
    """Record the passphrase three times; returns (model, mean embedding, heard-variant hashes)."""
    from .presence.voice import ResemblyzerEncoder, WhisperSTT
    from .speech import SoundDeviceMicrophone

    plan = cfg.compute_plan()
    mic = SoundDeviceMicrophone(cfg.mic_sample_rate, cfg.mic_device)
    enc = ResemblyzerEncoder(plan.torch_device)
    stt = WhisperSTT.from_plan(plan, cfg.whisper_language)
    embs: list[np.ndarray] = []
    heard_hashes: list[str] = []
    target = normalize_phrase(passphrase)
    for i in range(3):
        input(f"  recording {i + 1}/3: press Enter, then say the passphrase clearly...")
        audio = mic.record(cfg.passphrase_record_s)
        embs.append(enc.embed(audio, mic.sample_rate))
        heard = stt.transcribe(audio, mic.sample_rate)
        hn = normalize_phrase(heard)
        if hn and hn != target and target not in hn:
            print("    STT heard something different; storing it as an accepted variant")
            heard_hashes.extend(hash_phrase_pair(hn))
        elif hn:
            print("    STT heard the passphrase")
        else:
            print("    STT heard nothing - check the microphone")
    mean = np.mean(np.stack(embs), axis=0)
    mean /= np.linalg.norm(mean) or 1.0
    return "resemblyzer", mean.tolist(), heard_hashes


def add_person(cfg: Settings, label: str, *, mock: bool, path: Path) -> Enrolment:
    """``pincer-enroll --add-person LABEL``: capture one more face into an existing file."""
    enrol = load_enrolment(path)
    if mock:
        g = np.random.default_rng(abs(hash(label)) % (2**32))
        face = g.normal(size=(3, 512))
        face /= np.linalg.norm(face, axis=1, keepdims=True)
        person = EnrolledPerson(label=label, face_model="mock", face_embeddings=face.tolist())
    else:
        print(f"Enrolling {label}'s face (the primary owner stays {enrol.owner_name}).")
        model, embs = capture_face(cfg)
        if model != enrol.face_model:
            print(f"  warning: pack {model} differs from the owner's {enrol.face_model}")
        person = EnrolledPerson(label=label, face_model=model, face_embeddings=embs)
    enrol.add_person(person)
    save_enrolment(enrol, path)
    return enrol


def run_enrol(cfg: Settings, *, mock: bool, out: Path) -> Enrolment:
    if mock:
        enrol = build_mock_enrolment(cfg)
        save_enrolment(enrol, out)
        return enrol
    name = input("Owner name: ").strip() or cfg.owner_name
    passphrase = _ask_secret("Passphrase (2+ words)", min_words=2)
    duress = _ask_secret("Duress word (1-2 words, never used normally)", min_words=1)
    face_model, face_embs = capture_face(cfg)
    voice_model, voice_emb, heard = capture_voice(cfg, passphrase)
    enrol = Enrolment(
        owner_name=name,
        passphrase_hashes=hash_phrase_pair(passphrase) + heard,
        passphrase_words=len(normalize_phrase(passphrase).split()),
        duress_hashes=hash_phrase_pair(duress),
        duress_words=len(normalize_phrase(duress).split()),
        face_model=face_model,
        face_embeddings=face_embs,
        voice_model=voice_model,
        voice_embedding=voice_emb,
        notes="words are scrypt hashes; embeddings are raw personal data, keep the file 0600",
    )
    save_enrolment(enrol, out)
    return enrol


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="pincer-enroll", description="Enrol the Pincer1 owner.")
    p.add_argument("--mock", action="store_true", help="write a synthetic enrolment")
    p.add_argument("--out", type=Path, default=None, help="path (default: PINCER_ENROL_PATH)")
    p.add_argument("--hash", metavar="PHRASE", help="print the hashes for a phrase and exit")
    p.add_argument(
        "--add-person", metavar="LABEL", help="add another enrolled face (two-person rule)"
    )
    args = p.parse_args(argv)
    cfg = load_settings()
    if args.hash:
        for h in hash_phrase_pair(args.hash):
            print(h)
        return 0
    out = args.out or cfg.enrol_file
    try:
        if args.add_person:
            enrol = add_person(cfg, args.add_person.strip(), mock=args.mock, path=out)
            print(f"Enrolled people: {', '.join(enrol.labels())} -> {out}")
            return 0
        enrol = run_enrol(cfg, mock=args.mock, out=out)
    except KeyboardInterrupt:
        print("\naborted", file=sys.stderr)
        return 130
    print(
        f"Enrolled {enrol.owner_name}: {len(enrol.face_embeddings)} face frames, "
        f"voice={'yes' if enrol.voice_embedding else 'no'} -> {out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
