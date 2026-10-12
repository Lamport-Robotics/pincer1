/**
 * The EVM chains the robot signs for. The daemon signs any chainId it is given; this list is
 * what the extension lets dapps switch to (anything else → EIP-1193 error 4902) and where the
 * default, dapp-independent RPC for each lives. The popup can override every RPC URL.
 */

export interface EvmChain {
  /** Decimal chain id. */
  id: number;
  name: string;
  /** Short symbol dapps show (`wallet_addEthereumChain` shape). */
  nativeCurrency: { name: string; symbol: string; decimals: 18 };
  defaultRpcUrl: string;
  blockExplorerUrl: string;
}

export const EVM_CHAINS: readonly EvmChain[] = [
  {
    id: 1,
    name: 'Ethereum',
    nativeCurrency: { name: 'Ether', symbol: 'ETH', decimals: 18 },
    defaultRpcUrl: 'https://cloudflare-eth.com',
    blockExplorerUrl: 'https://etherscan.io',
  },
  {
    id: 8453,
    name: 'Base',
    nativeCurrency: { name: 'Ether', symbol: 'ETH', decimals: 18 },
    defaultRpcUrl: 'https://mainnet.base.org',
    blockExplorerUrl: 'https://basescan.org',
  },
  {
    id: 42161,
    name: 'Arbitrum One',
    nativeCurrency: { name: 'Ether', symbol: 'ETH', decimals: 18 },
    defaultRpcUrl: 'https://arb1.arbitrum.io/rpc',
    blockExplorerUrl: 'https://arbiscan.io',
  },
  {
    id: 56,
    name: 'BNB Smart Chain',
    nativeCurrency: { name: 'BNB', symbol: 'BNB', decimals: 18 },
    defaultRpcUrl: 'https://bsc-dataseed.binance.org',
    blockExplorerUrl: 'https://bscscan.com',
  },
  {
    id: 137,
    name: 'Polygon',
    nativeCurrency: { name: 'POL', symbol: 'POL', decimals: 18 },
    defaultRpcUrl: 'https://polygon-rpc.com',
    blockExplorerUrl: 'https://polygonscan.com',
  },
];

export const DEFAULT_EVM_CHAIN_ID = 1;

export function findEvmChain(id: number): EvmChain | undefined {
  return EVM_CHAINS.find((c) => c.id === id);
}

/** `"0x1"` → 1, `"1"` → 1, 1 → 1; NaN for junk. */
export function parseChainId(v: unknown): number {
  if (typeof v === 'number') return Number.isInteger(v) && v > 0 ? v : NaN;
  if (typeof v !== 'string' || !v) return NaN;
  const n = /^0x[0-9a-fA-F]+$/.test(v) ? Number.parseInt(v, 16) : /^\d+$/.test(v) ? Number.parseInt(v, 10) : NaN;
  return Number.isSafeInteger(n) && n > 0 ? n : NaN;
}

export function chainIdHex(id: number): string {
  return `0x${id.toString(16)}`;
}

/**
 * JSON-RPC methods a dapp may send straight through to the chain RPC. Everything that needs a
 * key goes to the robot; everything else is refused with 4200 so a dapp cannot use the wallet as
 * an open proxy to arbitrary methods (admin_, debug_, …).
 */
export const EVM_READONLY_METHODS: ReadonlySet<string> = new Set([
  'eth_blockNumber',
  'eth_call',
  'eth_estimateGas',
  'eth_feeHistory',
  'eth_gasPrice',
  'eth_getBalance',
  'eth_getBlockByHash',
  'eth_getBlockByNumber',
  'eth_getBlockTransactionCountByHash',
  'eth_getBlockTransactionCountByNumber',
  'eth_getCode',
  'eth_getLogs',
  'eth_getProof',
  'eth_getStorageAt',
  'eth_getTransactionByBlockHashAndIndex',
  'eth_getTransactionByBlockNumberAndIndex',
  'eth_getTransactionByHash',
  'eth_getTransactionCount',
  'eth_getTransactionReceipt',
  'eth_getUncleCountByBlockHash',
  'eth_getUncleCountByBlockNumber',
  'eth_maxPriorityFeePerGas',
  'eth_sendRawTransaction',
  'eth_syncing',
  'eth_createAccessList',
  'eth_getFilterChanges',
  'eth_getFilterLogs',
  'eth_newBlockFilter',
  'eth_newFilter',
  'eth_newPendingTransactionFilter',
  'eth_uninstallFilter',
  'web3_clientVersion',
  'web3_sha3',
]);
