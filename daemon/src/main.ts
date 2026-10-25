#!/usr/bin/env node
/**
 * pincerd — the Pincer1 wallet daemon.
 *
 *   CONTROLLER=sim      PINCER_MODE=dev  pnpm dev      laptop: software signer, mock or real robot
 *   CONTROLLER=/dev/ttyACM0 PINCER_MODE=prod            robot: real consent controller + SE050
 */
import { Connection } from "@solana/web3.js";
import { config, log } from "./config.js";
import { SimController } from "./controller/sim.js";
import { SerialController } from "./controller/serial.js";
import type { ControllerClient } from "./controller/types.js";
import { confirmFirmware } from "./firmware.js";
import { MockRobot, RobotClient, type RobotLink } from "./presence/client.js";
import { Signer } from "./sign.js";
import { ApiServer } from "./api/server.js";
import { Agent } from "./agent/index.js";
import { SettingsStore } from "./settings.js";
import { Offline } from "./offline/index.js";
import { Inheritance } from "./inheritance/index.js";
import { Alerts } from "./alerts/index.js";
import { Briefing } from "./briefing/index.js";
import { EvmAdapter } from "./chains/evm/index.js";
import { rpcsFromEnv } from "./chains/evm/rpc.js";
import { BitcoinAdapter } from "./chains/bitcoin/index.js";
import { Esplora } from "./chains/bitcoin/esplora.js";
import { SwapService } from "./swap/index.js";
import { OneInchProvider, ZeroXProvider } from "./swap/providers.js";

async function main() {
  const cfg = config();
  const conn = new Connection(cfg.RPC_URL, { commitment: "confirmed" });

  let controller: ControllerClient;
  let devTouch: ((long: boolean) => void) | undefined;
  if (cfg.CONTROLLER === "sim") {
    if (cfg.PINCER_MODE === "prod") throw new Error("PINCER_MODE=prod refuses the software signer. Set CONTROLLER=/dev/ttyACM0.");
    const sim = new SimController({ dailyLimitLamports: cfg.dailyLimitLamports });
    devTouch = (long) => sim.touch(long);
    controller = sim;
    log.warn("using the SOFTWARE signer (sim). Not for real funds.");
  } else {
    const serial = new SerialController(cfg.CONTROLLER);
    await serial.open();
    controller = serial;
    setInterval(() => serial.time(Math.floor(Date.now() / 1000)).catch(() => {}), 60_000);
  }

  let hello = await controller.hello();
  if (cfg.PINCER_MODE === "prod") {
    // production means: hardware secure element, a trusted display the owner reads, and no bench keys compiled in
    if (!hello.se050) throw new Error("controller reports no SE050; refusing to run in prod mode");
    if (hello.display !== true) throw new Error("controller reports no trusted display; refusing to run in prod mode");
    if (hello.dev_keys !== false) throw new Error("controller carries development channel keys (or does not say); refusing to run in prod mode");
    if (hello.update && hello.update.secure_boot !== true) throw new Error("controller's bootrom does not check image signatures (OTP not provisioned); refusing to run in prod mode");
  }
  if (!hello.initialized) {
    log.warn("controller has no key yet. Hold the touch pad for 3 s to create one. The 24 words appear ONLY on the robot's display; write them down there. Nothing is printed here.");
    const r = await controller.init();
    log.info("key created", { pubkey: r.pubkey, addresses: r.addresses });
    if (r.decoy_addresses) {
      log.warn("decoy wallet addresses (fund these with a small, believable balance; an empty decoy convinces nobody):", r.decoy_addresses);
      log.warn("later: GET /duress/decoy shows them again after your PIN");
    }
    hello = await controller.hello();
  }
  if (!hello.pin_set) {
    log.warn("no PIN yet. Tap your rhythm (4-8 taps, short and long gaps) on the head, then tap it again to confirm.");
    if (!(await controller.setPin())) throw new Error("PIN setup failed; restart and try again");
    hello = await controller.hello();
  }
  if (hello.pin_set && hello.duress_pin_set === false) {
    log.warn("no duress rhythm yet (optional, recommended). Tap a SECOND rhythm, different from your PIN, twice. Tapping it at unlock opens the decoy wallet and nobody can tell. Skip with a 3-second hold.");
    const r = await controller.setDuressPin();
    log.info(r.ok ? "duress rhythm set" : `duress rhythm not set (${r.reason}); set it later with POST /duress/set_pin`);
    hello = await controller.hello();
  }
  if (hello.update?.trial) {
    // the image we are talking to is the one staged by the previous daemon run; it booted, answered hello and
    // passed the checks above, so the trial ends. Left alone, the controller steps back after three boots.
    const r = await confirmFirmware(controller);
    log.warn(r.ok ? `firmware ${hello.fw} confirmed on partition ${hello.update.partition}` : `firmware ${hello.fw} NOT confirmed (${r.reason}); the controller will roll back`);
  }
  const owner = () => hello.pubkey!;
  const addresses = () => hello.addresses ?? { solana: hello.pubkey! };
  log.info("owner", owner(), { fw: hello.fw, se050: hello.se050, addresses: addresses() });

  const robot: RobotLink = cfg.ROBOT_WS === "mock" ? new MockRobot((s) => log.info(s)) : new RobotClient(cfg.ROBOT_WS);
  const settings = new SettingsStore(cfg.DATA_DIR, {
    roomMatchMin: cfg.ROOM_MATCH_MIN, twoPersonSol: cfg.TWO_PERSON_SOL, agent: cfg.AGENT_ENABLED,
    inheritance: { enabled: false, heir: cfg.INHERIT_HEIR, inactivityDays: cfg.INHERIT_DAYS, warnDays: cfg.INHERIT_WARN_DAYS.split(",").map(Number).filter(Boolean), resignPromptPct: 20 },
  });
  const signer = new Signer(cfg, controller, robot, conn, owner, undefined, 60_000, () => settings.get());
  signer.addresses = addresses;
  // ── multi-chain: adapters are registered only when the controller has the key family ──
  const evm = new EvmAdapter({ rpcs: rpcsFromEnv(), intel: { etherscanKey: cfg.ETHERSCAN_API_KEY || undefined }, extraRouters: cfg.evmRouterAllowlist });
  const bitcoin = new BitcoinAdapter({ esplora: new Esplora(cfg.BTC_ESPLORA_URL, { maxFeeRate: cfg.BTC_MAX_FEE_RATE }), network: cfg.BTC_NETWORK, feeTargetBlocks: cfg.BTC_FEE_TARGET_BLOCKS });
  if (addresses().evm) signer.register(evm); else log.warn("controller has no EVM key; EVM signing disabled");
  if (addresses().bitcoin) signer.register(bitcoin); else log.warn("controller has no Bitcoin key; Bitcoin signing disabled");
  const swap = new SwapService({
    signer, settings: () => settings.get(), evm: addresses().evm ? evm : undefined, bitcoin: addresses().bitcoin ? bitcoin : undefined,
    aggregators: [new OneInchProvider(cfg.ONEINCH_API_KEY), new ZeroXProvider(cfg.ZEROX_API_KEY)],
    maxSlippageBps: cfg.SWAP_MAX_SLIPPAGE_BPS,
  });
  const offline = new Offline(cfg.DATA_DIR, signer, conn);
  const inheritance = new Inheritance(controller, robot, conn, owner, cfg.NOTIFY_WEBHOOK_URL);
  const quiet = () => { const [a, b] = cfg.AGENT_QUIET_HOURS.split("-"); if (!a || !b) return false; const m = (t: string) => { const [h, mm] = t.split(":").map(Number); return h * 60 + (mm || 0); }; const d = new Date(); const cur = d.getHours() * 60 + d.getMinutes(); const s0 = m(a), e0 = m(b); return s0 <= e0 ? cur >= s0 && cur < e0 : cur >= s0 || cur < e0; };
  const alerts = new Alerts(conn, robot, owner, { ntfyTopic: process.env.NTFY_TOPIC, telegram: process.env.TELEGRAM_BOT_TOKEN && process.env.TELEGRAM_CHAT_ID ? { botToken: process.env.TELEGRAM_BOT_TOKEN, chatId: process.env.TELEGRAM_CHAT_ID } : undefined, webhook: cfg.NOTIFY_WEBHOOK_URL || undefined }, () => true, quiet);
  alerts.start();
  setInterval(() => inheritance.tick().catch((e) => log.warn("inheritance tick failed", String(e))), 3600_000);
  // controller touches double as acknowledgements on the robot (wake-up alarm)
  controller.onEvent((e) => { if (e.ev === "touch" || e.ev === "long_touch") robot.touch(); });
  // push the owner's robot-side settings on connect and whenever the robot reconnects
  const pushSettings = () => { const s = settings.get(); return robot.settings({ room_lock: s.roomLock, two_person: s.twoPersonSol > 0, airgap: s.airgap, wake: true, dock: s.dock, mode: s.mode, brain: s.brain }); };
  void pushSettings();

  // presence → lock/unlock the controller
  let wasPresent = true;
  controller.onEvent((e) => { if (e.ev === "wiped") log.error("CONTROLLER WIPED after repeated wrong PINs. Recover from the 24 words."); if (e.ev === "pin_failed") log.warn("wrong PIN rhythm"); });
  let wasOnline = true;
  let lastLowBattery = 0;
  let duressLatched = false;
  robot.on("presence", async (p) => {
    if (p.duress) {
      if (!duressLatched) {
        // the rising edge: switch the controller to the decoy once, and tell the phone once. The robot keeps
        // reporting duress for a while (it clears it itself when the owner is gone or after its hold time).
        duressLatched = true;
        log.warn("DURESS");
        await controller.duress();
        alerts?.phone("Duress word heard at the robot. The decoy wallet is active.", "duress").catch(() => undefined);
      }
      return;
    }
    duressLatched = false;
    const present = p.person && p.owner && !p.moved;
    if (!present && wasPresent) { await controller.lock(); log.info("locked: owner not present"); }
    wasPresent = present;
    // network back after an air-gap or outage: push anything signed offline
    if (p.network && !wasOnline && offline.pending().length) { const r = await offline.flush(); log.info("offline flush", r); }
    if (p.network != null) wasOnline = p.network;
    // battery low with no dock: the phone gets it, the robot already said so
    if (p.battery && p.battery.pct <= 15 && !p.battery.charging && p.dock !== "navigating" && p.dock !== "approaching") {
      const now = Date.now(); if (now - lastLowBattery > 3600_000) { lastLowBattery = now; await alerts.phone(`battery at ${p.battery.pct}%`, p.dock === "failed" ? "docking failed" : p.dock === "off" ? "dock disabled" : "not charging"); }
    }
  });

  const agent = cfg.AGENT_ENABLED
    ? new Agent({ budgetLamports: BigInt(Math.round(cfg.AGENT_BUDGET_SOL * 1e9)), maxPerTradeLamports: BigInt(Math.round(cfg.AGENT_MAX_PER_TRADE_SOL * 1e9)), maxOpen: cfg.AGENT_MAX_OPEN, stopLossPct: cfg.AGENT_STOP_LOSS_PCT, takeProfitPct: cfg.AGENT_TAKE_PROFIT_PCT, quietHours: cfg.AGENT_QUIET_HOURS, signalsUrl: cfg.SIGNALS_URL }, signer, controller, robot, conn, owner)
    : null;
  agent?.start();
  const briefing = new Briefing({ robot, settings: () => settings.get(), conn, getOwner: owner, agentReport: () => agent?.lastReport()?.items ?? null, offlinePending: () => offline.pending().length, inheritance: () => inheritance.status().then((s) => s as any).catch(() => null) });
  briefing.start();

  const api = new ApiServer({ signer, controller, robot, agent, settings, offline, inheritance, alerts, briefing, swap, conn, getOwner: owner, dataDir: cfg.DATA_DIR, bind: cfg.BIND, port: cfg.PORT, devTouch, prod: cfg.PINCER_MODE === "prod" });
  await api.listen();
  log.info(`pincerd ready. mode=${cfg.PINCER_MODE} controller=${cfg.CONTROLLER} robot=${cfg.ROBOT_WS} chains=${signer.chains().join(",")} wallet=${settings.get().mode}`);

  const shutdown = async () => { api.close(); agent?.stop(); alerts.stop(); briefing.stop(); robot.close(); await controller.close(); process.exit(0); };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

main().catch((e) => { log.error(e); process.exit(1); });
