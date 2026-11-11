# Going public

A plan for publishing gpu64. Written for the maintainer; delete or keep as
history once it is done.

## 1. How to say what it is

One sentence: **gpu64 is a graphics card for the Commodore 64.** The C64 runs
the program; the card draws.

The framing that survives contact with the community:

- The C64 is untouched. VIC-II, SID, CIAs, the 6510 at 1 MHz, 64 KB RAM, the
  KERNAL: all real, all in charge. Every game on gpu64 is a 6502 program on
  a real C64 doing its own input, logic, collision, AI and sound.
- gpu64 is a peripheral, in the same family as REU, SuperCPU, a Mouse, or
  an 80-column card on a C128. It is attached through the expansion port and
  a program has to ask it for anything.
- It does graphics work, and only on request: fill, blit, rotate, texture,
  depth-test, flip. It runs no game code, and it does not take over the
  machine. Take the cartridge out and the C64 is a C64.
- Its screen is a **second, independent output**, the way the C128's 80-column
  display is. It is not an overlay on the VIC-II picture.
- It is a new capability, not a claim about authenticity. Nobody is saying a
  Quake level on a C64 is "what the C64 does"; it is what a C64 plus a
  specific add-on can do, and the add-on is open hardware and open firmware.

### The objection: "that is not a true C64 any more"

Expect it, mostly from people who are right about the thing they are saying.
Do not argue it. Agree with the premise and move the point:

> Agreed: a C64 with a Pi doing the rasterising is not a stock C64, and
> nothing here claims otherwise. The demos are not "this is what a C64 can
> do." They are "this is what a C64 can do when you give it a graphics card,"
> in the same sense as a SuperCPU, a REU or a Mouse. Everything that matters
> in the program still runs on a 1 MHz 6510: the game loop, the controls, the
> sound, the logic. Without the cartridge it is an unmodified C64.

Points to have ready:

1. **Precedent.** REU, GeoRAM, SuperCPU, Final Cartridge, SIDKick, Ultimate
   carts: the scene accepts peripherals that do work the machine cannot. gpu64 is the same kind of thing.
2. **Honest numbers.** State what the 6502 does and what the Pi does, in the
   README and in every demo post. "The C64 sends 11 bytes of camera per
   frame" is a better answer than any argument.
3. **Compo rules are the scene's call.** gpu64 is not for "stock C64" compos
   and does not ask to be. Say so before somebody else does.
4. **It is a creative tool.** The goal is a graphics card for a C64: new
   things to make, not a replacement for old ones. Leave the 8-bit-purity
   crowd to enjoy their stock machines.

Do not: call it a "C64 port" of anything, claim a game "runs on the C64
alone", say "real C64 3D" without the qualifier, or imply the Pi is a
co-processor running the game.

Lead with the examples that make the split obvious: `rotate` (a 6502 that
never multiplies), `palette` (one command per frame) and `cube`, rather than
the Quake game, which invites the "it's just Quake on a Pi" reaction.

## 2. Before the first commit

### Strip (already done in the working tree)

- `They Stole a Million` disk image and everything derived from it: the
  `million/` folder is gitignored and not part of the release.
- `.sid` music files. The tune slot is optional; a silent stub is used when
  `gpu64_tune.bin` is absent.
- All built artefacts (`*.prg`, `*.reu`, `*.lst`) are gitignored.
- Quake game data (`games/quake/ID1/`) is gitignored; users supply it.

### Check by hand

- [ ] **`games/*/gpu64_hondani.bin`, `games/quake/hondani.art`.** `tools/hondani.py`
      says the logo is "the Doom port's ilogo6.art". If that art is not
      yours, it is third-party and needs the same treatment as the `.sid`s:
      remove or replace it before publishing.
- [ ] **Stunt Car Racer.** The game re-implements the original's track layouts
      from a public wiki image. It is a reimplementation with new code and
      meshes; keep the credits (it names the original), keep it clear that
      it is not affiliated, and consider whether to ship it at all under the
      original's name. Rename if in doubt.
- [ ] **Balloon sprite** (C64 User's Guide) in the stunts credits: say so in a
      third-party-assets note or replace it.
- [ ] No personal paths, device names, or credentials (`rg -n '/home/|/media/'`).
- [ ] Licence headers on every gpu64 file (see below).
- [ ] A photo or short video of the cartridge running. Nobody believes a text
      description of a second screen on a C64.

### Licence

- **gpu64 software: GPL-3.0-or-later**, Honza Slesinger. It already is, per
  file headers; the `LICENSE` file now states which licence covers what.
- **RAD** (software GPL-3, hardware CC BY-NC-SA 4.0) is referenced, not
  included. The repo is no longer a fork: the firmware (`Source/`), `Gerber/`
  and `Images/` are gitignored and stay on the maintainer's disk.
- Decide where the firmware lives (a second repo, or release binaries of
  `kernel_rad.img`). Until then the README says it is not part of this
  repository. If it is ever published, it contains RAD-derived files and
  must be GPL-3 with frntc's headers intact.

The header pass is done for everything under `sdk/ examples/ tests/ games/ tools/`.

### Fresh history

The working repository holds months of private history, bench logs and
notes. Publish a single clean commit and keep the old history private.

```
git checkout --orphan public-main
git add -A
git status                          # read it: no .prg/.reu, no million/, no ID1
git commit -m "gpu64 1.0"
git remote add origin git@github.com:<you>/gpu64.git
git push -u origin public-main:main
```

Keep the old repository as a private archive. Tag the first release `v1.0`
(the v1 API surface is frozen).

## 3. Relationship to RAD

The repository only references RAD; it contains none of its code or design.
Credit it in the README, link upstream for hardware, and tell its author
before announcing. If the firmware is published later and he wants the
additions upstreamed, that is a separate discussion with its own patch
series; the timing rules for the polling loop would be the first thing to
explain.

## 4. Where to announce

In rough order of audience fit: Lemon64 and Forum64 (hardware and
development); the RAD and Sidekick64 threads (where the hardware owners are);
CSDb for any release (with a demo, not a description); r/c64; the GitHub
repo's own README.

Post once, with a video, with the split stated in the first paragraph.
Answer the authenticity question calmly and once. Do not argue in
threads; the video does the arguing.

## 5. After going public

- Add a short `CONTRIBUTING.md`: bug reports welcome with a bench log; PRs for
  examples and docs are easy; firmware is out of scope for this repo.
- Issue templates: "bench report" (hardware, firmware build id, REU mode on?)
  catches the most common false report.
- Revisit this file after the first month and delete what the community
  already answered.
