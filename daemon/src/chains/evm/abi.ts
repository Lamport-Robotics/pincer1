/** Selectors, ABIs and constants the EVM parser recognises. Selectors are derived, never hand-typed. */
import { erc20Abi, maxUint256, parseAbi, toFunctionSelector, type Hex } from "viem";

export const UINT256_MAX = maxUint256;
export const UINT160_MAX = (1n << 160n) - 1n;
/** Anything at or above this is treated as "unlimited" (some dapps use 2^255 or 2^160−1 instead of 2^256−1). */
export const UNLIMITED_FLOOR = 1n << 159n;

export const ERC20 = erc20Abi;
export const ERC20_EXTRA = parseAbi([
  "function increaseAllowance(address spender, uint256 addedValue) returns (bool)",
  "function decreaseAllowance(address spender, uint256 subtractedValue) returns (bool)",
  "function setApprovalForAll(address operator, bool approved)",
  "function safeTransferFrom(address from, address to, uint256 tokenId)",
  "function safeTransferFrom(address from, address to, uint256 id, uint256 amount, bytes data)",
  "function permit(address owner, address spender, uint256 value, uint256 deadline, uint8 v, bytes32 r, bytes32 s)",
  "function permit(address holder, address spender, uint256 nonce, uint256 expiry, bool allowed, uint8 v, bytes32 r, bytes32 s)",
]);

/** Permit2 (0x000000000022D473030F116dDEE9F6B43aC78BA3) and router ABIs the swap verifier decodes. */
export const PERMIT2 = "0x000000000022D473030F116dDEE9F6B43aC78BA3";
export const PERMIT2_ABI = parseAbi([
  "function approve(address token, address spender, uint160 amount, uint48 expiration)",
  "function transferFrom(address from, address to, uint160 amount, address token)",
]);

export const SEL = {
  transfer: toFunctionSelector("transfer(address,uint256)"),
  approve: toFunctionSelector("approve(address,uint256)"),
  transferFrom: toFunctionSelector("transferFrom(address,address,uint256)"),
  setApprovalForAll: toFunctionSelector("setApprovalForAll(address,bool)"),
  increaseAllowance: toFunctionSelector("increaseAllowance(address,uint256)"),
  decreaseAllowance: toFunctionSelector("decreaseAllowance(address,uint256)"),
  safeTransferFrom721: toFunctionSelector("safeTransferFrom(address,address,uint256)"),
  safeTransferFrom1155: toFunctionSelector("safeTransferFrom(address,address,uint256,uint256,bytes)"),
  permit2612: toFunctionSelector("permit(address,address,uint256,uint256,uint8,bytes32,bytes32)"),
  permitDai: toFunctionSelector("permit(address,address,uint256,uint256,bool,uint8,bytes32,bytes32)"),
  permit2Approve: toFunctionSelector("approve(address,address,uint160,uint48)"),
  permit2TransferFrom: toFunctionSelector("transferFrom(address,address,uint160,address)"),
} as const satisfies Record<string, Hex>;

export const selectorOf = (data: Hex | undefined): Hex | null => (data && data.length >= 10 ? (data.slice(0, 10).toLowerCase() as Hex) : null);

/** Well-known tokens → symbol/decimals so amounts read well without an RPC call. Keys are lowercase. */
export const KNOWN_TOKENS: Record<number, Record<string, { symbol: string; decimals: number }>> = {
  1: {
    "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48": { symbol: "USDC", decimals: 6 },
    "0xdac17f958d2ee523a2206206994597c13d831ec7": { symbol: "USDT", decimals: 6 },
    "0xc02aaa39b223fe8d0a0e5c4f27ead9083c756cc2": { symbol: "WETH", decimals: 18 },
    "0x6b175474e89094c44da98b954eedeac495271d0f": { symbol: "DAI", decimals: 18 },
    "0x2260fac5e5542a773aa44fbcfedf7c193bc2c599": { symbol: "WBTC", decimals: 8 },
  },
  8453: {
    "0x833589fcd6edb6e08f4c7c32d4f71b54bda02913": { symbol: "USDC", decimals: 6 },
    "0x4200000000000000000000000000000000000006": { symbol: "WETH", decimals: 18 },
  },
  42161: {
    "0xaf88d065e77c8cc2239327c5edb3a432268e5831": { symbol: "USDC", decimals: 6 },
    "0xfd086bc7cd5c481dcc9c85ebe478a1c0b69fcbb9": { symbol: "USDT", decimals: 6 },
    "0x82af49447d8a07e3bd95bd0d56f35241523fbab1": { symbol: "WETH", decimals: 18 },
  },
  56: {
    "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d": { symbol: "USDC", decimals: 18 },
    "0x55d398326f99059ff775485246999027b3197955": { symbol: "USDT", decimals: 18 },
  },
  137: {
    "0x3c499c542cef5e3811e1192ce70d8cc03d5c3359": { symbol: "USDC", decimals: 6 },
    "0xc2132d05d31c914a87c6611c10748aeb04b58e8f": { symbol: "USDT", decimals: 6 },
  },
};
