"""Corpus collector on the offline fixtures: licence filter, dedup, language, dry run."""

from __future__ import annotations

from pathlib import Path

from pincer_brain import ROOT
from pincer_brain.corpus.collect import (
    Deduper,
    chunk,
    collect,
    extract_comments,
    html_to_text,
    is_english,
    load_sources,
    shingles,
)

FIX = ROOT / "corpus" / "fixtures"


def _run(**kw):
    spec = load_sources(FIX / "sources.yaml")
    return collect(spec, FIX, **kw)


def test_statuses_and_counts():
    rep = _run()
    by = {s.name: s for s in rep.sources}
    docs = by["fixture-docs"]
    assert docs.status == "ok"
    assert docs.docs_in == 5
    assert docs.dropped_short == 1  # tiny.md
    assert docs.dropped_language == 1  # turkce.md
    assert docs.dropped_duplicate == 1  # address_poisoning_copy.md
    assert docs.docs_out == 2
    code = by["fixture-code"]
    assert code.status == "ok" and code.docs_out >= 2
    assert by["fixture-gpl"].status == "skipped_license"
    assert by["fixture-gpl"].docs_out == 0
    assert by["fixture-disabled"].status == "disabled"
    assert by["fixture-url"].status == "needs_fetch"
    assert all("copyleft" not in r["text"].lower() for r in rep.rows)
    assert {r["license"] for r in rep.rows} == {"MIT"}
    assert rep.est_tokens > 0


def test_dry_run_writes_no_rows():
    rep = _run(dry_run=True)
    assert rep.rows == []
    assert rep.total_chars > 0


def test_gpl_passes_other_filters_so_only_licence_keeps_it_out():
    text = (FIX / "gpl" / "copyleft.md").read_text()
    assert is_english(text) and len(text) >= 200


def test_language_filter():
    assert not is_english((FIX / "docs" / "turkce.md").read_text())
    assert is_english((FIX / "docs" / "durable_nonce.md").read_text())
    assert not is_english("短い日本語の文章です。" * 10)
    assert not is_english("one two three")


def test_dedup_near_duplicates():
    a = (FIX / "docs" / "address_poisoning.md").read_text()
    b = (FIX / "docs" / "address_poisoning_copy.md").read_text()
    c = (FIX / "docs" / "durable_nonce.md").read_text()
    d = Deduper(threshold=0.8)
    assert not d.is_duplicate(a)
    assert d.is_duplicate(b)
    assert d.is_duplicate(a)  # exact
    assert not d.is_duplicate(c)
    assert len(shingles("a b c d e f g")) == 3


def test_extract_comments():
    ts = (FIX / "src" / "filter.ts").read_text()
    blocks = extract_comments(ts, ".ts")
    assert any(b.startswith("The scam filter.") for b in blocks)
    assert any("hard blocks" in b and "touch" in b for b in blocks)
    assert not any("export function" in b for b in blocks)
    py = (FIX / "src" / "duress.py").read_text()
    blocks = extract_comments(py, ".py")
    assert any(b.startswith("The duress word is never stored") for b in blocks)
    assert any("pure check" in b for b in blocks)


def test_chunk_and_html():
    paras = "\n\n".join(f"paragraph {i} " + "x" * 100 for i in range(50))
    pieces = chunk(paras, max_chars=1000)
    assert len(pieces) > 1 and all(len(p) <= 1100 for p in pieces)
    txt = html_to_text(
        "<html><head><style>a{}</style></head><body><h1>Hi</h1><p>one &amp; two</p><script>x()</script></body></html>"
    )
    assert txt == "Hi\none & two"


def test_real_sources_file_is_sane():
    spec = load_sources(ROOT / "corpus" / "sources.yaml")
    names = [s["name"] for s in spec["sources"]]
    assert len(names) == len(set(names))
    allow = set(spec["license_allow"])
    assert "GPL-3.0" not in allow
    gpl = next(s for s in spec["sources"] if s["name"] == "solana-com-docs")
    assert gpl["license"] not in allow
    # every enabled source is either allowed or the deliberately listed GPL demonstration
    for s in spec["sources"]:
        if s.get("enabled", True) and s["license"] not in allow:
            assert s["name"] == "solana-com-docs", s["name"]
    assert any("ftc" in n or "cisa" in n or "ic3" in n for n in names)
    rep = collect(spec, ROOT / "corpus", dry_run=True)
    by = {s.name: s for s in rep.sources}
    assert by["pincer1-docs"].status == "ok"
    assert by["solana-com-docs"].status == "skipped_license"
    assert by["ftc-consumer-advice-scams"].status == "needs_fetch"
    assert rep.rows == []
    assert Path(FIX / "sources.yaml").exists()
