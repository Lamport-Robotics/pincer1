/**
 * Where the daemon keeps Warden's owner memory: one JSON file, one history per signing address.
 *
 * Not exported from index.ts: this is the one file of the engine that touches the disk, and the browser
 * extension builds from the same folder.
 *
 * Two rules:
 *   - A save writes every history the file held, not only the ones used since the daemon started. The decoy
 *     wallet and the real one share the file, and signing with one must never erase what the other learned.
 *   - The file is replaced whole (write beside it, then rename), so a power cut in the middle of a save leaves
 *     the old file, never half of a new one.
 */
import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";
import { OwnerMemory } from "./memory.js";

export interface MemoryFile {
  /** The history of this signing address (empty when it has none), and the call that persists every history. */
  of(owner: string): { memory: OwnerMemory; save: () => void };
}

export function openMemoryFile(file: string, onUnreadable?: (err: string) => void): MemoryFile {
  let stored: Record<string, unknown> = {};
  try {
    if (existsSync(file)) {
      const j: unknown = JSON.parse(readFileSync(file, "utf8"));
      if (j && typeof j === "object" && !Array.isArray(j)) stored = j as Record<string, unknown>;
      else onUnreadable?.("not an object");
    }
  } catch (e) {
    onUnreadable?.(String((e as Error).message ?? e));
  }
  const open = new Map<string, OwnerMemory>();
  const save = () => {
    const all: Record<string, unknown> = { ...stored };
    for (const [owner, memory] of open) all[owner] = memory.toJSON();
    mkdirSync(dirname(file), { recursive: true });
    const tmp = `${file}.tmp`;
    writeFileSync(tmp, JSON.stringify(all), { mode: 0o600 });
    renameSync(tmp, file);
  };
  return {
    of(owner) {
      let memory = open.get(owner);
      if (!memory) { memory = OwnerMemory.from(stored[owner]); open.set(owner, memory); }
      return { memory, save };
    },
  };
}
