/**
 * EVM address intelligence. From the RPC alone: code present, balance, nonce, ERC-20 symbol/decimals, and
 * a contract's creation age by probing `eth_getCode` at older blocks (bounded: two bucket probes, a short
 * binary search inside the youngest bucket, then one timestamp). With an Etherscan-compatible key the
 * explorer gives creation time and source verification directly. Cached; best-effort; never blocks a
 * decision for more than `timeoutMs`.
 */
import { decodeFunctionResult, encodeFunctionData, getAddress, type Address, type Hex } from "viem";
import type { ChainAddressIntel, ChainIntel, ChainTokenMeta } from "../types.js";
import { ERC20, KNOWN_TOKENS } from "./abi.js";
import type { EvmRpc } from "./rpc.js";

const TTL_MS = 5 * 60_000;
const DAY = 86_400n;

export interface EvmIntelOptions {
  etherscanKey?: string;
  etherscanBase?: string;
  fetchImpl?: typeof fetch;
  timeoutMs?: number;
  now?: () => number;
}

export class EvmIntel implements ChainIntel {
  private addr = new Map<string, ChainAddressIntel>();
  private tokens = new Map<string, ChainTokenMeta | null>();
  private readonly timeoutMs: number;
  private readonly fetchImpl: typeof fetch;
  private readonly now: () => number;
  constructor(readonly rpc: EvmRpc, private opts: EvmIntelOptions = {}) {
    this.timeoutMs = opts.timeoutMs ?? 4000;
    this.fetchImpl = opts.fetchImpl ?? fetch;
    this.now = opts.now ?? Date.now;
  }

  private withTimeout<T>(p: Promise<T>, fallback: T): Promise<T> {
    return Promise.race([p, new Promise<T>((r) => setTimeout(() => r(fallback), this.timeoutMs))]).catch(() => fallback);
  }

  private async explorer(params: Record<string, string>): Promise<any> {
    if (!this.opts.etherscanKey) return null;
    const base = this.opts.etherscanBase ?? "https://api.etherscan.io/v2/api";
    const u = new URL(base);
    u.searchParams.set("chainid", String(this.rpc.chainId));
    for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v);
    u.searchParams.set("apikey", this.opts.etherscanKey);
    const r = await this.fetchImpl(u.toString(), { signal: AbortSignal.timeout(this.timeoutMs) });
    if (!r.ok) return null;
    const j: any = await r.json();
    return j?.status === "1" ? j.result : null;
  }

  /** Days since a contract appeared, by bisecting eth_getCode over block numbers. Bounded to ~20 RPC calls. */
  private async contractAgeByProbe(a: Address): Promise<number | null> {
    const latest = await this.rpc.blockNumber();
    const span = latest > 10_000n ? 10_000n : latest;
    if (span === 0n) return null;
    const [tLatest, tOld] = await Promise.all([this.rpc.blockTimestamp(latest), this.rpc.blockTimestamp(latest - span)]);
    const secPerBlock = Number(tLatest - tOld) / Number(span);
    if (!(secPerBlock > 0)) return null;
    const blocksFor = (days: number) => BigInt(Math.floor((days * 86_400) / secPerBlock));
    const hasCode = async (b: bigint) => (await this.rpc.getCode(a, b)) !== "0x";
    // buckets: older than 30 days → "30+", older than 3 days → bisect inside [−30d, −3d] coarsely; else bisect [−3d, latest]
    const b30 = latest > blocksFor(30) ? latest - blocksFor(30) : 0n;
    if (await hasCode(b30)) return 30;
    const b3 = latest > blocksFor(3) ? latest - blocksFor(3) : 0n;
    let lo: bigint, hi: bigint;
    if (await hasCode(b3)) { lo = b30; hi = b3; } else { lo = b3; hi = latest; }
    // invariant: no code at lo (or lo is genesis), code at hi
    for (let i = 0; i < 16 && hi - lo > 1n; i++) {
      const mid = (lo + hi) / 2n;
      if (await hasCode(mid)) hi = mid; else lo = mid;
    }
    const tCreated = await this.rpc.blockTimestamp(hi);
    return Number((BigInt(Math.floor(this.now() / 1000)) - tCreated)) / Number(DAY);
  }

  async address(raw: string): Promise<ChainAddressIntel> {
    const a = getAddress(raw);
    const key = a.toLowerCase();
    const hit = this.addr.get(key);
    if (hit && this.now() - hit.fetchedAt < TTL_MS) return hit;
    const [code, balance, nonce] = await Promise.all([
      this.withTimeout(this.rpc.getCode(a), null as Hex | null),
      this.withTimeout(this.rpc.getBalance(a), null as bigint | null),
      this.withTimeout(this.rpc.transactionCount(a), null as number | null),
    ]);
    const isContract = !!code && code !== "0x";
    let ageDays: number | null = null;
    let verified: boolean | null = null;
    if (isContract) {
      const creation = await this.withTimeout(this.explorer({ module: "contract", action: "getcontractcreation", contractaddresses: a }), null);
      const ts = Array.isArray(creation) ? Number(creation[0]?.timestamp ?? 0) : 0;
      if (ts > 0) ageDays = (this.now() / 1000 - ts) / 86_400;
      else ageDays = await this.withTimeout(this.contractAgeByProbe(a), null);
      if (this.opts.etherscanKey) {
        const src = await this.withTimeout(this.explorer({ module: "contract", action: "getsourcecode", address: a }), null);
        verified = Array.isArray(src) ? !!src[0]?.SourceCode : null;
      }
    } else if (this.opts.etherscanKey) {
      const first = await this.withTimeout(this.explorer({ module: "account", action: "txlist", address: a, page: "1", offset: "1", sort: "asc" }), null);
      const ts = Array.isArray(first) ? Number(first[0]?.timeStamp ?? 0) : 0;
      if (ts > 0) ageDays = (this.now() / 1000 - ts) / 86_400;
    }
    const out: ChainAddressIntel = {
      address: a,
      exists: isContract || (balance ?? 0n) > 0n || (nonce ?? 0) > 0,
      isContract,
      ageDays,
      txCount: nonce ?? 0,
      txCountIsLowerBound: true, // nonce counts outgoing transactions only
      balance,
      verified,
      fetchedAt: this.now(),
    };
    this.addr.set(key, out);
    return out;
  }

  async token(raw: string): Promise<ChainTokenMeta | null> {
    const a = getAddress(raw);
    const key = a.toLowerCase();
    const known = KNOWN_TOKENS[this.rpc.chainId]?.[key];
    if (known) return known;
    if (this.tokens.has(key)) return this.tokens.get(key)!;
    const read = async <T,>(fn: "symbol" | "decimals"): Promise<T | null> => {
      const data = encodeFunctionData({ abi: ERC20, functionName: fn });
      const r = await this.withTimeout(this.rpc.call(a, data), null as Hex | null);
      if (!r || r === "0x") return null;
      try { return decodeFunctionResult({ abi: ERC20, functionName: fn, data: r }) as T; } catch { return null; }
    };
    const [symbol, decimals] = await Promise.all([read<string>("symbol"), read<number>("decimals")]);
    const out = symbol == null && decimals == null ? null : { symbol: symbol ?? undefined, decimals: decimals ?? undefined };
    this.tokens.set(key, out);
    return out;
  }

  async ownerBalance(owner: string): Promise<bigint | null> {
    return this.withTimeout(this.rpc.getBalance(getAddress(owner)), null);
  }

  /** ERC-20 allowance(owner, spender), for the swap module's exact-amount approvals. */
  async allowance(token: string, owner: string, spender: string): Promise<bigint | null> {
    const data = encodeFunctionData({ abi: ERC20, functionName: "allowance", args: [getAddress(owner), getAddress(spender)] });
    const r = await this.withTimeout(this.rpc.call(getAddress(token), data), null as Hex | null);
    if (!r || r === "0x") return null;
    try { return decodeFunctionResult({ abi: ERC20, functionName: "allowance", data: r }) as bigint; } catch { return null; }
  }

  /** ERC-20 balanceOf(owner). */
  async tokenBalance(token: string, owner: string): Promise<bigint | null> {
    const data = encodeFunctionData({ abi: ERC20, functionName: "balanceOf", args: [getAddress(owner)] });
    const r = await this.withTimeout(this.rpc.call(getAddress(token), data), null as Hex | null);
    if (!r || r === "0x") return null;
    try { return decodeFunctionResult({ abi: ERC20, functionName: "balanceOf", data: r }) as bigint; } catch { return null; }
  }

  /** Test seam. */
  seed(entries: Partial<ChainAddressIntel>[]) {
    for (const e of entries) this.addr.set(e.address!.toLowerCase(), { exists: true, isContract: false, ageDays: null, txCount: 0, txCountIsLowerBound: true, balance: 0n, verified: null, fetchedAt: this.now(), ...e } as ChainAddressIntel);
  }
}
