// Renders every PNG in brand/ (and the extension icons) from the SVG sources. `pnpm install && pnpm render`.
import { Resvg } from "@resvg/resvg-js";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..");
const fonts = { fontFiles: ["Manrope-Bold.ttf", "Manrope-Medium.ttf", "GeistMono-Medium.ttf"].map((f) => join(here, "fonts", f)), loadSystemFonts: false, defaultFontFamily: "Manrope" };
const render = (svg, out, width) => writeFileSync(out, new Resvg(readFileSync(join(here, svg), "utf8"), { fitTo: { mode: "width", value: width }, font: fonts }).render().asPng());
for (const w of [16, 32, 64, 128, 180, 256, 512, 1024]) render("lamport-mark.svg", join(here, `mark-${w}.png`), w);
render("lamport-lockup.svg", join(here, "lockup-1600.png"), 1600);
render("display-800x480.svg", join(here, "display-800x480.png"), 800);
render("og-1200x630.svg", join(here, "og-1200x630.png"), 1200);
for (const w of [16, 48, 128]) render("lamport-mark.svg", join(root, "extension/public/icons", `icon${w}.png`), w);
const b64 = readFileSync(join(root, "extension/public/icons/icon128.png")).toString("base64");
writeFileSync(join(root, "extension/src/generated/icon.ts"), `// Lamport Robotics mark, 128 px PNG. Regenerate with \`pnpm render\` in brand/.\nexport const ICON_DATA_URI = 'data:image/png;base64,${b64}' as const;\n`);
console.log("rendered");
