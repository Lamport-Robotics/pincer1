/**
 * The only places a swap may send funds. Static addresses for Jupiter, 1inch and 0x; THORChain's router and
 * inbound vaults rotate, so they are fetched live and pinned for ten minutes (a stale pin is refused, never
 * silently refreshed mid-swap).
 */
import { JUPITER_V6 } from "../tx/known.js";

export { JUPITER_V6 };
export const ONEINCH_V6_ROUTER = "0x111111125421cA6dc452d289314280a0f8842A65";
export const ZEROX_EXCHANGE_PROXY = "0xDef1C0ded9bec7F1a1670819833240f027b25EfF";

/** Static EVM router allowlist (same address on every supported chain for both aggregators). */
export const EVM_ROUTERS: readonly string[] = [ONEINCH_V6_ROUTER, ZEROX_EXCHANGE_PROXY];

export const ROUTER_NAMES: Record<string, string> = {
  [ONEINCH_V6_ROUTER.toLowerCase()]: "1inch",
  [ZEROX_EXCHANGE_PROXY.toLowerCase()]: "0x",
  [JUPITER_V6]: "Jupiter",
};

export const PIN_TTL_MS = 10 * 60_000;

export interface ThorInbound { chain: string; address: string; router?: string; halted: boolean; gasRate?: string; dustThreshold?: string }

/** Live THORChain inbound addresses, pinned. The EVM adapter's router set is extended with the pinned router. */
export class ThorPins {
  private pinned: { at: number; inbound: ThorInbound[] } | null = null;
  constructor(private base: string, private fetchImpl: typeof fetch = fetch, private now: () => number = Date.now, private onRouter?: (router: string) => void) {}

  async refresh(): Promise<ThorInbound[]> {
    const r = await this.fetchImpl(`${this.base.replace(/\/$/, "")}/thorchain/inbound_addresses`, { signal: AbortSignal.timeout(4000) });
    if (!r.ok) throw new Error(`thornode inbound_addresses ${r.status}`);
    const j = (await r.json()) as any[];
    const inbound: ThorInbound[] = j.filter((x) => x && typeof x.chain === "string" && typeof x.address === "string").map((x) => ({ chain: x.chain, address: x.address, router: typeof x.router === "string" && x.router ? x.router : undefined, halted: !!x.halted || !!x.global_trading_paused || !!x.chain_trading_paused, gasRate: x.gas_rate, dustThreshold: x.dust_threshold }));
    this.pinned = { at: this.now(), inbound };
    for (const i of inbound) if (i.router) this.onRouter?.(i.router);
    return inbound;
  }

  /** The pinned entry for a chain, refreshing when the pin is older than ten minutes. */
  async inbound(chain: string): Promise<ThorInbound> {
    if (!this.pinned || this.now() - this.pinned.at > PIN_TTL_MS) await this.refresh();
    const e = this.pinned!.inbound.find((i) => i.chain === chain);
    if (!e) throw new Error(`THORChain has no inbound address for ${chain}`);
    if (e.halted) throw new Error(`THORChain trading on ${chain} is halted`);
    return e;
  }

  /** Is this exact address pinned right now (and not expired)? Used when verifying the built transaction. */
  isPinned(address: string, chain?: string): boolean {
    if (!this.pinned || this.now() - this.pinned.at > PIN_TTL_MS) return false;
    return this.pinned.inbound.some((i) => (!chain || i.chain === chain) && (i.address.toLowerCase() === address.toLowerCase() || i.router?.toLowerCase() === address.toLowerCase()));
  }
  get pinnedAt() { return this.pinned?.at ?? 0; }
}
