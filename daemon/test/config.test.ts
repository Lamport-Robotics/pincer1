/** env parsing: booleans are read as written, not by JavaScript truthiness. */
import { describe, it, expect, vi, beforeEach } from "vitest";

async function load(env: Record<string, string>) {
  vi.resetModules();
  const saved = { ...process.env };
  for (const k of Object.keys(process.env)) if (k.startsWith("AGENT_") || k === "PINCER_MODE") delete process.env[k];
  Object.assign(process.env, env);
  try { const m = await import("../src/config.js"); return m.config(); } finally { process.env = saved; }
}

describe("config", () => {
  beforeEach(() => vi.resetModules());
  it("AGENT_ENABLED=false means false", async () => {
    expect((await load({ AGENT_ENABLED: "false" })).AGENT_ENABLED).toBe(false);
    expect((await load({ AGENT_ENABLED: "0" })).AGENT_ENABLED).toBe(false);
    expect((await load({ AGENT_ENABLED: "" })).AGENT_ENABLED).toBe(false);
    expect((await load({})).AGENT_ENABLED).toBe(false);
  });
  it("AGENT_ENABLED=true / 1 means true", async () => {
    expect((await load({ AGENT_ENABLED: "true" })).AGENT_ENABLED).toBe(true);
    expect((await load({ AGENT_ENABLED: "1" })).AGENT_ENABLED).toBe(true);
  });
  it("anything else is refused rather than guessed", async () => {
    await expect(load({ AGENT_ENABLED: "maybe" })).rejects.toThrow();
  });
});
