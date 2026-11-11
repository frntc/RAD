# Games

Three complete programs built on the SDK. They are larger worked examples of
what the API supports, not the purpose of gpu64: each one is a 6502 program
running on the C64, with gpu64 doing only the graphics.

| Game | Shows |
|---|---|
| [nighthawk](nighthawk/) | A flight game: 3D scene graph, chase and cockpit cameras, in one file. |
| [stunts](stunts/) | A Stunt Car Racer-style racer: tracks, physics on the 6502, menus, a league. Ships as a PRG plus a `.reu` image. |
| [quake](quake/) | A Quake level with monsters, weapons and doors. Needs your own game data. |

Each game is a PRG plus, for stunts and quake, a `.reu` image of its assets
loaded through RAD's REU. They have no code in the firmware: the firmware is
game-agnostic. Build and check with `tools/examples.sh <name>`; deploy with
`tools/deploy_prg.sh`.
