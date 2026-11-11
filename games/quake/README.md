# Quake on gpu64

A first-person Quake level, driven from the C64. It exists to show that the
gpu64 API is complete enough for a real game, not as the purpose of the
project.

The 6502 runs the game: input, player movement, collision against the level's
hulls, monster AI, weapons, doors, sound. gpu64 draws the world (the level is
a retained scene graph, culled by the level's PVS), the monsters and the
weapon sprites.

## What you must supply

**The game data is not included.** You need your own copy of Quake's
`PAK0.PAK` (the shareware episode is enough for E1M1). Put it at
`games/quake/ID1/PAK0.PAK`, then:

```
python3 tools/gen_quakelevel.py ...    # BSP -> gpu64 level (see the script's --help)
python3 tools/pack_levels.py ...       # pack levels into quake.reu
```

Without it, `tools/examples.sh level` is skipped and the others use what is
available. Title music is also not shipped; a silent stub is used unless you
provide `gpu64_tune.bin` (see [games/stunts/README.md](../stunts/README.md)).

## Programs

| File | What it is |
|---|---|
| `gpu64_demo_quake.a` | A walkable room with directional monsters: class 2, `DRAW_WORLD`/`DRAW_THINGS`. |
| `gpu64_demo_quake3d.a` | The same idea on the class 1 scene graph; read it for checked dispatch and the state-refresh ring. |
| `gpu64_demo_level.a` | Loads a whole level from the REU image. |
| `gpu64_demo_game.a` + `gpu64_game_*.inc` | The game: combat, AI, doors, pause menu, title screens, sound. |

PC check: `tools/examples.sh quake3d` renders every frame through the
firmware's own renderer.

Quake is a trademark of id Software. This project is not affiliated with or
endorsed by id Software and contains none of its assets.
