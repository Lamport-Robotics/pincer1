/**
 * Address intelligence from the RPC: how old is this address, how active, is it a program, is the mint
 * authority still live. Cached in memory; every lookup is best-effort and never blocks a decision for
 * more than a few seconds.
 */
import { Connection, PublicKey } from "@solana/web3.js";

export interface AddressIntel {
  address: string;
  exists: boolean;
  isProgram: boolean;
  /** Days since the earliest signature we could see; null when unknown. */
  ageDays: number | null;
  txCount: number;
  /** true when we only saw the newest page (≥1000 signatures). */
  txCountIsLowerBound: boolean;
  lamports: number;
  fetchedAt: number;
}

export interface MintIntel {
  mint: string;
  decimals: number | null;
  mintAuthority: string | null;
  freezeAuthority: string | null;
  supply: bigint | null;
}

const TTL_MS = 5 * 60_000;

export class Intel {
  private addr = new Map<string, AddressIntel>();
  private mints = new Map<string, MintIntel>();
  constructor(private conn: Connection, private timeoutMs = 4000) {}

  private withTimeout<T>(p: Promise<T>, fallback: T): Promise<T> {
    return Promise.race([p, new Promise<T>((r) => setTimeout(() => r(fallback), this.timeoutMs))]).catch(() => fallback);
  }

  async address(a: string): Promise<AddressIntel> {
    const hit = this.addr.get(a);
    if (hit && Date.now() - hit.fetchedAt < TTL_MS) return hit;
    const pk = new PublicKey(a);
    const [info, sigs] = await Promise.all([
      this.withTimeout(this.conn.getAccountInfo(pk, "confirmed"), null),
      this.withTimeout(this.conn.getSignaturesForAddress(pk, { limit: 1000 }, "confirmed"), null),
    ]);
    const oldest = sigs && sigs.length ? sigs[sigs.length - 1].blockTime ?? null : null;
    const out: AddressIntel = {
      address: a,
      exists: !!info || (sigs?.length ?? 0) > 0,
      isProgram: !!info?.executable,
      ageDays: oldest ? (Date.now() / 1000 - oldest) / 86_400 : sigs ? null : null,
      txCount: sigs?.length ?? 0,
      txCountIsLowerBound: (sigs?.length ?? 0) >= 1000,
      lamports: info?.lamports ?? 0,
      fetchedAt: Date.now(),
    };
    this.addr.set(a, out);
    return out;
  }

  async mint(m: string): Promise<MintIntel> {
    const hit = this.mints.get(m);
    if (hit) return hit;
    const pk = new PublicKey(m);
    const info = await this.withTimeout(this.conn.getParsedAccountInfo(pk, "confirmed"), null);
    const data: any = info && info.value && "parsed" in (info.value.data as any) ? (info.value.data as any).parsed?.info : null;
    const out: MintIntel = {
      mint: m,
      decimals: data?.decimals ?? null,
      mintAuthority: data?.mintAuthority ?? null,
      freezeAuthority: data?.freezeAuthority ?? null,
      supply: data?.supply != null ? BigInt(data.supply) : null,
    };
    this.mints.set(m, out);
    return out;
  }

  async ownerBalance(owner: string): Promise<number> {
    return this.withTimeout(this.conn.getBalance(new PublicKey(owner), "confirmed"), -1);
  }

  /** Test seam: preload results without an RPC. */
  seed(entries: Partial<AddressIntel>[]) {
    for (const e of entries) {
      this.addr.set(e.address!, { exists: true, isProgram: false, ageDays: null, txCount: 0, txCountIsLowerBound: false, lamports: 0, fetchedAt: Date.now(), ...e } as AddressIntel);
    }
  }
}
