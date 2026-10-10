/**
 * Serial (USB CDC) client for the real consent controller. Newline-delimited JSON, request ids,
 * unsolicited events. See docs/PROTOCOL.md §1.
 */
import { SerialPort } from "serialport";
import type { Addresses, Allowance, AnyClaims, Claims, ControllerClient, ControllerEvent, DenyReason, Hello, InheritanceStatus, PolicyChain, SignKind, PendingHook } from "./types.js";

type Waiter = { resolve: (m: any) => void; reject: (e: Error) => void; onPending?: PendingHook };

export class SerialController implements ControllerClient {
  private port: SerialPort;
  private buf = "";
  private waiters = new Map<string, Waiter>();
  private listeners: ((e: ControllerEvent) => void)[] = [];
  private seq = 0;
  private pend: { id: string; need: "touch" | "long_touch" | "pin" } | null = null;
  private lastHello: Hello | null = null;

  constructor(path: string, baud = 115200) {
    this.port = new SerialPort({ path, baudRate: baud, autoOpen: false });
    this.port.on("data", (d: Buffer) => this.onData(d));
    this.port.on("error", (e) => { for (const w of this.waiters.values()) w.reject(e); this.waiters.clear(); });
  }

  async open() {
    await new Promise<void>((res, rej) => this.port.open((e) => (e ? rej(e) : res())));
    await this.time(Math.floor(Date.now() / 1000));
  }

  private onData(d: Buffer) {
    this.buf += d.toString("utf8");
    let i;
    while ((i = this.buf.indexOf("\n")) >= 0) {
      const line = this.buf.slice(0, i).trim();
      this.buf = this.buf.slice(i + 1);
      if (!line) continue;
      let m: any;
      try { m = JSON.parse(line); } catch { continue; }
      if (m.t === "event") { for (const l of this.listeners) l(m); continue; }
      if (m.t === "pending" && m.id) { this.pend = { id: m.id, need: m.need }; this.waiters.get(m.id)?.onPending?.({ need: m.need, parsed: m.parsed }); continue; }
      if (m.id && this.waiters.has(m.id)) {
        if (this.pend?.id === m.id) this.pend = null;
        const w = this.waiters.get(m.id)!; this.waiters.delete(m.id); w.resolve(m);
      } else if (m.t === "hello") {
        this.waiters.get("hello")?.resolve(m); this.waiters.delete("hello");
      } else if (m.t === "locked") {
        this.waiters.get("lock")?.resolve(m); this.waiters.delete("lock");
      } else if (m.t === "inheritance_status") {
        this.waiters.get("inheritance_status")?.resolve(m); this.waiters.delete("inheritance_status");
      }
    }
  }

  private send(obj: Record<string, unknown>) {
    return new Promise<void>((res, rej) => this.port.write(JSON.stringify(obj) + "\n", (e) => (e ? rej(e) : res())));
  }

  private request<T = any>(obj: Record<string, unknown>, key?: string, timeoutMs = 90_000, onPending?: Waiter["onPending"]): Promise<T> {
    const id = key ?? `r${++this.seq}`;
    const msg = key ? obj : { ...obj, id };
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => { this.waiters.delete(id); reject(new Error("controller timeout")); }, timeoutMs);
      this.waiters.set(id, { resolve: (m) => { clearTimeout(timer); resolve(m); }, reject: (e) => { clearTimeout(timer); reject(e); }, onPending });
      this.send(msg).catch((e) => { clearTimeout(timer); this.waiters.delete(id); reject(e); });
    });
  }

  onEvent(cb: (e: ControllerEvent) => void) { this.listeners.push(cb); }
  pending() { return this.pend; }

  async time(unix: number) { await this.send({ t: "time", unix }); }

  async hello(): Promise<Hello> {
    const h = await this.request<Hello>({ t: "hello" }, "hello", 5000);
    // firmware without the multi-chain extension: expose Solana only
    if (!h.addresses && h.pubkey) h.addresses = { solana: h.pubkey };
    this.lastHello = h;
    return h;
  }

  async init() {
    // The words are shown on the controller's own display and never cross this link.
    const m = await this.request<{ pubkey: string; addresses?: Addresses; decoy_addresses?: Addresses; t: string }>({ t: "init" }, undefined, 15 * 60_000);
    if (m.t !== "init") throw new Error("init failed: " + JSON.stringify(m));
    return { pubkey: m.pubkey, addresses: m.addresses, decoy_addresses: m.decoy_addresses };
  }

  async setPin() { const m = await this.request<any>({ t: "set_pin" }, undefined, 180_000); return !!m.ok || m.t === "pin_set"; }

  /** For `btc_psbt` the controller answers with `psbt` (the PSBT carrying its partial signatures); it is returned in `sig`. */
  async sign(kind: SignKind, bytes: Uint8Array, claims: AnyClaims, ttlMs = 60_000, onPending?: Waiter["onPending"]) {
    const m = await this.request<any>({ t: "sign", kind, bytes: Buffer.from(bytes).toString("base64"), claims, ttl_ms: ttlMs }, undefined, ttlMs + 10_000, onPending);
    if (m.t === "signed" && (m.sig || m.psbt)) return { ok: true as const, sig: new Uint8Array(Buffer.from(m.sig ?? m.psbt, "base64")) };
    return { ok: false as const, reason: (m.reason ?? "policy") as DenyReason };
  }

  async inheritanceSet(bytes: Uint8Array, claims: Claims, cfg: { inactivityDays: number; warnDays: number[]; heir: string }) {
    const m = await this.request<any>({ t: "inheritance_set", bytes: Buffer.from(bytes).toString("base64"), claims, inactivity_days: cfg.inactivityDays, warn_days: cfg.warnDays, heir: cfg.heir }, undefined, 120_000);
    if (m.ok && m.sig) return { ok: true as const, sig: new Uint8Array(Buffer.from(m.sig, "base64")) };
    return { ok: false as const, reason: (m.reason ?? "policy") as DenyReason };
  }
  async inheritanceStatus(): Promise<InheritanceStatus> {
    const m = await this.request<any>({ t: "inheritance_status" }, "inheritance_status", 5000);
    return { armed: !!m.armed, inactivity_days: m.inactivity_days ?? 0, days_since_touch: m.days_since_touch ?? 0, powered_days: m.powered_days, heir: m.heir ?? null, warned: m.warned ?? [] };
  }
  async inheritanceRelease() {
    const m = await this.request<any>({ t: "inheritance_release" });
    if (m.tx) return { ok: true as const, tx: new Uint8Array(Buffer.from(m.tx, "base64")) };
    return { ok: false as const, reason: (m.reason === "not_armed" ? "not_armed" : "not_expired") as "not_expired" | "not_armed" };
  }
  async inheritanceClear() { const m = await this.request<any>({ t: "inheritance_clear" }, undefined, 120_000); return !!m.ok; }

  async elevate(limitLamports: bigint) { const m = await this.request<any>({ t: "elevate", limit_lamports: limitLamports.toString() }); return m.t === "elevated"; }
  async allowance(a: Omit<Allowance, "spent"> | { revoke: true }) {
    const m = await this.request<any>("revoke" in a ? { t: "allowance", revoke: true } : { t: "allowance", ...a });
    return !!m.ok;
  }
  async lock() { await this.request({ t: "lock" }, "lock", 5000); }
  async unlock() { const m = await this.request<any>({ t: "unlock" }); return m.t === "unlocked"; }
  async duress() { await this.request({ t: "duress" }); }
  async setDuressPin() { const m = await this.request<any>({ t: "set_duress_pin" }, undefined, 180_000); return m.ok ? { ok: true as const } : { ok: false as const, reason: (m.reason ?? "policy") as DenyReason }; }
  async decoyAddresses() { const m = await this.request<any>({ t: "decoy_addresses" }, undefined, 120_000); return m.t === "decoy_addresses" && m.addresses ? { ok: true as const, addresses: m.addresses as Addresses } : { ok: false as const, reason: (m.reason ?? "policy") as DenyReason }; }
  async setPolicy(dailyLimitLamports: bigint) { const m = await this.request<any>({ t: "policy", daily_limit_lamports: dailyLimitLamports.toString() }); return !!m.ok; }
  async setChainPolicy(chain: Exclude<PolicyChain, "solana">, limit: bigint) {
    const m = await this.request<any>(chain === "evm" ? { t: "policy", daily_limit_wei: limit.toString() } : { t: "policy", daily_limit_sats: limit.toString() });
    return !!m.ok;
  }
  async close() { await new Promise<void>((r) => this.port.close(() => r())); }
  get info() { return this.lastHello; }
}
