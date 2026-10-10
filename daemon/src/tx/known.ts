/** Program ids the explainer and the risk filter know by name. */
export const SYSTEM = "11111111111111111111111111111111";
export const TOKEN = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
export const TOKEN_2022 = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb";
export const ATA = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL";
export const COMPUTE_BUDGET = "ComputeBudget111111111111111111111111111111";
export const MEMO = "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr";
export const MEMO_V1 = "Memo1UhkJRfHyvLMcVucJwxXeuD728EqVDDwQDxFMNo";
export const JUPITER_V6 = "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4";
export const PUMP = "6EF8rrecthR5Dkzon8Nwu78hRvfCKubJ14M5uBEwF6P";
export const PUMPSWAP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA";
export const RAYDIUM_AMM = "675kPX9MHTjS2zt1qfr1NYHuzeLXfQM9H24wFSUt1Mp4";
export const RAYDIUM_CLMM = "CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTurDpMLyFkuzL";
export const ORCA_WHIRLPOOL = "whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc";
export const METEORA_DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo";
export const STAKE = "Stake11111111111111111111111111111111111111";
export const LOOKUP_TABLE = "AddressLookupTab1e1111111111111111111111111";
export const BPF_UPGRADEABLE = "BPFLoaderUpgradeab1e11111111111111111111111";

export const PROGRAM_NAMES: Record<string, string> = {
  [SYSTEM]: "System",
  [TOKEN]: "Token",
  [TOKEN_2022]: "Token-2022",
  [ATA]: "Associated Token Account",
  [COMPUTE_BUDGET]: "Compute Budget",
  [MEMO]: "Memo",
  [MEMO_V1]: "Memo",
  [JUPITER_V6]: "Jupiter",
  [PUMP]: "pump.fun",
  [PUMPSWAP]: "PumpSwap",
  [RAYDIUM_AMM]: "Raydium",
  [RAYDIUM_CLMM]: "Raydium CLMM",
  [ORCA_WHIRLPOOL]: "Orca",
  [METEORA_DLMM]: "Meteora",
  [STAKE]: "Stake",
  [LOOKUP_TABLE]: "Address Lookup Table",
};

/** Programs whose normal operation is understood well enough that invoking them is not itself suspicious. */
export const TRUSTED_PROGRAMS = new Set(Object.keys(PROGRAM_NAMES));

/** Well-known mints → symbol/decimals, for readable amounts without an RPC call. */
export const KNOWN_MINTS: Record<string, { symbol: string; decimals: number }> = {
  EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v: { symbol: "USDC", decimals: 6 },
  Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB: { symbol: "USDT", decimals: 6 },
  So11111111111111111111111111111111111111112: { symbol: "wSOL", decimals: 9 },
  mSoLzYCxHdYgdzU16g5QSh3i5K3z3KZK7ytfqcJm7So: { symbol: "mSOL", decimals: 9 },
  J1toso1uCk3RLmjorhTtrVwY9HJ7X8V9yYac6Y7kGCPn: { symbol: "JitoSOL", decimals: 9 },
  JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN: { symbol: "JUP", decimals: 6 },
};

export const U64_MAX = 18446744073709551615n;
