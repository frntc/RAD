# gpu64 for AI coding agents

You are helping someone write a program for a **Commodore 64 with a gpu64
cartridge**. This page is everything you need to build it, check it, and get
it onto the hardware. Read it fully before writing code.

## The machine

- The program runs on the **C64**: a 1 MHz 6510 with 64 KB RAM. You write
  6502 assembly (64tass). There is no OS beyond the KERNAL.
- **gpu64** is a Raspberry Pi in the cartridge. It owns a separate HDMI
  screen (320x200 at 256 colours, or 80x50 text) and does graphics work on
  request. It never runs your game logic. Anything that needs a multiply,
  divide, projection or per-pixel loop belongs in a gpu64 command, not in
  6502 code.
- The C64's own VIC-II screen is unaffected and still available.
- The 6502 talks to gpu64 through 25 registers at `$DF50-$DF68`: stage
  arguments in `ARG0-ARG15`, write the opcode to `CMD_LO`. That write **halts
  the C64 until the command finishes**; then read `ERRCODE`.

## Repo map

| Path | Use |
|---|---|
| `sdk/gpu64.inc` | Register and opcode equates, staging macros. Include it. |
| `sdk/gpu64_rt.inc` | Small runtime: `dmInit`, `dmFlip`, `dmText`, `dmStop`, `dmHold`, ... Include it last. |
| `sdk/template/` | A working starter. Copy it. |
| `examples/` | Eleven programs, simple to advanced. Copy idioms from here. |
| `games/` | Large worked examples (class 1 scene graph, level data, sound). |
| `docs/guide/getting-started.md` | Register map, commands, blob descriptors. |
| `docs/api/` | Reference by class: `class0-2d-reference.md`, `class1-3d-mesh-reference.md`, `class2-raster-reference.md`, `error-codes.md`. |
| `tools/examples.sh` | Assemble + simulate + render examples and games. |
| `tools/prgsim/runsim.py` | The simulator it uses. |
| `Source/` | Firmware. **Do not edit** unless asked. |

Look at the closest example before you write anything. If an opcode is not in
`sdk/gpu64.inc`, check the API reference rather than guessing a number.

## Writing code

```asm
	.include "gpu64.inc"
	#basicStub              ; RUN starts at $0810
start	jsr dmInit              ; always first: gpu64 state outlives programs
	...
	.include "gpu64_rt.inc"
```

- Stage arguments with `#argb off,val`, `#argw off,val`, `#argwv off,var`,
  `#argblob off,space,addr,len`, `#id16 val`. Fire with `#cmd OP_X` (class 0)
  or `#cmdc1 C1_X` (class 1).
- Draw into the hidden page, then `jsr dmFlip`. Do not draw to the visible
  page.
- Negative numbers in `.word` do not assemble in 64tass: use `.sint`.
- A loop variable called `a` in `.for` turns later `lsr a` into `lsr $03`.
  Use another name.
- RAD's macros do not parenthesise their arguments; avoid `?:`, `+`, `%` in
  macro arguments.

## The bus drops writes

About one register write in 35,000 to 180,000 never reaches gpu64, with no
error. A short program will not notice. A game that runs for minutes will.
Design for it:

1. A multi-write command can execute on a half-written argument block. For
   anything whose failure would be visible, use the checked-dispatch pattern
   in `games/quake/gpu64_demo_quake3d.a` (`csDispatch`): sequence number in
   `SEQ`, confirm with `SEQACK`.
2. Fence every readback with `SET_DMA_WINDOW` before trusting the bytes.
3. A retained 3D scene remembers every dropped write. Refresh node state on a
   rotation: [docs/api/state-refresh.md](../api/state-refresh.md).
4. Four destructive class 1 opcodes need `ARG15 = C1_KEY` in the same dispatch.
5. If one glitch appears once and cannot be explained, suspect this first.

## Verify before asking for hardware

Hardware time is the human's scarce resource. Everything below runs on a PC.

```
tools/examples.sh cube          # assemble + simulate + render one example
tools/examples.sh -v bounce     # also print the C64 screen
```

For your own program:

```
64tass -I sdk path/to/prog.a -o prog.prg          # never --nostart
python3 tools/prgsim/runsim.py prog.prg --demo --stop-after 400 --ppm=out/prog.ppm
```

Then **look at the PPM** (convert to PNG if your viewer needs it). A black
rectangle in the simulator is a black rectangle on the hardware. Run several
hundred frames: defects have started as late as frame 98. Class 1 programs are
rendered with `--c1-stream` and `tools/hostsim/scenesim`; see how
`tools/examples.sh` does it for `cube`.

Checklist before you say a program is done:

- [ ] Assembles without warnings.
- [ ] Runs 400+ frames in `runsim.py` in both display models (`tools/examples.sh` does both).
- [ ] You looked at the rendered frame and it is what was asked.
- [ ] It ends cleanly on RUN/STOP (`dmStop` / `dmHold`).
- [ ] If it uses class 1 over time: state refresh is in place.

## Deploying to the SD card

The human takes the SD card out of the Pi in the cartridge, mounts it on
their PC, and tells you the mount point. Never guess it, never write to a
device you were not told about.

| What | Where on the card |
|---|---|
| Firmware (only if `Source/` changed) | `SDCARD=<mount> tools/build.sh` writes `kernel_rad.img` |
| A program | `<mount>/RAD_PRG/yourname.prg` |
| A game's REU image | `<mount>/REU/` |

For one program, `cp prog.prg <mount>/RAD_PRG/` is the deployment. Do not
name your own programs `gpu64*`: `tools/deploy_prg.sh` prunes unknown
`gpu64*` files from the card. `SDCARD=<mount> tools/deploy_prg.sh --dry-run`
shows what it would do to this repo's own programs.

Then tell the human exactly what to do at the machine:

1. Put the card back, power on.
2. In the RAD menu, **press `T` until it reads `REU`**. Without this none of
   gpu64 runs, and nothing on screen will say why.
3. Pick the program from the menu and run it.
4. Report what the HDMI screen and the C64 screen show.

The card boots `kernel_rad.img`. If you rebuilt firmware, check the boot log's
build id: stale-firmware deploys have wasted whole test rounds.

## Reading a failed run

- HDMI shows a plain copy of the C64's text screen: no command ever
  succeeded. The cartridge is in mirror mode. Check REU mode and the SD card.
- HDMI frozen, C64 alive: look at `ERRCODE`/`STATUS`
  ([error-codes.md](../api/error-codes.md)).
- A glitch once in tens of thousands of commands: the bus floor above, not
  your code.

## Hard limits

- Do not edit `Source/Firmware/` without being asked; its timing rules are
  binding and violations look like random hardware failure.
- The v1 API is frozen; do not invent opcodes.
- 3D coordinates are 8.8 fixed point; the far plane cannot exceed 127.99
  units. Axes: +x right, +y up, +z away.
- Do not add third-party copyrighted assets (music, game data) to the repo.
