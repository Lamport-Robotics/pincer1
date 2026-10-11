"""Daemon-shaped facts and the sentences the daemon makes from them.

A straight port of ``daemon/src/tx/explain.ts`` (``explain``) and ``daemon/src/risk/filter.ts``
(``assess``) so that every transaction sample is voiced with the daemon's own words, numbers
and address shortening. If the TypeScript changes, change this file; ``tests/test_facts.py``
pins the sentences.
"""

from __future__ import annotations

import random
from dataclasses import dataclass, field

# ── known programs and mints (daemon/src/tx/known.ts) ───────────────────────────────────────
SYSTEM = "11111111111111111111111111111111"
TOKEN = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
TOKEN_2022 = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"
ATA = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"
COMPUTE_BUDGET = "ComputeBudget111111111111111111111111111111"
MEMO = "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr"
MEMO_V1 = "Memo1UhkJRfHyvLMcVucJwxXeuD728EqVDDwQDxFMNo"
JUPITER_V6 = "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4"
PUMP = "6EF8rrecthR5Dkzon8Nwu78hRvfCKubJ14M5uBEwF6P"
PUMPSWAP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
RAYDIUM_AMM = "675kPX9MHTjS2zt1qfr1NYHuzeLXfQM9H24wFSUt1Mp4"
RAYDIUM_CLMM = "CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTurDpMLyFkuzL"
ORCA_WHIRLPOOL = "whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc"
METEORA_DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
STAKE = "Stake11111111111111111111111111111111111111"
LOOKUP_TABLE = "AddressLookupTab1e1111111111111111111111111"

PROGRAM_NAMES: dict[str, str] = {
    SYSTEM: "System",
    TOKEN: "Token",
    TOKEN_2022: "Token-2022",
    ATA: "Associated Token Account",
    COMPUTE_BUDGET: "Compute Budget",
    MEMO: "Memo",
    MEMO_V1: "Memo",
    JUPITER_V6: "Jupiter",
    PUMP: "pump.fun",
    PUMPSWAP: "PumpSwap",
    RAYDIUM_AMM: "Raydium",
    RAYDIUM_CLMM: "Raydium CLMM",
    ORCA_WHIRLPOOL: "Orca",
    METEORA_DLMM: "Meteora",
    STAKE: "Stake",
    LOOKUP_TABLE: "Address Lookup Table",
}
TRUSTED_PROGRAMS = frozenset(PROGRAM_NAMES)
SWAP_PROGRAMS = frozenset(
    {JUPITER_V6, PUMP, PUMPSWAP, RAYDIUM_AMM, RAYDIUM_CLMM, ORCA_WHIRLPOOL, METEORA_DLMM}
)
PLUMBING = frozenset({SYSTEM, TOKEN, TOKEN_2022, ATA, COMPUTE_BUDGET, MEMO, MEMO_V1})

KNOWN_MINTS: dict[str, tuple[str, int]] = {
    "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v": ("USDC", 6),
    "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB": ("USDT", 6),
    "So11111111111111111111111111111111111111112": ("wSOL", 9),
    "mSoLzYCxHdYgdzU16g5QSh3i5K3z3KZK7ytfqcJm7So": ("mSOL", 9),
    "J1toso1uCk3RLmjorhTtrVwY9HJ7X8V9yYac6Y7kGCPn": ("JitoSOL", 9),
    "JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN": ("JUP", 6),
}
USDC = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
U64_MAX = 18446744073709551615
LAMPORTS = 1_000_000_000

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def pubkey(rng: random.Random) -> str:
    """A 44-character base58 string: the shape of a Solana public key."""
    return "".join(rng.choice(B58) for _ in range(44))


def lookalike_of(known: str, rng: random.Random) -> str:
    """Same first four and last four characters, different middle: address poisoning."""
    while True:
        mid = "".join(rng.choice(B58) for _ in range(len(known) - 8))
        cand = known[:4] + mid + known[-4:]
        if cand != known:
            return cand


# ── the parsed transaction and its context ───────────────────────────────────────────────────
@dataclass
class TokenMove:
    amount: int
    mint: str | None
    dest: str


@dataclass
class Danger:
    kind: str  # set_authority | approve | close_account | assign | lookup_table | nonce | burn
    detail: str
    to: str | None = None
    amount: int | None = None


@dataclass
class Intel:
    exists: bool = True
    age_days: float | None = None
    tx_count: int = 0
    tx_count_lower_bound: bool = False
    is_program: bool = False


@dataclass
class Tx:
    lamports_out: int = 0
    dest: str | None = None
    token_out: list[TokenMove] = field(default_factory=list)
    token_in: list[TokenMove] = field(default_factory=list)
    programs: list[str] = field(default_factory=list)
    dangers: list[Danger] = field(default_factory=list)
    uses_durable_nonce: bool = False
    memo: str | None = None


@dataclass
class Facts:
    tx: Tx
    owner: str
    origin: str | None = None
    intel: dict[str, Intel] = field(default_factory=dict)
    address_book: dict[str, str] = field(default_factory=dict)
    token_meta: dict[str, tuple[str, int]] = field(default_factory=dict)
    owner_lamports: int | None = None
    daily_limit_lamports: int | None = None
    origin_allowlist: list[str] | None = None
    seen_programs: set[str] = field(default_factory=set)


@dataclass
class Explanation:
    spoken: str
    lines: list[str]


@dataclass
class Verdict:
    level: str
    reasons: list[str]


# ── formatting (explain.ts) ──────────────────────────────────────────────────────────────────
def short(a: str, n: int = 4) -> str:
    return f"{a[:n]}…{a[-n:]}" if len(a) > 2 * n + 1 else a


def _locale(v: float, max_frac: int) -> str:
    s = f"{v:,.{max_frac}f}"
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return s


def fmt_sol(lamports: int) -> str:
    sol = lamports / 1e9
    if sol == 0:
        return "0 SOL"
    if sol < 0.001:
        return f"{lamports} lamports"
    return f"{_locale(sol, 4 if sol < 1 else 3)} SOL"


def fmt_token(amount: int, mint: str | None, meta: tuple[str, int] | None = None) -> str:
    known = KNOWN_MINTS.get(mint) if mint else None
    symbol, decimals = (None, None)
    if meta:
        symbol, decimals = meta
    if known:
        symbol = symbol or known[0]
        decimals = known[1] if decimals is None else decimals
    if symbol is None:
        symbol = f"token {short(mint)}" if mint else "tokens"
    if decimals is None:
        return f"{amount} units of {symbol}"
    v = amount / 10**decimals
    return f"{_locale(v, 6 if v < 1 else 2)} {symbol}"


def strip_origin(origin: str) -> str:
    for p in ("https://", "http://"):
        if origin.startswith(p):
            return origin[len(p) :]
    return origin


def explain(f: Facts) -> Explanation:
    p = f.tx
    lines: list[str] = []

    def who(a: str) -> str:
        label = f.address_book.get(a)
        if label:
            return label
        i = f.intel.get(a)
        if i and i.is_program:
            return f"program {short(a)}"
        if i and i.age_days is not None:
            if i.age_days < 1:
                age = "created today"
            elif i.age_days < 30:
                age = f"{int(i.age_days)} days old"
            else:
                age = f"{int(i.age_days / 30)} months old"
            plus = "+" if i.tx_count_lower_bound else ""
            return f"{short(a)}, a wallet {age} with {i.tx_count}{plus} transactions"
        return short(a)

    apps = [x for x in p.programs if x not in PLUMBING]
    app_names = [PROGRAM_NAMES.get(x, f"unknown program {short(x)}") for x in apps]

    if p.lamports_out > 0 and p.dest:
        n = "also " if p.token_out else ""
        lines.append(f"{n}sends {fmt_sol(p.lamports_out)} to {who(p.dest)}")
    for t in p.token_out:
        meta = f.token_meta.get(t.mint) if t.mint else None
        lines.append(f"sends {fmt_token(t.amount, t.mint, meta)} to {who(t.dest)}")
    for t in p.token_in:
        meta = f.token_meta.get(t.mint) if t.mint else None
        lines.append(f"receives {fmt_token(t.amount, t.mint, meta)}")
    if any(x in SWAP_PROGRAMS for x in apps) and p.lamports_out == 0 and not p.token_out:
        lines.append(f"swap through {' and '.join(app_names)}")
    elif apps:
        lines.append(f"interacts with {', '.join(app_names)}")
    for d in p.dangers:
        if d.kind == "set_authority":
            lines.append(f"DANGER: {d.detail}{f' to {who(d.to)}' if d.to else ''}")
        elif d.kind == "approve":
            lines.append(f"DANGER: {d.detail} to {who(d.to) if d.to else 'someone'}")
        elif d.kind in ("close_account", "assign"):
            lines.append(f"DANGER: {d.detail}")
        elif d.kind in ("lookup_table", "nonce"):
            lines.append(f"note: {d.detail}")
        elif d.kind == "burn":
            lines.append(f"burns tokens: {d.detail}")
    if p.uses_durable_nonce:
        lines.append("note: uses a durable nonce, so it can be broadcast later")
    if p.memo:
        lines.append(f'memo: "{p.memo}"')
    if not lines:
        lines.append("does nothing recognisable: no SOL or tokens leave your wallet")

    spoken_parts = [
        ln for ln in lines if not ln.startswith("note:") and not ln.startswith("memo:")
    ][:3]
    spoken = ". ".join(spoken_parts)
    spoken = spoken[:1].upper() + spoken[1:] + "."
    if f.origin:
        spoken = f"From {strip_origin(f.origin)}: {spoken}"
    return Explanation(spoken=spoken, lines=lines)


# ── the scam filter (filter.ts) ─────────────────────────────────────────────────────────────
_RANK = {"low": 0, "medium": 1, "high": 2, "block": 3}


def is_lookalike(candidate: str, known: str) -> bool:
    if candidate == known or len(candidate) < 32 or len(known) < 32:
        return False
    return candidate[:4] == known[:4] and candidate[-4:] == known[-4:]


def assess(f: Facts) -> Verdict:
    tx, owner = f.tx, f.owner
    reasons: list[str] = []
    level = "low"

    def raise_(to: str, why: str) -> None:
        nonlocal level
        reasons.append(why)
        if _RANK[to] > _RANK[level]:
            level = to

    book = f.address_book
    intel = f.intel.get

    for d in tx.dangers:
        if d.kind == "set_authority":
            raise_(
                "block",
                "it hands over control of your token account"
                f"{f' to {short(d.to)}' if d.to else ''}. That's how drainers work.",
            )
        if d.kind == "assign":
            raise_(
                "block",
                "it reassigns your wallet account to another program. "
                "Nothing legitimate does that.",
            )
        if d.kind == "close_account" and d.to and d.to != owner and d.to not in book:
            raise_("block", "it closes your token account and sends the rent to a stranger.")
        if d.kind == "approve":
            to_intel = intel(d.to) if d.to else None
            if d.amount == U64_MAX and d.to and not (to_intel and to_intel.is_program):
                raise_(
                    "block",
                    f"it gives {short(d.to)}, a plain wallet, unlimited permission to spend"
                    " your tokens.",
                )
            elif d.amount == U64_MAX:
                raise_("high", "it gives a program unlimited permission to spend your tokens.")
            else:
                raise_(
                    "medium",
                    "it delegates some of your tokens to "
                    f"{short(d.to) if d.to else 'another account'}.",
                )
        if d.kind == "lookup_table":
            raise_(
                "medium",
                "it uses address lookup tables I could not fully read, so part of it is opaque.",
            )
        if d.kind == "burn":
            raise_("medium", "it burns some of your tokens.")

    if tx.dest and tx.lamports_out > 0:
        dest = tx.dest
        i = intel(dest)
        in_book = dest in book
        for k, label in book.items():
            if is_lookalike(dest, k):
                raise_(
                    "block",
                    f'the destination looks like your contact "{label}" but is not the same'
                    " address. This is address poisoning.",
                )
        if not in_book and i:
            if not i.exists:
                raise_("high", "the destination has never been seen on chain.")
            elif i.age_days is not None and i.age_days < 3:
                age = "less than a day" if i.age_days < 1 else f"{int(i.age_days)} days"
                raise_("high", f"the destination wallet is {age} old.")
            elif i.tx_count < 3:
                raise_("high", "the destination wallet has almost no history.")
        if f.owner_lamports is not None and f.owner_lamports > 0:
            share = tx.lamports_out / f.owner_lamports
            if share > 0.9 and not in_book:
                raise_(
                    "block",
                    f"it empties your wallet: {round(share * 100)}% of your SOL to an address"
                    " not in your contacts.",
                )
            elif share > 0.5 and not in_book:
                raise_("high", f"it moves {round(share * 100)}% of your SOL.")
        if f.daily_limit_lamports is not None and tx.lamports_out > f.daily_limit_lamports:
            raise_("medium", "it is above your daily limit; I'll need a long touch.")

    for t in tx.token_out:
        i = intel(t.dest)
        if i and i.age_days is not None and i.age_days < 3 and t.dest not in book:
            when = "today" if i.age_days < 1 else f"{int(i.age_days)} days ago"
            raise_("high", f"tokens go to a wallet created {when}.")

    for prog in tx.programs:
        if prog in TRUSTED_PROGRAMS:
            continue
        i = intel(prog)
        seen = prog in f.seen_programs
        if i and not i.is_program:
            raise_(
                "block",
                f"{short(prog)} is invoked as a program but is not executable. The transaction"
                " is malformed or hostile.",
            )
        elif i and i.age_days is not None and i.age_days < 7:
            raise_(
                "high", f"it calls a program deployed {int(i.age_days)} days ago that I don't know."
            )
        elif not seen:
            raise_("medium", f"it calls {short(prog)}, a program you haven't used before.")

    if f.origin and f.origin_allowlist is not None:
        if not any(f.origin.startswith(o) for o in f.origin_allowlist) and (
            tx.lamports_out > LAMPORTS or tx.token_out
        ):
            raise_(
                "medium",
                f"the request comes from {strip_origin(f.origin)}, "
                "which is not on your trusted sites.",
            )

    if tx.uses_durable_nonce and f.origin:
        raise_(
            "high", "it uses a durable nonce, so whoever holds it can broadcast it any time later."
        )

    if not reasons:
        reasons.append("nothing unusual.")
    return Verdict(level=level, reasons=reasons)


def event_block(f: Facts, expl: Explanation, verdict: Verdict) -> str:
    """The sign-request event exactly as the model will see it in a user turn."""
    out = ["Event: sign_request"]
    if f.origin:
        out.append(f"origin: {strip_origin(f.origin)}")
    out.append("what it does:")
    out.extend(f"- {ln}" for ln in expl.lines)
    out.append(f"verdict: {verdict.level}")
    out.append("reasons:")
    out.extend(f"- {r}" for r in verdict.reasons)
    return "\n".join(out)
