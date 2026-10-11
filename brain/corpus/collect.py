"""Build the continued-pretraining corpus from ``sources.yaml``.

Stages, in order, for every enabled source whose licence is on the allowlist:

  1. read    repo globs (docs as text, code as comments/docstrings) or URLs (only with --fetch)
  2. chunk   paragraphs packed to ~4000 characters
  3. filter  minimum length, English heuristic
  4. dedup   exact (hash) then near-duplicate (MinHash, 128 permutations, LSH 16x8, Jaccard >= 0.8)
  5. write   JSONL rows {text, source, license, origin, chars, est_tokens}

Nothing touches the network unless ``--fetch`` is given; ``--dry-run`` reports sizes and
writes nothing. ``corpus/fixtures/`` is a tiny offline source tree the tests run against.

    python -m pincer_brain.corpus.collect --dry-run
    python -m pincer_brain.corpus.collect --fetch --out corpus/out/cpt.jsonl
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import random
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

HERE = Path(__file__).resolve().parent
DEFAULT_SOURCES = HERE / "sources.yaml"
CHARS_PER_TOKEN = 4  # rough; the real count comes from the tokenizer at training time

# ── reading ─────────────────────────────────────────────────────────────────────────────────
_TEXT_EXT = {".md", ".txt", ".rst", ".mdx"}
_COMMENT_EXT = {".ts", ".tsx", ".js", ".c", ".h", ".cpp", ".rs", ".py"}


def extract_comments(text: str, ext: str) -> list[str]:
    """Comment blocks and docstrings as separate documents; code itself is dropped."""
    blocks: list[str] = []
    if ext == ".py":
        for m in re.finditer(r'(?:"""|\'\'\')(.*?)(?:"""|\'\'\')', text, re.DOTALL):
            blocks.append(m.group(1))
        run: list[str] = []
        for line in text.splitlines():
            s = line.strip()
            if s.startswith("#") and not s.startswith("#!"):
                run.append(s.lstrip("# ").rstrip())
            elif run:
                blocks.append("\n".join(run))
                run = []
        if run:
            blocks.append("\n".join(run))
    else:
        for m in re.finditer(r"/\*+(.*?)\*/", text, re.DOTALL):
            body = "\n".join(ln.strip().lstrip("*").strip() for ln in m.group(1).splitlines())
            blocks.append(body)
        run = []
        for line in text.splitlines():
            s = line.strip()
            if s.startswith("//"):
                run.append(s.lstrip("/ ").rstrip())
            elif run:
                blocks.append("\n".join(run))
                run = []
        if run:
            blocks.append("\n".join(run))
    return [b.strip() for b in blocks if b.strip()]


def html_to_text(raw: str) -> str:
    s = re.sub(r"(?is)<(script|style|nav|footer|header)\b.*?</\1>", " ", raw)
    s = re.sub(r"(?i)<br\s*/?>|</p>|</div>|</li>|</h[1-6]>|</tr>", "\n", s)
    s = re.sub(r"(?s)<[^>]+>", " ", s)
    s = html.unescape(s)
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r" *\n *", "\n", s)
    s = re.sub(r"\n{3,}", "\n\n", s)
    return s.strip()


def read_file(path: Path, extract: str) -> list[str]:
    ext = path.suffix.lower()
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    mode = extract
    if mode == "auto":
        mode = "comments" if ext in _COMMENT_EXT else "text"
    if mode == "comments":
        return extract_comments(text, ext)
    return [text]


def fetch_url(url: str, cache_dir: Path | None, timeout: float = 30.0) -> str:
    """GET one URL (explicitly enabled by ``--fetch``). Cached on disk by URL hash."""
    key = hashlib.sha256(url.encode()).hexdigest()[:24]
    if cache_dir:
        cached = cache_dir / f"{key}.txt"
        if cached.exists():
            return cached.read_text(encoding="utf-8")
    req = urllib.request.Request(url, headers={"User-Agent": "lamport-corpus/0.1 (+pincer1)"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (explicit --fetch)
        raw = resp.read().decode("utf-8", errors="replace")
        ctype = resp.headers.get("Content-Type", "")
    text = (
        html_to_text(raw)
        if "html" in ctype or raw.lstrip().lower().startswith(("<!doctype", "<html"))
        else raw
    )
    if cache_dir:
        cache_dir.mkdir(parents=True, exist_ok=True)
        (cache_dir / f"{key}.txt").write_text(text, encoding="utf-8")
    return text


# ── chunking and filters ────────────────────────────────────────────────────────────────────
def chunk(text: str, max_chars: int = 4000) -> list[str]:
    paras = [p.strip() for p in re.split(r"\n\s*\n", text) if p.strip()]
    out: list[str] = []
    cur: list[str] = []
    size = 0
    for p in paras:
        if size + len(p) > max_chars and cur:
            out.append("\n\n".join(cur))
            cur, size = [], 0
        cur.append(p)
        size += len(p) + 2
    if cur:
        out.append("\n\n".join(cur))
    return out


_STOPWORDS = frozenset(
    "the of and to in a is that for it as with on be this by are from or an at not which you "
    "your can will if we one all has have its their there when they any been use only".split()
)
_WORD = re.compile(r"[A-Za-z']+")


def is_english(text: str, min_words: int = 20, min_stop_ratio: float = 0.06) -> bool:
    """Latin script and a normal share of English function words. Code comments pass,
    Turkish and Chinese text do not."""
    letters = [c for c in text if c.isalpha()]
    if not letters:
        return False
    latin = sum(1 for c in letters if c.isascii())
    if latin / len(letters) < 0.9:
        return False
    words = [w.lower() for w in _WORD.findall(text)]
    if len(words) < min_words:
        return False
    stop = sum(1 for w in words if w in _STOPWORDS)
    return stop / len(words) >= min_stop_ratio


# ── MinHash near-duplicate detection ────────────────────────────────────────────────────────
_PRIME = (1 << 61) - 1
_MAX = (1 << 32) - 1


def shingles(text: str, k: int = 5) -> set[int]:
    words = re.findall(r"\w+", text.lower())
    if len(words) < k:
        return {hash_text(" ".join(words))} if words else set()
    return {hash_text(" ".join(words[i : i + k])) for i in range(len(words) - k + 1)}


def hash_text(s: str) -> int:
    return int.from_bytes(hashlib.blake2b(s.encode("utf-8"), digest_size=8).digest(), "big")


class MinHasher:
    def __init__(self, n_perm: int = 128, seed: int = 1) -> None:
        rng = random.Random(seed)
        self.n_perm = n_perm
        self.params = [
            (rng.randint(1, _PRIME - 1), rng.randint(0, _PRIME - 1)) for _ in range(n_perm)
        ]

    def signature(self, sh: set[int]) -> tuple[int, ...]:
        if not sh:
            return tuple([_MAX] * self.n_perm)
        sig = []
        for a, b in self.params:
            sig.append(min(((a * x + b) % _PRIME) & _MAX for x in sh))
        return tuple(sig)


def jaccard_estimate(a: tuple[int, ...], b: tuple[int, ...]) -> float:
    return sum(1 for x, y in zip(a, b, strict=True) if x == y) / len(a)


class Deduper:
    """Exact dedup by hash, then MinHash + LSH banding for near duplicates."""

    def __init__(
        self, threshold: float = 0.8, n_perm: int = 128, bands: int = 16, seed: int = 1
    ) -> None:
        self.threshold = threshold
        self.hasher = MinHasher(n_perm, seed)
        self.bands = bands
        self.rows = n_perm // bands
        self.exact: set[str] = set()
        self.buckets: dict[tuple[int, tuple[int, ...]], list[int]] = {}
        self.sigs: list[tuple[int, ...]] = []

    def is_duplicate(self, text: str) -> bool:
        norm = " ".join(text.split()).lower()
        h = hashlib.sha256(norm.encode()).hexdigest()
        if h in self.exact:
            return True
        self.exact.add(h)
        sig = self.hasher.signature(shingles(text))
        candidates: set[int] = set()
        keys = []
        for b in range(self.bands):
            key = (b, sig[b * self.rows : (b + 1) * self.rows])
            keys.append(key)
            candidates.update(self.buckets.get(key, ()))
        for idx in candidates:
            if jaccard_estimate(sig, self.sigs[idx]) >= self.threshold:
                return True
        idx = len(self.sigs)
        self.sigs.append(sig)
        for key in keys:
            self.buckets.setdefault(key, []).append(idx)
        return False


# ── the run ────────────────────────────────────────────────────────────────────────────────
@dataclass
class SourceStats:
    name: str
    license: str
    status: str  # ok | skipped_license | disabled | needs_fetch | error
    docs_in: int = 0
    docs_out: int = 0
    chars: int = 0
    dropped_short: int = 0
    dropped_language: int = 0
    dropped_duplicate: int = 0
    note: str = ""

    @property
    def est_tokens(self) -> int:
        return self.chars // CHARS_PER_TOKEN


@dataclass
class Report:
    sources: list[SourceStats] = field(default_factory=list)
    rows: list[dict[str, Any]] = field(default_factory=list)

    @property
    def total_chars(self) -> int:
        return sum(s.chars for s in self.sources)

    @property
    def est_tokens(self) -> int:
        return self.total_chars // CHARS_PER_TOKEN


def load_sources(path: Path = DEFAULT_SOURCES) -> dict[str, Any]:
    with Path(path).open() as f:
        spec = yaml.safe_load(f)
    spec.setdefault("license_allow", [])
    spec.setdefault("sources", [])
    return spec


def _iter_docs(
    src: dict[str, Any], base: Path, fetch: bool, cache_dir: Path | None
) -> tuple[list[tuple[str, str]], str]:
    """``[(origin, text)]`` plus a note. URL sources yield nothing without ``fetch``."""
    kind = src.get("kind", "repo")
    docs: list[tuple[str, str]] = []
    if kind == "repo":
        root = (base / src.get("root", ".")).resolve()
        extract = src.get("extract", "auto")
        seen: set[Path] = set()
        for pattern in src.get("globs", []):
            for path in sorted(root.glob(pattern)):
                if path.is_file() and path not in seen and ".venv" not in path.parts:
                    seen.add(path)
                    for text in read_file(path, extract):
                        docs.append((str(path.relative_to(root)), text))
        return docs, ""
    if kind == "url":
        if not fetch:
            return [], "needs --fetch"
        for url in src.get("urls", []):
            try:
                docs.append((url, fetch_url(url, cache_dir)))
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                print(f"  ! {url}: {exc}", file=sys.stderr)
        return docs, ""
    raise ValueError(f"unknown source kind {kind!r}")


def collect(
    spec: dict[str, Any],
    base: Path,
    *,
    fetch: bool = False,
    dry_run: bool = False,
    min_chars: int = 200,
    threshold: float = 0.8,
    cache_dir: Path | None = None,
) -> Report:
    allow = set(spec["license_allow"])
    report = Report()
    dedup = Deduper(threshold=threshold)
    for src in spec["sources"]:
        name = src["name"]
        lic = str(src.get("license", "unknown"))
        stats = SourceStats(name=name, license=lic, status="ok")
        report.sources.append(stats)
        if not src.get("enabled", True):
            stats.status, stats.note = "disabled", str(src.get("reason", ""))
            continue
        if lic not in allow:
            stats.status, stats.note = "skipped_license", f"{lic} not in license_allow"
            continue
        docs, note = _iter_docs(src, base, fetch, cache_dir)
        if note:
            stats.status, stats.note = "needs_fetch", note
            continue
        stats.docs_in = len(docs)
        for origin, text in docs:
            for piece in chunk(text):
                if len(piece) < min_chars:
                    stats.dropped_short += 1
                    continue
                if not is_english(piece):
                    stats.dropped_language += 1
                    continue
                if dedup.is_duplicate(piece):
                    stats.dropped_duplicate += 1
                    continue
                stats.docs_out += 1
                stats.chars += len(piece)
                if not dry_run:
                    report.rows.append(
                        {
                            "text": piece,
                            "source": name,
                            "license": lic,
                            "origin": origin,
                            "chars": len(piece),
                            "est_tokens": len(piece) // CHARS_PER_TOKEN,
                        }
                    )
    return report


def write_jsonl(rows: list[dict[str, Any]], out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


def print_report(report: Report, dry_run: bool) -> None:
    from rich.console import Console
    from rich.table import Table

    table = Table(title="corpus sources" + (" (dry run)" if dry_run else ""))
    for col in (
        "source",
        "licence",
        "status",
        "in",
        "out",
        "short",
        "lang",
        "dup",
        "chars",
        "~tokens",
        "note",
    ):
        table.add_column(
            col,
            justify="right"
            if col in ("in", "out", "short", "lang", "dup", "chars", "~tokens")
            else "left",
        )
    for s in report.sources:
        table.add_row(
            s.name,
            s.license,
            s.status,
            str(s.docs_in),
            str(s.docs_out),
            str(s.dropped_short),
            str(s.dropped_language),
            str(s.dropped_duplicate),
            f"{s.chars:,}",
            f"{s.est_tokens:,}",
            s.note,
        )
    Console(stderr=True, width=160).print(table)
    print(f"total: {report.total_chars:,} chars, ~{report.est_tokens:,} tokens", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--sources", type=Path, default=DEFAULT_SOURCES)
    ap.add_argument("--out", type=Path, default=HERE / "out" / "cpt.jsonl")
    ap.add_argument("--dry-run", action="store_true", help="report sizes, write nothing")
    ap.add_argument("--fetch", action="store_true", help="allow network: download URL sources")
    ap.add_argument("--cache-dir", type=Path, default=HERE / "cache")
    ap.add_argument("--min-chars", type=int, default=200)
    ap.add_argument("--threshold", type=float, default=0.8, help="MinHash Jaccard threshold")
    args = ap.parse_args(argv)
    spec = load_sources(args.sources)
    report = collect(
        spec,
        args.sources.parent,
        fetch=args.fetch,
        dry_run=args.dry_run,
        min_chars=args.min_chars,
        threshold=args.threshold,
        cache_dir=args.cache_dir,
    )
    print_report(report, args.dry_run)
    if not args.dry_run:
        write_jsonl(report.rows, args.out)
        print(f"wrote {len(report.rows)} rows -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
