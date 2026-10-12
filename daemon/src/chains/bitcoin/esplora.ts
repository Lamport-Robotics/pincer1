/**
 * Esplora-compatible API client (Blockstream, mempool.space, a self-hosted electrs). UTXOs, fee
 * estimates, address stats, broadcast. Every call has a 4 s timeout; fee hints are clamped to
 * `maxFeeRate` because a lying fee oracle must not be able to burn the owner's coins.
 */
export interface Utxo { txid: string; vout: number; value: number; confirmed: boolean }
export interface AddressStats { txCount: number; fundedTxoCount: number; spentTxoCount: number; balanceSats: number }

export interface EsploraOptions { timeoutMs?: number; maxFeeRate?: number; fetchImpl?: typeof fetch }

export class Esplora {
  readonly base: string;
  readonly timeoutMs: number;
  readonly maxFeeRate: number;
  private fetchImpl: typeof fetch;
  constructor(base = "https://blockstream.info/api", opts: EsploraOptions = {}) {
    this.base = base.replace(/\/$/, "");
    this.timeoutMs = opts.timeoutMs ?? 4000;
    this.maxFeeRate = opts.maxFeeRate ?? 200;
    this.fetchImpl = opts.fetchImpl ?? fetch;
  }

  private async get<T>(path: string): Promise<T> {
    const r = await this.fetchImpl(`${this.base}${path}`, { signal: AbortSignal.timeout(this.timeoutMs) });
    if (!r.ok) throw new Error(`esplora ${path} ${r.status}`);
    return (await r.json()) as T;
  }

  async utxos(address: string): Promise<Utxo[]> {
    const raw = await this.get<{ txid: string; vout: number; value: number; status: { confirmed: boolean } }[]>(`/address/${address}/utxo`);
    return raw.filter((u) => typeof u.txid === "string" && Number.isInteger(u.vout) && Number.isInteger(u.value) && u.value > 0).map((u) => ({ txid: u.txid, vout: u.vout, value: u.value, confirmed: !!u.status?.confirmed }));
  }

  async stats(address: string): Promise<AddressStats> {
    const j = await this.get<{ chain_stats: { funded_txo_count: number; spent_txo_count: number; funded_txo_sum: number; spent_txo_sum: number; tx_count: number } }>(`/address/${address}`);
    const c = j.chain_stats;
    return { txCount: c.tx_count, fundedTxoCount: c.funded_txo_count, spentTxoCount: c.spent_txo_count, balanceSats: c.funded_txo_sum - c.spent_txo_sum };
  }

  /** sat/vB for the target confirmation window, clamped to [1, maxFeeRate]. Falls back to a modest rate when the oracle is unreachable. */
  async feeRate(targetBlocks = 3): Promise<{ rate: number; clamped: boolean; source: "oracle" | "fallback" }> {
    let est: Record<string, number> | null = null;
    try { est = await this.get<Record<string, number>>("/fee-estimates"); } catch { est = null; }
    if (!est) return { rate: Math.min(10, this.maxFeeRate), clamped: false, source: "fallback" };
    const keys = Object.keys(est).map(Number).filter((k) => k >= targetBlocks).sort((a, b) => a - b);
    const raw = keys.length ? est[String(keys[0])] : est[String(Math.max(...Object.keys(est).map(Number)))];
    const hint = Number.isFinite(raw) && raw > 0 ? Math.ceil(raw) : 10;
    const rate = Math.max(1, Math.min(hint, this.maxFeeRate));
    return { rate, clamped: hint > this.maxFeeRate, source: "oracle" };
  }

  async broadcast(txHex: string): Promise<string> {
    const r = await this.fetchImpl(`${this.base}/tx`, { method: "POST", body: txHex, headers: { "content-type": "text/plain" }, signal: AbortSignal.timeout(this.timeoutMs) });
    const text = await r.text();
    if (!r.ok) throw new Error(`esplora broadcast ${r.status}: ${text.slice(0, 200)}`);
    return text.trim();
  }
}
