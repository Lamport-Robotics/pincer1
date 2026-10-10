/** The morning newspaper: a few spoken lines, in the order a person wants to hear them. */
import { pnlPct, type Decision, type Position, short } from "./rules.js";

export interface Report { at: number; items: string[] }

export function buildReport(input: { actions: string[]; positions: Position[]; proposals: Decision[]; budgetLamports: bigint }): Report {
  const items: string[] = [];
  const hour = new Date().getHours();
  items.push(hour < 12 ? "Good morning." : "Here's the report.");

  if (input.actions.length === 0) items.push("Quiet night. I didn't trade.");
  else {
    items.push(`${input.actions.length} ${input.actions.length === 1 ? "move" : "moves"} overnight.`);
    for (const a of input.actions.slice(0, 5)) items.push(a + ".");
  }

  if (input.positions.length) {
    const total = input.positions.reduce((a, p) => a + Number(p.costLamports), 0) / 1e9;
    const lines = input.positions.map((p) => { const pct = pnlPct(p); return `${short(p.mint)} ${pct >= 0 ? "up" : "down"} ${Math.abs(pct).toFixed(0)}%`; });
    items.push(`Open: ${lines.join(", ")}. ${total.toFixed(2)} SOL at work.`);
  }

  if (input.proposals.length) items.push(`${input.proposals.length} ${input.proposals.length === 1 ? "thing" : "things"} I want to ask you about.`);
  else items.push("Nothing to ask you.");

  return { at: Date.now(), items };
}
