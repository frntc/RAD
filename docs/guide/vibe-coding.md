# Vibe-coding a gpu64 game with Claude Code

You describe the game; an AI agent writes the 6502, checks it on a PC, and
leaves a program on an SD card for you to try. This page is the human side.

## What you need

- A PC with Linux or macOS and a terminal.
- `64tass` and `python3` (`sudo apt install 64tass python3`).
- This repository, cloned.
- [Claude Code](https://claude.com/claude-code), signed in.
- A gpu64 cartridge with its SD card, and a card reader for the PC.

You do not need to know 6502 assembly. You do need to be able to run the C64
and tell the agent what you see.

## Setup

```
git clone <this repo> gpu64 && cd gpu64
claude
```

Claude Code reads `AGENTS.md` / `CLAUDE.md` in the repo root, which routes it to
[docs/ai/AGENTS.md](../ai/AGENTS.md). Check it worked by asking:

> What is gpu64, and how do you build and test a program for it?

It should mention the C64/Pi split, `tools/examples.sh`, and pressing `T` to
REU in the RAD menu. If it does not, tell it to read `docs/ai/AGENTS.md`.

## The loop

1. **Say what you want**, in game terms: "a one-screen asteroid shooter, ship
   rotates with A/D, joystick optional, score on the C64 screen, everything
   else on HDMI."
2. **The agent writes it** in a new folder (ask for `games/<name>/`), starting
   from `sdk/template/` or the nearest example.
3. **It simulates it** with `tools/examples.sh`-style runs and looks at the
   rendered frame. Ask to see the picture.
4. **You put the SD card in your PC** and tell the agent where it is mounted:
   "the card is at `/media/me/SIDEKICK`, deploy it."
5. **You put the card back**, power the C64, press `T` in the RAD menu until it
   says `REU`, pick the program, run it.
6. **You tell it what happened**, in plain words. "The ship flickers after
   about a minute." "Nothing on HDMI."

## Prompts that work

- "Build a clone of X, simplest version first. Verify on the simulator and
  show me a screenshot before anything else."
- "Add a second mode that uses the 80x50 text screen for a scoreboard."
- "That worked. Now make it 3D: use the cube example as the starting point."
- "Something is wrong on hardware, here is what I see: ..."

## What to expect

- The agent can be confidently wrong about hardware it cannot see. The
  simulator catches most mistakes; it cannot catch timing on the real bus.
- Very occasionally (one write in 35,000+) the cartridge misses a register
  write. Short programs never notice; the agent knows to design long-running
  games around it. If you see **one** unexplained glitch, say so rather than
  asking for a fix.
- The agent is instructed not to modify the firmware (`Source/`). If a plan
  involves it, stop and decide whether that is what you want.
- Do not ask it to include copyrighted game assets or music in the repo.

## Safety

The agent writes files to the SD card only at the mount point you give it.
Eject the card from the PC properly before moving it back to the cartridge.
