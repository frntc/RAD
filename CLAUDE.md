# AGENTS.md

This repository is the firmware and SDK for gpu64, a graphics add-on for the
Commodore 64. Two kinds of work happen here; find yours:

- **Writing a C64 program (game, demo, tool) that uses gpu64** — the common
  case. Read [docs/ai/AGENTS.md](docs/ai/AGENTS.md). You will work in
  `examples/`, `games/` or your own folder, and never touch `Source/`.
- **Changing the firmware** (`Source/Firmware/`). Read
  [Source/Firmware/CLAUDE.md](Source/Firmware/CLAUDE.md) first. It contains
  binding timing rules for the cycle-critical bus loop; breaking one produces
  failures that no build or code reading will show.

If the task does not say which, it is the first.
