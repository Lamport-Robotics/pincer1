/** SerialController over a fake port: what the daemon writes, and how replies without an id find their caller. */
import { describe, it, expect, vi } from "vitest";

const ports: FakePort[] = [];
class FakePort {
  written: any[] = [];
  private handlers = new Map<string, (d: any) => void>();
  constructor(_opts: unknown) { ports.push(this); }
  on(ev: string, cb: (d: any) => void) { this.handlers.set(ev, cb); return this; }
  open(cb: (e?: Error) => void) { cb(); }
  write(data: string, cb: (e?: Error) => void) { this.written.push(JSON.parse(data)); cb(); return true; }
  reply(o: unknown) { this.handlers.get("data")?.(Buffer.from(JSON.stringify(o) + "\n")); }
}
vi.mock("serialport", () => ({ SerialPort: FakePort }));
const { SerialController } = await import("../src/controller/serial.js");

const within = <T>(p: Promise<T>, ms = 1000) => Promise.race([p, new Promise<never>((_, rej) => setTimeout(() => rej(new Error("still waiting")), ms))]);

describe("serial controller", () => {
  it("two callers of hello (or lock) at once share the one untagged reply", async () => {
    const c = new SerialController("/dev/fake");
    const port = ports.at(-1)!;
    const a = c.hello(), b = c.hello();
    port.reply({ t: "hello", pubkey: "So11111111111111111111111111111111111111112", locked: false });
    const [ha, hb] = await within(Promise.all([a, b])); // the first caller used to wait out its 5 s and fail
    expect(port.written.filter((m) => m.t === "hello")).toHaveLength(1);
    expect(ha.pubkey).toBe("So11111111111111111111111111111111111111112");
    expect(hb).toEqual(ha);

    const l1 = c.lock(), l2 = c.lock();
    port.reply({ t: "locked" });
    await within(Promise.all([l1, l2]));
    // and the next one is a fresh request
    const again = c.hello();
    expect(port.written.filter((m) => m.t === "hello")).toHaveLength(2);
    port.reply({ t: "hello", pubkey: "x", locked: true });
    expect((await within(again)).locked).toBe(true);
  });
});
