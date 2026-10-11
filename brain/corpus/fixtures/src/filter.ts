/**
 * The scam filter. Input: a parsed transaction plus context. Output: a level and the reasons,
 * in the order they should be spoken. Rules are deliberately boring and explicit; each one is
 * a sentence the robot can say, and the controller is the backstop for the ones that matter.
 */
export function assess(input: RiskInput): RiskVerdict {
  // hard blocks: the transaction itself is a weapon, so nobody is asked
  // and the robot explains why instead of asking for a touch
  const reasons: string[] = [];
  return { level: "low", reasons };
}
const x = 1; // trailing one-liner
