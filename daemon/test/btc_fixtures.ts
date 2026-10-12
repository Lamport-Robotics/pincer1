/** A fake Esplora-compatible API for the Bitcoin and swap tests. */
import * as bitcoin from "bitcoinjs-lib";
import { Esplora } from "../src/chains/bitcoin/esplora.js";

/** Fake Esplora: utxos per address, a fee oracle (optionally lying), address stats, broadcast log. */
export function fakeEsplora(o: { utxos?: Record<string, { txid: string; vout: number; value: number; confirmed?: boolean }[]>; fee?: Record<string, number>; stats?: Record<string, { tx_count: number; funded: number; spent: number }>; maxFeeRate?: number } = {}) {
  const sent: string[] = [];
  const fetchImpl = (async (url: string | URL | Request, init?: RequestInit) => {
    const u = String(url);
    const path = u.replace(/^https?:\/\/[^/]+(\/api)?/, "");
    if (path === "/tx" && init?.method === "POST") { sent.push(String(init.body)); return new Response(bitcoin.Transaction.fromHex(String(init.body)).getId(), { status: 200 }); }
    if (path === "/fee-estimates") return new Response(JSON.stringify(o.fee ?? { "1": 12, "3": 8, "6": 5 }), { status: 200 });
    let m = /^\/address\/([^/]+)\/utxo$/.exec(path);
    if (m) return new Response(JSON.stringify((o.utxos?.[m[1]] ?? []).map((x) => ({ txid: x.txid, vout: x.vout, value: x.value, status: { confirmed: x.confirmed ?? true } }))), { status: 200 });
    m = /^\/address\/([^/]+)$/.exec(path);
    if (m) { const s = o.stats?.[m[1]] ?? { tx_count: 0, funded: 0, spent: 0 }; return new Response(JSON.stringify({ chain_stats: { tx_count: s.tx_count, funded_txo_count: s.tx_count, spent_txo_count: 0, funded_txo_sum: s.funded, spent_txo_sum: s.spent } }), { status: 200 }); }
    return new Response("not found", { status: 404 });
  }) as unknown as typeof fetch;
  return { esplora: new Esplora("https://esplora.test/api", { fetchImpl, maxFeeRate: o.maxFeeRate ?? 200, timeoutMs: 1000 }), sent };
}

