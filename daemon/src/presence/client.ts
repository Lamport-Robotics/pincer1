/**
 * Client for the robot service (docs/PROTOCOL.md §2). Keeps the latest presence state, lets the daemon
 * make the robot speak, and reconnects forever.
 */
import WebSocket from "ws";
import { EventEmitter } from "node:events";

export interface Presence {
  person: boolean;
  owner: boolean;
  voice_ok: boolean;
  duress: boolean;
  moved: boolean;
  confidence: { lidar: number; face: number };
  ts: number;
  /** Similarity of the current lidar scan to the enrolled room (0–1); undefined when the robot has no fingerprint yet. */
  room_match?: number;
  /** Distinct human bodies the lidar sees. */
  persons?: number;
  /** Labels of enrolled faces matched right now (the two-person rule needs two). */
  owners?: string[];
  /** Whether the robot's radio is on (air-gap relay). */
  network?: boolean;
  /** Pack state from the INA226 (optional dock feature). */
  battery?: { pct: number; voltage: number; charging: boolean };
  dock?: "off" | "idle" | "navigating" | "approaching" | "verifying" | "charging" | "undocking" | "failed";
}

export const NO_PRESENCE: Presence = { person: false, owner: false, voice_ok: false, duress: false, moved: false, confidence: { lidar: 0, face: 0 }, ts: 0 };

export interface RobotLink {
  state(): Presence;
  /** True when the state is fresh (< 6 s) and the owner is verified and nothing is wrong. */
  ownerPresent(): boolean;
  say(text: string): Promise<void>;
  askPassphrase(): Promise<boolean>;
  attention(): Promise<void>;
  morningReport(items: string[]): Promise<void>;
  /** Morning briefing: structured facts plus plain sentences. The robot's brain phrases them in its voice, or reads the sentences as they are. */
  brief(facts: unknown, lines: string[]): Promise<void>;
  /** Air-gap relay: ask the robot to enable its radio for a while. */
  network(on: boolean, reason: string, forSecs?: number): Promise<void>;
  enrollRoom(): Promise<boolean>;
  /** Wake the owner: walk over (navigating to their room when mapped), speak escalating lines until a touch, the passphrase, or maxSecs. Returns acked. */
  wake(reason: string, lines: string[], maxSecs?: number): Promise<boolean>;
  /** Push owner settings that live on the robot (room lock, two-person, airgap mode, wake). */
  settings(s: { room_lock?: boolean; two_person?: boolean; airgap?: "off" | "strict" | "night"; wake?: boolean; dock?: boolean; mode?: "offline" | "online"; brain?: "lamport" | "claude" | "openai" | "deepseek" }): Promise<void>;
  /** Charging dock (optional): send the robot to charge, or pull it off. */
  dock(action: "go" | "leave" | "status"): Promise<unknown>;
  /** Forward a controller touch so the robot can treat it as an acknowledgement. */
  touch(): void;
  on(ev: "presence", cb: (p: Presence) => void): void;
  close(): void;
}

export class RobotClient extends EventEmitter implements RobotLink {
  private ws: WebSocket | null = null;
  private cur: Presence = NO_PRESENCE;
  private seq = 0;
  private waiters = new Map<string, (m: any) => void>();
  private closed = false;
  private backoff = 500;

  constructor(private url: string) { super(); this.connect(); }

  private connect() {
    if (this.closed) return;
    const ws = new WebSocket(this.url);
    this.ws = ws;
    ws.on("open", () => { this.backoff = 500; });
    ws.on("message", (raw) => {
      let m: any; try { m = JSON.parse(raw.toString()); } catch { return; }
      if (m.t === "presence") { this.cur = m; this.emit("presence", m); return; }
      if (m.id && this.waiters.has(m.id)) { this.waiters.get(m.id)!(m); this.waiters.delete(m.id); }
    });
    const retry = () => { if (this.closed) return; this.cur = { ...NO_PRESENCE }; setTimeout(() => this.connect(), this.backoff); this.backoff = Math.min(this.backoff * 2, 10_000); };
    ws.on("close", retry);
    ws.on("error", () => ws.close());
  }

  private send(obj: Record<string, unknown>) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    this.ws.send(JSON.stringify(obj));
    return true;
  }

  private request(obj: Record<string, unknown>, timeoutMs = 30_000): Promise<any> {
    const id = `d${++this.seq}`;
    return new Promise((resolve) => {
      if (!this.send({ ...obj, id })) return resolve(null);
      const timer = setTimeout(() => { this.waiters.delete(id); resolve(null); }, timeoutMs);
      this.waiters.set(id, (m) => { clearTimeout(timer); resolve(m); });
    });
  }

  state() { return this.cur; }
  ownerPresent() {
    const p = this.cur;
    return Date.now() / 1000 - p.ts < 6 && p.person && p.owner && !p.duress && !p.moved;
  }
  async say(text: string) { await this.request({ t: "say", text }, 60_000); }
  async askPassphrase() { const m = await this.request({ t: "ask_passphrase" }, 30_000); return !!m?.ok; }
  async attention() { this.send({ t: "attention" }); }
  async morningReport(items: string[]) { await this.request({ t: "morning_report", items }, 300_000); }
  async brief(facts: unknown, lines: string[]) { await this.request({ t: "brief", facts, lines }, 300_000); }
  async network(on: boolean, reason: string, forSecs = 600) { this.send({ t: "network", on, reason, for_secs: forSecs }); }
  async enrollRoom() { const m = await this.request({ t: "enroll_room" }, 120_000); return !!m?.ok; }
  async wake(reason: string, lines: string[], maxSecs = 120) { const m = await this.request({ t: "wake", reason, lines, max_secs: maxSecs }, (maxSecs + 60) * 1000); return !!m?.acked; }
  async settings(s: Record<string, unknown>) { this.send({ t: "settings", ...s }); }
  async dock(action: "go" | "leave" | "status") { return this.request({ t: "dock", action }, 30_000); }
  touch() { this.send({ t: "touch" }); }
  close() { this.closed = true; this.ws?.close(); }
}

/** No robot attached (dev): presence is whatever you set, speech goes to the log. */
export class MockRobot extends EventEmitter implements RobotLink {
  cur: Presence = { ...NO_PRESENCE, person: true, owner: true, voice_ok: true, confidence: { lidar: 1, face: 1 }, ts: Date.now() / 1000 };
  spoken: string[] = [];
  passphraseOk = true;
  constructor(private log: (s: string) => void = () => {}) { super(); }
  set(p: Partial<Presence>) { this.cur = { ...this.cur, ...p, ts: Date.now() / 1000 }; this.emit("presence", this.cur); }
  state() { return { ...this.cur, ts: Date.now() / 1000 }; }
  ownerPresent() { const p = this.cur; return p.person && p.owner && !p.duress && !p.moved; }
  async say(text: string) { this.spoken.push(text); this.log(`[robot says] ${text}`); }
  async askPassphrase() { return this.passphraseOk; }
  async attention() {}
  async morningReport(items: string[]) { for (const i of items) await this.say(i); }
  briefs: { facts: unknown; lines: string[] }[] = [];
  async brief(facts: unknown, lines: string[]) { this.briefs.push({ facts, lines }); for (const l of lines) await this.say(l); }
  networkLog: { on: boolean; reason: string }[] = [];
  async network(on: boolean, reason: string) { this.networkLog.push({ on, reason }); this.cur = { ...this.cur, network: on }; }
  async enrollRoom() { this.cur = { ...this.cur, room_match: 1 }; return true; }
  wakes: { reason: string; lines: string[] }[] = [];
  wakeAck = true;
  async wake(reason: string, lines: string[]) { this.wakes.push({ reason, lines }); for (const l of lines.slice(0, 2)) await this.say(l); return this.wakeAck; }
  settingsLog: Record<string, unknown>[] = [];
  async settings(s: Record<string, unknown>) { this.settingsLog.push(s); }
  async dock(action: "go" | "leave" | "status") { return { ok: true, action }; }
  touch() {}
  close() {}
}
