# Brand

The mark is an L built from bars: a stack that thins upward and a row of bars that rise, on a base. Bone white
`#F2EEE3` on black `#0A0A0A`, nothing else. Wordmark: Manrope Bold, wide tracking; "ROBOTICS" in Manrope Medium,
wider still, in warm grey `#9A968C`.

| file | use |
|---|---|
| `lamport-mark.svg` | the mark on its black square (favicons, app icons) |
| `lamport-mark-transparent.svg` | the mark alone, for placing on dark surfaces |
| `lamport-lockup.svg` | mark + wordmark, horizontal |
| `display-800x480.svg` | the robot's front panel idle screen |
| `og-1200x630.svg` | social preview for the repository and site |
| `source-lamportlogo.jpg` | the original raster the vector was traced from |

`pnpm install && pnpm render` regenerates every PNG here and the browser extension's icons. Fonts in `fonts/`
are Manrope and Geist Mono under the SIL Open Font License.

Rules: no gradients, no glow, no outline, never on a light background without the black square, never
stretched, clear space around the mark equal to the base bar's height.
