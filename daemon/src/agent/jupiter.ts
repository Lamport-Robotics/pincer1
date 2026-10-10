/** Jupiter swap via the public lite API. Returns an unsigned v0 transaction for the owner to sign. */
const LITE = "https://lite-api.jup.ag/swap/v1";

export interface Quote { inputMint: string; outputMint: string; inAmount: string; outAmount: string; slippageBps: number; [k: string]: unknown }

export async function jupiterQuote(inputMint: string, outputMint: string, amount: bigint, slippageBps = 150): Promise<Quote> {
  const u = `${LITE}/quote?inputMint=${inputMint}&outputMint=${outputMint}&amount=${amount}&slippageBps=${slippageBps}&restrictIntermediateTokens=true`;
  const r = await fetch(u);
  if (!r.ok) throw new Error(`jupiter quote ${r.status}: ${await r.text()}`);
  return (await r.json()) as Quote;
}

export async function jupiterSwapTx(quote: Quote, userPublicKey: string): Promise<string> {
  const r = await fetch(`${LITE}/swap`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ quoteResponse: quote, userPublicKey, wrapAndUnwrapSol: true, dynamicComputeUnitLimit: true, prioritizationFeeLamports: { priorityLevelWithMaxLamports: { maxLamports: 1_000_000, priorityLevel: "medium" } } }),
  });
  if (!r.ok) throw new Error(`jupiter swap ${r.status}: ${await r.text()}`);
  const j = (await r.json()) as { swapTransaction: string };
  return j.swapTransaction;
}
