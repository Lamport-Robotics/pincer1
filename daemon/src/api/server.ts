/**
 * One port, two faces (docs/PROTOCOL.md §3 and §4):
 *   ws://  — the browser extension: pair / connect / signTransaction / signMessage (Solana, EVM, Bitcoin)
 *   http:// — local status, explain, swaps, agent controls, pairing-code display
 *
 * Pairing: the first connection must present the 6-digit code the robot spoke; it gets a token that is
 * stored on disk. Every later message must carry that token.
 */
import http from "node:http";
import { randomBytes, randomInt, timingSafeEqual } from "node:crypto";
import { readFileSync, writeFileSync, mkdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { WebSocketServer, type WebSocket } from "ws";
import type { Signer } from "../sign.js";
import type { ControllerClient } from "../controller/types.js";
import type { RobotLink } from "../presence/client.js";
import type { Agent } from "../agent/index.js";
import type { SettingsStore } from "../settings.js";
import type { Offline } from "../offline/index.js";
import type { Inheritance } from "../inheritance/index.js";
import type { Alerts } from "../alerts/index.js";
import type { Briefing } from "../briefing/index.js";
import type { SwapService } from "../swap/index.js";
import { SwapError } from "../swap/index.js";
import { SettingsError, isCloudBrain } from "../settings.js";
import type { ChainId } from "../chains/types.js";
import { nonceAccountCreateTx } from "../offline/index.js";
import type { Connection } from "@solana/web3.js";
import { log } from "../config.js";
import { flashFirmware, confirmFirmware, checkImage } from "../firmware.js";

export interface ServerDeps {
  signer: Signer;
  controller: ControllerClient;
  robot: RobotLink;
  agent: Agent | null;
  settings?: SettingsStore;
  offline?: Offline;
  inheritance?: Inheritance;
  alerts?: Alerts;
  briefing?: Briefing;
  swap?: SwapService;
  conn?: Connection;
  getOwner: () => string;
  dataDir: string;
  bind: string;
  port: number;
  /** Dev only: lets the HTTP API simulate a touch on the sim controller. */
  devTouch?: (long: boolean) => void;
  /** PINCER_MODE=prod: firmware updates need secure boot on the controller. */
  prod?: boolean;
}

interface Pairing { tokens: string[] }

class HttpError extends Error {
  readonly status: number;
  constructor(status: number, code: string) { super(code); this.status = status; }
}

/** Chain selector on wire messages; absent means Solana (the pre-multi-chain contract). */
const chainOf = (v: unknown): ChainId | null => (v == null || v === "solana" ? "solana" : v === "evm" || v === "bitcoin" ? v : null);

export class ApiServer {
  private httpServer: http.Server;
  private wss: WebSocketServer;
  private pairing: Pairing;
  private code: string | null = null;
  private codeExpires = 0;
  private history: { at: number; origin?: string; summary: string; risk: string; outcome: string }[] = [];

  constructor(private d: ServerDeps) {
    mkdirSync(d.dataDir, { recursive: true });
    this.pairing = this.loadPairing();
    this.httpServer = http.createServer((req, res) => this.http(req, res).catch((e) => { if (e instanceof HttpError) { res.statusCode = e.status; res.setHeader("content-type", "application/json"); res.end(JSON.stringify({ error: e.message })); return; } res.statusCode = 500; res.end(JSON.stringify({ error: String(e) })); }));
    this.wss = new WebSocketServer({ server: this.httpServer });
    this.wss.on("connection", (ws, req) => this.socket(ws, req.socket.remoteAddress ?? "?"));
  }

  private pairingPath() { return join(this.d.dataDir, "pairing.json"); }
  private loadPairing(): Pairing {
    try { return existsSync(this.pairingPath()) ? JSON.parse(readFileSync(this.pairingPath(), "utf8")) : { tokens: [] }; } catch { return { tokens: [] }; }
  }
  private savePairing() { writeFileSync(this.pairingPath(), JSON.stringify(this.pairing), { mode: 0o600 }); }

  /** Generate a fresh pairing code and have the robot speak it. Valid 2 minutes. */
  private pairMisses = 0;
  async newPairingCode(): Promise<string> {
    this.pairMisses = 0;
    this.code = String(randomInt(0, 1_000_000)).padStart(6, "0");
    this.codeExpires = Date.now() + 120_000;
    await this.d.robot.say(`Pairing code: ${this.code.split("").join(" ")}. It expires in two minutes.`);
    return this.code;
  }

  private tokenOk(t: unknown) {
    if (typeof t !== "string" || !t) return false;
    const b = Buffer.from(t);
    return this.pairing.tokens.some((k) => { const kb = Buffer.from(k); return kb.length === b.length && timingSafeEqual(kb, b); });
  }

  listen() {
    return new Promise<void>((res) => this.httpServer.listen(this.d.port, this.d.bind, () => { log.info(`api on ${this.d.bind}:${this.d.port}`); res(); }));
  }
  close() { this.wss.close(); this.httpServer.close(); }
  get address() { return this.httpServer.address(); }

  // ── websocket (extension) ──
  private socket(ws: WebSocket, remote: string) {
    let authed = false;
    let origin: string | undefined;
    const send = (o: unknown) => { if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(o)); };
    ws.on("message", async (raw) => {
      if (typeof raw !== "string" && (raw as Buffer).length > 256 * 1024) { send({ t: "error", error: "too_big" }); return ws.close(1009); }
      let m: any; try { m = JSON.parse(raw.toString()); } catch { return send({ t: "error", error: "bad_json" }); }
      try {
        if (m.t === "pair") {
          // a six-digit code is guessable in its two-minute window from any host that can reach this port:
          // three misses retire the code (the owner asks for a new one), and every miss waits a second
          if (!this.code || Date.now() > this.codeExpires) return send({ t: "error", error: "bad_code" });
          const codeOk = typeof m.code === "string" && m.code.length === 6 && timingSafeEqual(Buffer.from(m.code), Buffer.from(this.code));
          if (!codeOk) {
            this.pairMisses++;
            await new Promise((r) => setTimeout(r, 1000 * Math.min(this.pairMisses, 5)));
            if (this.pairMisses >= 3) { this.code = null; await this.d.robot.say("Pairing cancelled after three wrong codes. Ask me for a new one."); return send({ t: "error", error: "code_retired" }); }
            if (this.pairMisses === 1) await this.d.robot.say("Wrong pairing code.");
            return send({ t: "error", error: "bad_code" });
          }
          const token = randomBytes(32).toString("base64url");
          this.pairing.tokens.push(token); this.savePairing(); this.code = null;
          authed = true;
          await this.d.robot.say("Paired. Your browser can talk to me now.");
          return send({ t: "paired", token, pubkey: this.d.getOwner(), addresses: this.d.signer.addresses() });
        }
        if (m.t === "connect") {
          if (!this.tokenOk(m.token)) return send({ t: "error", error: "unauthorized" });
          authed = true; origin = typeof m.origin === "string" ? m.origin : undefined;
          log.info("extension connected", { remote, origin });
          return send({ t: "connected", pubkey: this.d.getOwner(), addresses: this.d.signer.addresses() });
        }
        if (!authed) return send({ t: "error", error: "unauthorized" });
        if (m.t === "signTransaction") {
          const chain = chainOf(m.chain);
          if (!chain) return send({ t: "denied", id: m.id, reason: "bad_chain", detail: `chain must be solana, evm or bitcoin` });
          const payload = chain === "bitcoin" ? m.psbt : m.tx;
          if (typeof payload !== "string") return send({ t: "denied", id: m.id, reason: "parse_error", detail: chain === "bitcoin" ? "psbt (base64) required" : "tx (base64) required" });
          const bytes = new Uint8Array(Buffer.from(payload, "base64"));
          const o = typeof m.origin === "string" ? m.origin : origin;
          const requestedChainId = chain === "evm" && m.chainId != null ? Number(m.chainId) : undefined;
          // explain first so the popup shows what the robot is reading out
          try {
            const ex = await this.d.signer.explainChain(chain, bytes, o, requestedChainId);
            send({ t: "explain", id: m.id, chain, summary: ex.explanation.spoken, lines: ex.explanation.lines, risk: ex.risk.level, reasons: ex.risk.reasons });
          } catch (e) { return send({ t: "denied", id: m.id, reason: "parse_error", detail: String((e as Error).message) }); }
          const r = await this.d.signer.signChain({ chain, bytes, origin: o, source: "extension", requestedChainId });
          this.history.unshift({ at: Date.now(), origin: o, summary: r.explanation?.spoken ?? "", risk: r.risk?.level ?? "?", outcome: r.ok ? "signed" : r.reason }); this.history.length = Math.min(this.history.length, 50);
          if (!r.ok) return send({ t: "denied", id: m.id, reason: r.reason, detail: r.detail });
          if (chain === "bitcoin") {
            const { Transaction } = await import("bitcoinjs-lib");
            const tx = Transaction.fromBuffer(Buffer.from(r.signedTx));
            return send({ t: "signed", id: m.id, chain, tx: Buffer.from(r.signedTx).toString("hex"), txid: tx.getId() });
          }
          return send({ t: "signed", id: m.id, chain, tx: chain === "evm" ? "0x" + Buffer.from(r.signedTx).toString("hex") : Buffer.from(r.signedTx).toString("base64") });
        }
        if (m.t === "signMessage") {
          const chain = chainOf(m.chain);
          if (chain === "evm") {
            const r = await this.d.signer.signEvmMessage({ message: typeof m.message === "string" ? new Uint8Array(Buffer.from(m.message, "base64")) : undefined, typedData: m.typedData, origin: typeof m.origin === "string" ? m.origin : origin, requestedChainId: m.chainId != null ? Number(m.chainId) : undefined });
            if (r.ok) return send({ t: "signed", id: m.id, chain, signature: "0x" + Buffer.from(r.signature).toString("hex") });
            return send({ t: "denied", id: m.id, reason: r.reason, detail: r.detail });
          }
          if (chain === "bitcoin") return send({ t: "denied", id: m.id, reason: "unsupported", detail: "Bitcoin message signing (BIP-322) is not available yet" });
          const bytes = new Uint8Array(Buffer.from(String(m.message), "base64"));
          const r = await this.d.signer.signMessage(bytes, typeof m.origin === "string" ? m.origin : origin);
          if (r.ok) return send({ t: "signed", id: m.id, signature: Buffer.from(r.signature).toString("base64") });
          return send({ t: "denied", id: m.id, reason: r.reason });
        }
        if (m.t === "ping") return send({ t: "pong" });
        send({ t: "error", id: m.id, error: "unknown_message" });
      } catch (e) {
        send({ t: "error", id: m?.id, error: String((e as Error).message ?? e) });
      }
    });
  }

  // ── http (local) ──
  private async http(req: http.IncomingMessage, res: http.ServerResponse) {
    const url = new URL(req.url ?? "/", "http://x");
    const json = (code: number, body: unknown) => { res.statusCode = code; res.setHeader("content-type", "application/json"); res.end(JSON.stringify(body, (_k, v) => (typeof v === "bigint" ? v.toString() : v))); };
    const body = async () => {
      if (req.method === "POST" && !/^application\/json\b/i.test(String(req.headers["content-type"] ?? ""))) throw new HttpError(415, "json_only");
      const chunks: Buffer[] = []; let size = 0;
      for await (const c of req) { size += (c as Buffer).length; if (size > 64 * 1024) throw new HttpError(413, "too_big"); chunks.push(c as Buffer); }
      const s = Buffer.concat(chunks).toString("utf8"); return s ? JSON.parse(s) : {};
    };
    const local = ["127.0.0.1", "::1", "::ffff:127.0.0.1"].includes(req.socket.remoteAddress ?? "");
    // a browser on this machine can be pointed at these routes by any page (DNS rebinding, a plain cross-site
    // POST): the Host header must name this machine, and a paired caller may use its token from elsewhere
    const host = String(req.headers.host ?? "");
    const hostLocal = /^(127\.0\.0\.1|localhost|\[::1\])(:\d+)?$/i.test(host);
    const bearer = /^Bearer\s+(\S+)$/.exec(String(req.headers.authorization ?? ""))?.[1];
    const paired = bearer !== undefined && this.tokenOk(bearer);
    if (!(local && hostLocal) && !paired) return json(403, { error: "local_only" });

    if (req.method === "GET" && url.pathname === "/status") {
      const h = await this.d.controller.hello();
      const st = this.d.settings?.get();
      return json(200, { pubkey: this.d.getOwner(), addresses: this.d.signer.addresses(), chains: this.d.signer.chains(), mode: st?.mode ?? "online", brain: st?.brain ?? "lamport", controller: { fw: h.fw, se050: h.se050, locked: h.locked, policy: h.policy, allowance: h.allowance, duress_pin_set: h.duress_pin_set ?? false, pending: this.d.controller.pending() }, presence: this.d.robot.state(), agent: this.d.agent?.status() ?? null, history: this.history.slice(0, 20) });
    }
    if (req.method === "POST" && url.pathname === "/explain") {
      const b = await body();
      const chain = chainOf(b.chain);
      if (!chain) return json(400, { error: "bad_chain" });
      if (chain === "solana") {
        const bytes = new Uint8Array(Buffer.from(String(b.tx), "base64"));
        const ex = await this.d.signer.explainTx(bytes, b.origin);
        return json(200, { chain, summary: ex.explanation.spoken, lines: ex.explanation.lines, risk: ex.risk.level, reasons: ex.risk.reasons, parsed: { lamportsOut: ex.parsed.lamportsOut, dest: ex.parsed.dest, programs: ex.parsed.programs, dangers: ex.parsed.dangers, tokenOut: ex.parsed.tokenOut } });
      }
      const bytes = new Uint8Array(Buffer.from(String(chain === "bitcoin" ? b.psbt : b.tx), "base64"));
      const ex = await this.d.signer.explainChain(chain, bytes, b.origin, b.chainId != null ? Number(b.chainId) : undefined);
      return json(200, { chain, summary: ex.explanation.spoken, lines: ex.explanation.lines, risk: ex.risk.level, reasons: ex.risk.reasons, parsed: { nativeOut: ex.parsed.nativeOut, nativeUnit: ex.parsed.nativeUnit, dest: ex.parsed.dest, programs: ex.parsed.programs, dangers: ex.parsed.dangers, tokenOut: ex.parsed.tokenOut } });
    }
    if (!(local && hostLocal)) return json(403, { error: "local_only" }); /* the rest is the operator's, on this machine */
    if (req.method === "POST" && url.pathname === "/pair/new") return json(200, { code: await this.newPairingCode() });
    if (req.method === "GET" && url.pathname === "/tokens") return json(200, { tokens: this.tokenList(), idleDays: TOKEN_IDLE_MS / 86_400_000 });
    // ── programs the owner approved by hand; nothing else ever stops being "unknown" ──
    if (url.pathname === "/programs" && this.d.settings) {
      if (req.method === "GET") return json(200, { known: this.d.settings.get().knownPrograms });
      if (req.method === "POST") {
        const b = await body();
        const idOk = (p: unknown): p is string => typeof p === "string" && (/^[1-9A-HJ-NP-Za-km-z]{32,44}$/.test(p) || /^0x[0-9a-fA-F]{40}$/.test(p));
        const add = (Array.isArray(b.add) ? b.add : []).filter(idOk).map((p: string) => (p.startsWith("0x") ? p.toLowerCase() : p));
        const remove = new Set((Array.isArray(b.remove) ? b.remove : []).filter(idOk).map((p: string) => (p.startsWith("0x") ? p.toLowerCase() : p)));
        if ((Array.isArray(b.add) ? b.add : []).length !== add.length) return json(400, { error: "bad_program", detail: "programs are base58 Solana ids or 0x EVM addresses" });
        const cur = this.d.settings.get().knownPrograms;
        const known = [...new Set([...cur.filter((p) => !remove.has(p)), ...add])];
        try { this.d.settings.update({ knownPrograms: known }); } catch (e) { return json(400, { error: "bad_settings", detail: String((e as Error).message ?? e).slice(0, 300) }); }
        return json(200, { known });
      }
    }
    if (req.method === "POST" && url.pathname === "/pair/revoke") { this.pairing = { tokens: [] }; this.savePairing(); return json(200, { ok: true }); }
    if (req.method === "GET" && url.pathname === "/agent") return json(200, this.d.agent?.status() ?? { enabled: false });
    if (req.method === "GET" && url.pathname === "/agent/report") return json(200, this.d.agent?.lastReport() ?? { items: [] });
    if (req.method === "POST" && url.pathname === "/agent/budget") { const b = await body(); if (!this.d.agent) return json(400, { error: "agent_disabled" }); const ok = await this.d.agent.setBudget(BigInt(b.lamports)); return json(200, { ok }); }
    if (req.method === "POST" && url.pathname === "/agent/arm") { if (!this.d.agent) return json(400, { error: "agent_disabled" }); const b = await body(); const hours = Number(b.hours ?? 10); return json(200, { ok: await this.d.agent.arm(Math.floor(Date.now() / 1000) + hours * 3600) }); }
    if (req.method === "POST" && url.pathname === "/agent/morning") { if (!this.d.agent) return json(400, { error: "agent_disabled" }); return json(200, await this.d.agent.morning()); }
    if (req.method === "POST" && url.pathname === "/agent/tick") { if (!this.d.agent) return json(400, { error: "agent_disabled" }); return json(200, await this.d.agent.tick()); }
    // ── controller firmware (PROTOCOL §1 "update"; local only, the robot's own operator) ──
    if (url.pathname.startsWith("/firmware")) {
      if (!local) return json(403, { error: "local_only" });
      if (req.method === "GET" && url.pathname === "/firmware") { const h = await this.d.controller.hello(); return json(200, { fw: h.fw, update: h.update ?? null, pending: this.d.controller.pending() }); }
      if (req.method === "POST" && url.pathname === "/firmware/check") { const b = await body(); return json(200, checkImage(new Uint8Array(Buffer.from(String(b.image ?? ""), "base64")))); }
      if (req.method === "POST" && url.pathname === "/firmware/update") {
        const b = await body();
        const h0 = await this.d.controller.hello();
        if (this.d.prod && h0.update?.secure_boot !== true) return json(400, { error: "secure_boot_off", detail: "the controller's bootrom does not check signatures; provision the OTP before updating a production unit" });
        const image = new Uint8Array(Buffer.from(String(b.image ?? ""), "base64"));
        if (!image.length) return json(400, { error: "no_image" });
        const r = await flashFirmware(this.d.controller, image, String(b.version ?? ""), { onProgress: (sent, total) => { if (sent === total || sent % (64 * 1024) === 0) log.info("firmware", { sent, total }); } });
        if (r.ok) log.warn("firmware update accepted; the controller is rebooting into " + r.version + ", this daemon restarts when the link returns");
        return json(r.ok ? 200 : 400, r);
      }
      if (req.method === "POST" && url.pathname === "/firmware/commit") return json(200, await confirmFirmware(this.d.controller));
      if (req.method === "POST" && url.pathname === "/firmware/abort") { await this.d.controller.updateAbort(); return json(200, { ok: true }); }
      return json(404, { error: "not_found" });
    }
    if (req.method === "POST" && url.pathname === "/lock") { await this.d.controller.lock(); return json(200, { ok: true }); }
    if (req.method === "POST" && url.pathname === "/unlock") return json(200, { ok: await this.d.controller.unlock() });
    // ── decoy wallet: a second rhythm opens it through /unlock; the owner funds it so it looks real ──
    if (req.method === "POST" && url.pathname === "/duress/set_pin") {
      await this.d.robot.say("Tap a second rhythm, different from your PIN. It opens the decoy wallet. Then tap it again.");
      const r = await this.d.controller.setDuressPin();
      await this.d.robot.say(r.ok ? "Duress rhythm set. Tapping it opens the decoy; nobody will know." : r.reason === "pin_mismatch" ? "That's your real PIN. Pick a different rhythm." : "Not set.");
      return json(r.ok ? 200 : 400, r);
    }
    if (req.method === "GET" && url.pathname === "/duress/decoy") {
      await this.d.robot.say("Tap your PIN to see the decoy wallet's addresses.");
      const r = await this.d.controller.decoyAddresses();
      return json(r.ok ? 200 : 400, r.ok ? { ...r, hint: "Send a small, believable balance to these addresses. An empty decoy convinces nobody." } : r);
    }
    // ── owner settings: every optional feature is off until switched on here ──
    if (url.pathname === "/settings" && this.d.settings) {
      if (req.method === "GET") return json(200, this.d.settings.get());
      if (req.method === "POST") {
        let s;
        try { s = this.d.settings.update(await body()); } catch (e) {
          if (e instanceof SettingsError) return json(400, { error: e.code, detail: e.message });
          return json(400, { error: "bad_settings", detail: String((e as Error).message ?? e).slice(0, 300) });
        }
        await this.d.robot.settings({ room_lock: s.roomLock, two_person: s.twoPersonSol > 0, airgap: s.airgap, wake: true, dock: s.dock, mode: s.mode, brain: s.brain });
        if (isCloudBrain(s.brain)) log.info("brain", s.brain);
        return json(200, s);
      }
    }
    // ── swaps (online mode only; every route re-verified against the router allowlist before signing) ──
    if (url.pathname === "/swap/quote" || url.pathname === "/swap/execute") {
      if (req.method !== "POST") return json(405, { error: "method" });
      if (!this.d.swap) return json(400, { error: "swaps_disabled" });
      if (this.d.settings?.get().mode === "offline") return json(403, { error: "offline_mode", detail: "swaps need the network and the wallet is in offline mode. Switch mode to online in settings first." });
      try {
        const b = await body();
        if (url.pathname === "/swap/quote") {
          const q = await this.d.swap.quote({ from: b.from, to: b.to, amount: b.amount, slippageBps: b.slippageBps != null ? Number(b.slippageBps) : undefined });
          await this.d.robot.say(q.spoken);
          return json(200, { quoteId: q.id, route: q.route, from: { chain: q.from.chain, chainId: q.from.chainId, asset: q.from.id, symbol: q.from.symbol, decimals: q.from.decimals }, to: { chain: q.to.chain, chainId: q.to.chainId, asset: q.to.id, symbol: q.to.symbol, decimals: q.to.decimals }, amountIn: q.amountIn, expectedOut: q.expectedOut, minOut: q.minOut, slippageBps: q.slippageBps, spoken: q.spoken, expiresAt: q.expiresAt });
        }
        const r = await this.d.swap.execute(String(b.quoteId), b.origin);
        await this.d.robot.say(r.spoken);
        return json(200, r);
      } catch (e) {
        if (e instanceof SwapError) { if (!["offline_mode", "unknown_quote", "expired", "bad_amount", "bad_slippage"].includes(e.code)) log.warn("swap refused", { code: e.code, detail: e.message }); return json(e.code === "offline_mode" ? 403 : 400, { ok: false, error: e.code, detail: e.message }); }
        return json(502, { ok: false, error: "provider", detail: String((e as Error).message ?? e).slice(0, 300) });
      }
    }
    if (req.method === "POST" && url.pathname === "/broadcast") {
      const b = await body();
      const chain = chainOf(b.chain);
      if (!chain) return json(400, { error: "bad_chain" });
      const raw = String(b.tx ?? "");
      const bytes = chain === "solana" ? Buffer.from(raw, "base64") : Buffer.from(raw.replace(/^0x/, ""), "hex");
      try { return json(200, { chain, txid: await this.d.signer.broadcast(chain, new Uint8Array(bytes)) }); } catch (e) { return json(502, { error: "broadcast_failed", detail: String((e as Error).message ?? e).slice(0, 300) }); }
    }
    if (req.method === "POST" && url.pathname === "/room/enroll") return json(200, { ok: await this.d.robot.enrollRoom() });
    if (req.method === "POST" && url.pathname === "/dock") { const b = await body(); const action = ["go", "leave", "status"].includes(b.action) ? b.action : "status"; return json(200, await this.d.robot.dock(action)); }
    // ── offline payments ──
    if (this.d.offline && this.d.settings?.get().offlinePayments) {
      if (req.method === "POST" && url.pathname === "/offline/sign") { const b = await body(); const r = await this.d.offline.sign(new Uint8Array(Buffer.from(String(b.tx), "base64")), b.origin); return json(r.ok ? 200 : 403, r); }
      if (req.method === "GET" && url.pathname === "/offline/pending") return json(200, this.d.offline.pending());
      if (req.method === "POST" && url.pathname === "/offline/broadcast") { const b = await body(); return json(200, await this.d.offline.broadcast(String(b.id))); }
      if (req.method === "POST" && url.pathname === "/offline/flush") return json(200, await this.d.offline.flush());
      if (req.method === "POST" && url.pathname === "/offline/nonce/create" && this.d.conn) { const { PublicKey } = await import("@solana/web3.js"); const r = await nonceAccountCreateTx(this.d.conn, new PublicKey(this.d.getOwner())); const signed = await this.d.signer.sign({ txBytes: r.tx, source: "api" }); if (!signed.ok) return json(403, signed); const sig = await this.d.conn.sendRawTransaction(signed.signedTx); return json(200, { nonceAccount: r.nonceAccount, signature: sig }); }
    } else if (url.pathname.startsWith("/offline/")) return json(403, { error: "offline_payments_disabled" });
    // ── dead-man's switch ──
    if (this.d.inheritance) {
      if (req.method === "GET" && url.pathname === "/inheritance") return json(200, { ...(await this.d.inheritance.status()), settings: this.d.settings?.get().inheritance });
      if (req.method === "POST" && url.pathname === "/inheritance/arm") {
        const b = await body(); const st = this.d.settings?.get().inheritance;
        const r = await this.d.inheritance.arm({ heir: b.heir ?? st?.heir, inactivityDays: Number(b.inactivityDays ?? st?.inactivityDays ?? 90), warnDays: b.warnDays ?? st?.warnDays ?? [14, 7, 1], solLamports: b.solLamports != null ? BigInt(b.solLamports) : undefined, mints: b.mints ?? [], nonceAccount: String(b.nonceAccount) });
        if (r.ok && this.d.settings) this.d.settings.update({ inheritance: { enabled: true, heir: b.heir ?? st?.heir } });
        return json(r.ok ? 200 : 403, r);
      }
      if (req.method === "POST" && url.pathname === "/inheritance/clear") { const ok = await this.d.inheritance.clear(); if (ok && this.d.settings) this.d.settings.update({ inheritance: { enabled: false } }); return json(200, { ok }); }
      if (req.method === "POST" && url.pathname === "/inheritance/release") return json(200, await this.d.inheritance.release());
    }
    // ── morning briefing ──
    if (this.d.briefing) {
      if (req.method === "GET" && url.pathname === "/briefing") { const facts = await this.d.briefing.collect(); return json(200, { facts, lines: this.d.briefing.compose(facts) }); }
      if (req.method === "POST" && url.pathname === "/briefing/now") return json(200, await this.d.briefing.deliver());
    }
    // ── alerts ──
    if (this.d.alerts) {
      if (req.method === "GET" && url.pathname === "/alerts") return json(200, this.d.alerts.list());
      if (req.method === "POST" && url.pathname === "/alerts") { const b = await body(); return json(200, this.d.alerts.add({ asset: String(b.asset ?? "TOTAL"), dropPct: Number(b.dropPct ?? 10), risePct: b.risePct != null ? Number(b.risePct) : undefined, reference: b.reference === "set" ? "set" : "day", wakeAtNight: !!b.wakeAtNight, enabled: b.enabled !== false })); }
      if (req.method === "DELETE" && url.pathname.startsWith("/alerts/")) { this.d.alerts.remove(url.pathname.slice(8)); return json(200, { ok: true }); }
      if (req.method === "POST" && url.pathname === "/alerts/tick") return json(200, await this.d.alerts.tick());
    }
    if (req.method === "POST" && url.pathname === "/dev/touch") { if (!this.d.devTouch) return json(400, { error: "not_dev" }); const b = await body(); this.d.devTouch(!!b.long); return json(200, { ok: true }); }
    json(404, { error: "not_found" });
  }
}
