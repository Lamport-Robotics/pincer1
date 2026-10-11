/**
 * Tiny JSON-RPC 2.0 client over fetch (EVM nodes) and an Esplora-style bitcoin broadcaster.
 * Both run in the service worker; `fetchImpl` is injectable for tests.
 */

export class RpcError extends Error {
  constructor(
    readonly code: number,
    message: string,
    readonly data?: unknown,
  ) {
    super(message);
    this.name = 'RpcError';
  }
}

type FetchLike = (input: string, init?: { method?: string; headers?: Record<string, string>; body?: string }) => Promise<{
  ok: boolean;
  status: number;
  text(): Promise<string>;
}>;

let rpcId = 0;

export async function jsonRpc(
  url: string,
  method: string,
  params: unknown[],
  fetchImpl: FetchLike = globalThis.fetch as FetchLike,
): Promise<unknown> {
  const body = JSON.stringify({ jsonrpc: '2.0', id: ++rpcId, method, params });
  let res: Awaited<ReturnType<FetchLike>>;
  try {
    res = await fetchImpl(url, { method: 'POST', headers: { 'content-type': 'application/json' }, body });
  } catch (e) {
    throw new RpcError(-32603, `RPC unreachable (${url}): ${(e as Error).message}`);
  }
  const text = await res.text();
  if (!res.ok) throw new RpcError(-32603, `RPC ${url} answered HTTP ${res.status}`);
  let parsed: { result?: unknown; error?: { code?: number; message?: string; data?: unknown } };
  try {
    parsed = JSON.parse(text) as typeof parsed;
  } catch {
    throw new RpcError(-32603, `RPC ${url} returned non-JSON`);
  }
  if (parsed.error) {
    throw new RpcError(
      typeof parsed.error.code === 'number' ? parsed.error.code : -32603,
      parsed.error.message || 'RPC error',
      parsed.error.data,
    );
  }
  return parsed.result;
}

/** `POST {apiBase}/tx` with the raw transaction hex; the body of the reply is the txid. */
export async function broadcastBitcoinTx(
  apiBase: string,
  txHex: string,
  fetchImpl: FetchLike = globalThis.fetch as FetchLike,
): Promise<string> {
  const url = `${apiBase.replace(/\/$/, '')}/tx`;
  let res: Awaited<ReturnType<FetchLike>>;
  try {
    res = await fetchImpl(url, { method: 'POST', headers: { 'content-type': 'text/plain' }, body: txHex });
  } catch (e) {
    throw new RpcError(-32603, `bitcoin API unreachable (${url}): ${(e as Error).message}`);
  }
  const text = (await res.text()).trim();
  if (!res.ok) throw new RpcError(-32603, `broadcast rejected: ${text || `HTTP ${res.status}`}`);
  if (!/^[0-9a-fA-F]{64}$/.test(text)) throw new RpcError(-32603, `bitcoin API returned no txid: ${text.slice(0, 80)}`);
  return text.toLowerCase();
}
