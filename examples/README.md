# Examples

Small, heavily commented programs, in the order to read them. Each is one
`.a` file (64tass) that includes [`sdk/gpu64.inc`](../sdk/gpu64.inc), does
one thing, and ends on RUN/STOP. None depends on another.

| # | Example | What it teaches | Opcodes |
|---|---------|-----------------|---------|
| 1 | `hello` | The first program: issue a command, read the result. Clipping is not an error. | `CLEAR`, `RECT_FILL`, `RECT`, `LINE`, `SET_PIXEL`, `SET_BORDER` |
| 2 | `text80` | The 80x50 text screen — a display mode, not a drawing op. | `TEXT_MODE`, `TEXT_WRITE`, `TEXT_FILL`, `TEXT_UPLOAD` |
| 3 | `palette` | 256 colours; animation with no pixel writes — one command a frame cycles the palette. | `PAL_LOAD`, `PAL_SET` |
| 4 | `sprites` | Blitting: opaque, keyed, and save/restore for moving a sprite over a background. | `BLIT`, `BLIT_KEYED`, `READ_RECT` |
| 5 | `bounce` | **The animation loop to copy**: draw into the hidden page, flip on a frame boundary. | `SET_DRAW_PAGE`, `PAGE_FLIP`, `VBLANK_SYNC` |
| 6 | `rotate` | gpu64 as a maths coprocessor: a shape scaled and rotated each frame; the 6502 never multiplies. | `MAT_SCALE`, `MAT_MUL` |
| 7 | `matrix` | The same maths with every answer printed, including 8.8 vs float32 and two error paths. | `MAT_*`, `FLT_*` |
| 8 | `cube` | **First 3D**: upload a mesh, make a camera and an object, spin it with one command a frame. | class 1: `UPLOAD_MESH`, `CREATE_OBJECT`, `SCENE_COMMIT` |
| 9 | `raycast` | A textured first-person view at 320x160; the 6502 marches rays, gpu64 draws. | class 2: `DRAW_COLUMNS`, `DRAW_SPANS` |
| 10 | `walls` | No raycaster at all: 162 world-space wall segments, a frame is ten bytes of camera. | class 2: `DRAW_WALLS` |
| 11 | `sectors` | Floors, ceilings, steps, two-sided portals and depth-tested sprites. | class 2: `DRAW_SECTORS`, `DRAW_THINGS` |

`nighthawk` is a further, larger 3D example (a flight game in one file) that
lives in [`games/nighthawk/`](../games/nighthawk/).

## Run them on the PC first

```
tools/examples.sh              # build and simulate all of them
tools/examples.sh -v bounce    # one, and print the C64 screen it produced
```

Each program runs in a 6502 + gpu64 model; the last HDMI frame is written to
`out/examples/<name>.ppm`. Look at it before you copy anything to an SD card.
Class 1 programs (`cube`, the games) also leave every rendered frame in
`out/examples/<name>.c1/`.

## Build one by hand

```
64tass -I sdk examples/gpu64_demo_hello.a -o hello.prg
```

Never pass `--nostart`: it removes the load address and the program silently
never runs. Copy the `.prg` to the SD card's `RAD_PRG/`, or load from `gpu64-examples.d64`: `LOAD"HELLO",8,1`
and `RUN`. RAD must be in **REU** mode (press `T` in its menu until it says
REU) or none of gpu64 runs.

## Where next

- The register map and how a command works:
  [docs/guide/getting-started.md](../docs/guide/getting-started.md)
- Your own program: copy [`sdk/template/`](../sdk/template/) and edit.
- Whole games: [`games/`](../games/).
