/**
 * Warden engine: one entry point for "what will this signature do, and how careful should we be".
 * Deterministic: decoder + scam filter + explainer (already in this daemon) + the owner's own history.
 * See README.md in this folder.
 */
export { OwnerMemory, looksAlike, type Counterparty, type MemoryState, type ConfirmedSend } from "./memory.js";
export { checkTransaction, checkMessage, recordConfirmed, destinationsOf, historyOf, type History, type WardenVerdict, type WardenLevel, type TransactionInput, type MessageInput } from "./verdict.js";
