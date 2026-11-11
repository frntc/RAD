# gpu64

**A graphics card for the Commodore 64.**

gpu64 is a cartridge that gives a C64 a second, independent screen on HDMI,
and a command interface to draw on it. It is built on the
[RAD](https://github.com/frntc/RAD) board (a Raspberry Pi 3A+ on the
expansion port); gpu64 is the firmware and the SDK; this repository is the SDK, examples and documentation.

The C64 keeps its own VIC-II output and its own screen. The HDMI screen is a
separate display, the way the C128's 80-column screen is: 320x200 in 256
colours from a 24-bit palette, or an 80x50 text mode. The 6502 draws on it
by writing a few bytes to I/O registers at `$DF50`.

## What runs where

Your program runs on the C64: 1 MHz 6510, 64 KB RAM, your own code, your own
game logic, your own input. gpu64 does none of that. It only does graphics
work you ask for, much like a PC graphics card: fill, blit, rotate, texture,
shade, depth-sort, flip pages. A 3D game on gpu64 is a 6502 program that
moves a few numbers a frame and sends them over; the Pi draws the picture.

That split is the point. The things a 6502 cannot do in 20 ms (multiply,
divide, project, rasterise) are the things gpu64 does. Everything else is
still the C64.

## What it is good for

- **A second screen.** A map, a debugger, a tool palette or a 80x50 text
  editor on HDMI while the C64's own screen shows something else.
- **256-colour 2D** for games and demos: sprites by blit, palette animation,
  tear-free double buffering.
- **A maths coprocessor** for matrices and vectors, on its own or driving
  graphics.
- **3D.** A retained scene graph: upload meshes, create objects and a
  camera, move them with one command each per frame; lighting, depth and
  texturing are gpu64's.
- **Doom-style raster layers** for levels with sectors, portals and sprites.

Programs are ordinary 6502 programs that write to I/O. Three finished games in [`games/`](games/) show
how far it goes, but they are examples of use, not the purpose.

## Hardware

- A [RAD cartridge](https://github.com/frntc/RAD) (design and build instructions are upstream) with a Raspberry Pi 3A+.
- A C64 or C64 Ultimate. Tested on a C64 Ultimate and on an original
  short-board C64.
- An HDMI display and an SD card.

gpu64 runs inside RAD's REU mode. Its registers sit at `$DF50-$DF68`,
clear of the REU's own `$DF00-$DF0A`.

## Quick start

Got a RAD cartridge? Download the release, unzip it onto the SD card, press
`T` until the RAD menu says `REU`, and run `gpu64_demo_hello`. Step by step:
[docs/guide/install.md](docs/guide/install.md). Every program is also on a
`.d64` (`LOAD"HELLO",8,1`).

To write your own, copy [`sdk/template/`](sdk/template/) and edit it.

## Examples

Eleven small programs from "draw a rectangle" to "spin a lit 3D cube",
described in [`examples/README.md`](examples/README.md). You do not need the
hardware to start: every example runs in a PC simulator that renders what the
HDMI screen would show.

```
tools/examples.sh cube        # assemble, simulate, write out/examples/cube.ppm
```

## Documentation

| | |
|---|---|
| [docs/guide/getting-started.md](docs/guide/getting-started.md) | Registers, how a command works, your first program |
| [docs/api/](docs/api/) | Reference: 2D, 3D scene graph, raster layer, errors |
| [docs/guide/vibe-coding.md](docs/guide/vibe-coding.md) | Setting up an AI coding agent to build gpu64 programs |
| [docs/ai/AGENTS.md](docs/ai/AGENTS.md) | What an AI agent needs to know (build, simulate, deploy) |
| [dev/](dev/) | Design notes and project history |

## Things worth knowing before you build on it

The C64 bus is not perfectly reliable here: about one register write in
35,000 to 180,000 is not seen by the Pi, with no error. The API is designed
for that (sequence numbers, checked commands, readback fences, a refresh
ring for retained scenes) and the reference tells you which commands need
which protection. The [guide](docs/guide/getting-started.md) and
[state-refresh](docs/api/state-refresh.md) explain what to do.

The v1 API surface is frozen: opcodes and argument layouts will not change
meaning.

## Firmware

gpu64's Raspberry Pi firmware is built on RAD and Circle and is not part of
this repository; it ships in each release. This repository holds everything you
need to write programs for it. Maintainers: `tools/release.sh`.

## Layout

```
sdk/        include files for your programs (+ template/)
examples/   small programs, read in order
games/      larger programs built on the SDK
docs/       guide and API reference
tests/      conformance suite (self-verdicting PRGs)
tools/      assembler wrappers, PC simulators, deploy scripts
dev/        design history
```

## Licence and credits

GPL-3.0-or-later, © 2026 Honza Slesinger. See [LICENSE](LICENSE).

gpu64 builds on [RAD](https://github.com/frntc/RAD) by Carsten Dachsbacher (frntc), whose hardware
design is CC BY-NC-SA 4.0 and whose software is GPL-3, and on
[Circle](https://github.com/rsta2/circle) by R. Stange. Commodore 64 is a
trademark of its owner; this project is not affiliated with Commodore. The
Quake example needs your own copy of the game data and ships none.
