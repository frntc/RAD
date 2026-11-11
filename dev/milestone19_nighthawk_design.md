# Milestone 19 — NIGHTHAWK, a stealth strike game

A C64 game in the spirit of MicroProse's *F-19 / F-117A Stealth Fighter*,
built on the class-1 retained scene: take off, cross a coastline at low level
through a radar net, put a laser-guided bomb on a target, get home and land.
The PC original drew flat-shaded polygons; this one uses textures where they
buy something a polygon count cannot (terrain, runways, building fronts,
smoke and fire) and keeps flat facets where they are the look (the F-117
itself).

This document is the as-designed record. Status goes in
`dev/progress_tracker.md`, and the user-facing entry goes in
`docs/demos.md` once it exists.

Decided with the user on 2026-09-26: **one theatre with several missions;
a stealth/detection meter; SAMs and fighters; takeoff and landing; day and
night; joystick plus keys; a coastal theatre with hills; the C64's own screen
as the cockpit's MFDs; no firmware change.**

## What is being proved

**The frozen v1 surface can carry a vehicle simulation over open terrain,
not only a corridor world.** `game` and `stunt` both live inside a small
bounded space. A flight sim is the opposite case: kilometres of view
distance, a world far larger than any one mesh, and a camera that can see
most of that world at once from altitude. The question is whether the
far-plane and mesh limits of the 8.8 format, the 256-node table and core 1's
triangle rate leave room for it without new opcodes.

What it does not claim: it is not a flight model, and not a bench result
until it has run on the bench.

## World scale — why no floating origin is needed

The question put to the user assumed a floating origin. Reading the class-1
reference more closely shows that one is unnecessary. **Node positions are
signed 16.16 (±32768 units).** Only three things are 8.8: the far plane,
mesh vertices (±128 units) and the per-frame deltas. So the world is simply
authored at a scale where the far plane is far enough:

| Quantity | Value | Reason |
|---|---|---|
| 1 world unit | **100 m** | far plane 127.99 units = **12.8 km** of view |
| Theatre | 256 × 256 units = 25.6 km square | 8 × 8 terrain tiles |
| Terrain tile | 32 units = 3.2 km, one mesh | fits ±128 easily; see "Terrain" |
| `near` | **0.25 (25 m)** | see "Depth precision" |
| `FOV` | ~70° | narrower than stunt's 99°: a cockpit, not a chase car |
| Cruise speed | ~0.10 units/frame at 25 fps (≈ 250 m/s) | crossing the theatre takes about 100 s |
| Aircraft and missile meshes | drawn at **3× true size** | a real 15 m MiG is 4 px wide at 1 km; the PC game made the same trade |

### Depth precision

The z-buffer stores `near/z` in 16.16 (`gpu64_3d_render.cpp:378`), so two
surfaces at distance `z` separate only if they are more than about
`z² / (near · 65536)` apart:

| `near` | at 2 km (z=20) | at 10 km (z=100) |
|---|---|---|
| 0.05 (5 m) | 12 m | 305 m |
| **0.25 (25 m)** | **2.4 m** | **61 m** |

`near` = 0.25 it is. A 25 m near plane clips nothing a cockpit can see,
because the nose hides the ground that close. This also sets a rule that
shapes the whole world: **there is no coplanar geometry anywhere.** No runway
laid over terrain, and no decals. Every surface owns its own patch of ground
(see "Terrain" and "The airbase").

## The shape of the split

| Lives on the Pi (uploaded once) | Lives on the C64 |
|---|---|
| 64 terrain tile meshes, the ocean, building/aircraft meshes, ~20 textures | Heightmap and tile map (≈6 KB) — also the collision source |
| The scene: every static node, placed once and kept by the refresh ring | Aircraft state, enemy AI, missiles, bombs, the mission script |
| Rendering, lighting, frustum culling (per-mesh bounding sphere, already in the firmware) | Detection, damage and scoring |

### Terrain: the C64 builds it

The terrain is too big to ship as mesh bytes. A tile of 8 × 8 cells is 81
vertices and 128 triangles, which is 2 KB of blobs, and 64 tiles would be
130 KB. So the program carries only the maps, and **the 6502 generates each
tile's vertex and face blobs into one scratch buffer, uploads it, and moves
on**:

- `hMap`: 65 × 65 corner heights, one byte each (1/16 unit = 6.25 m,
  so up to 1.6 km of relief), 4.2 KB. The same table answers
  "how high is the ground here" for landing, crashes and terrain masking.
- `tMap`: 64 × 64 cell types as nibbles, 2 KB — farmland A/B, forest,
  rock, beach, town, road, airbase-hole.
- The cell is 4 units (400 m). Each cell is two triangles with the texture id
  its type names. The UVs run 0..32 on a tileable 32 × 32 texture, so
  neighbouring cells meet seamlessly and a texel is 12.5 m.
- **Sea is not in the terrain at all.** Cells entirely below height 0 are
  skipped, and one *ocean* node — a flat 8 × 8 grid of 30-unit cells, 128
  triangles — follows the camera in whole-cell steps, like stunt's ground.
  The theatre has sea on every side, so there is no edge of the world to fly
  off. Shore triangles dip below 0 and intersect the ocean plane; the
  z-buffer draws the coastline, and no one authors it.
- **Airbase cells are holes** in the terrain, filled exactly by the airbase
  meshes. This follows from the coplanar rule.

Generation on the 6502 is about 3 KB of writes per tile, so of the order of
a couple of seconds for all 64 at start-up, behind a loading bar. Each upload
is believed only when `RESULT` reads back 128 faces, as in stunt.
`tools/gen_nighthawk.py` computes the same blobs in Python, so the host run
can compare them byte for byte.

### Everything else

`tools/gen_nighthawk.py` emits, into `gpu64_demo_nighthawk.inc`: the maps,
the palette, the textures, the object meshes, and the static placement
table.

| Mesh | Style |
|---|---|
| F-117 (player, chase/external views) | flat facets, dark greys: flat shading *is* the F-117 look |
| MiG-29 (×1 mesh, instanced) | flat plus one camo texture |
| Airbase: runway strip, taxiway, apron (flat, owns its cells) | runway texture along the strip (piano keys, centre line, numbers) |
| Hangar, tower, block building, factory hall, chimney, bridge span, radar dish, SAM launcher, fuel tank | textured walls: windows, doors, corrugated metal, brick |
| Day/night variants of the buildings | **shared vertex blob, second face blob with the window faces flagged unlit**, the same trick stunt uses for its two cars |

Sprites (billboards): smoke puff, explosion (two sizes), fire, flare,
missile (a sprite at this range, since it is a few pixels long), and a
target-designator box. Runway and approach lights are **unlit quads inside
the airbase mesh**, not sprites, so they cost no nodes.

The texture budget is about 20 textures × 1 KB, some of them 16 × 16.

### Day and night

Night needs **no palette change** (which would need the loop stopped): it
is `SET_LIGHT` with a low ambient, a dark `SET_BACKGROUND` and the
night face-blob variants. (As built: no face-blob variants, and the light
is a *low moon*, not a dim sun — see step 4 below. `SET_LIGHT` normalises
its direction, so a short vector is not a dim light.) All three are already refresh-ring entries and
legal while the loop runs. Glow comes from unlit faces, unlit sprites, and
the eight point lights: burning targets, explosions, missile motors and the
airbase beacon, switched with `SET_VISIBLE`.

### The HUD on the HDMI screen

The loop owns the framebuffer, so class 0 cannot draw a HUD over it. The HUD
is therefore geometry:

- **Pitch ladder**: one mesh node with rungs at ±5°, ±10° … on a
  0.4-unit sphere. It is placed at the camera position with orientation
  **(yaw, 0, 0)**. Being world-levelled, it is correct under any pitch and
  roll with no arithmetic on the 6502. Unlit, in HUD green. It costs two
  commands a frame, like the camera.
- **Target box**: an unlit sprite at the designated target's world position,
  placed once when the target is designated. It is occluded by hills, and
  that is the point: if you cannot see the box, the laser cannot either.
- Everything numeric (heading, speed, altitude) goes on the C64 screen.

**Superseded in stage C by the user:** the C64 screen is out of sight when
you watch the HDMI one, so the whole cockpit moved to HDMI. As built, one
static mesh (`MESH_HUD`) is posed on the camera every frame, so its model
space is the camera's: a 40 × 6 character panel along the bottom of the
view, cut into 60 segments of 4 characters, each its own 32 × 8 texture,
plus a 16 × 16-texel tactical map top right. The C64 renders a segment's
texels from rows 0–5 of its own screen, with its own character ROM and
colours, whenever that segment's text or colour changes, and re-uploads it
with `UPLOAD_TEXTURE`. The panel code writes those rows exactly as before;
the HDMI copy is a side effect. One segment in `HUD_REPAIR` (16) frames is
re-sent unasked, because a lost reply reads as OK.

**The cost is arena.** A re-upload cannot free the old copy, so every
upload leaks 256 bytes. At most one segment every other frame plus the map
once a second is 3.4 KB/s: about two and a half hours of the 32 MB. The
first `OUT_OF_MEMORY` stops HUD uploads for good (row 7 reads `DEAD`), and
the HDMI cockpit freezes on its last picture while the game goes on. Fixing
that needs a texture *replace* opcode, i.e. a firmware change, which this
milestone rules out.

## The C64 screen: the cockpit panel

(As built after stage C, rows 0–5 are the panel the HDMI HUD mirrors, and
everything below is debug: row 7 the HUD uploads, 8 landings/crashes, 9
the missile slots and the last AIM-9's closest approach, 12–13 the extra
keys and the mission line, 15–21 position, timing, setup and the bus report
card, 22–24 the keys. The sketch below is the original design.)

40 × 25 text, redrawn a group of rows a frame round-robin, as stunt's
`showHud` does.

```
 NIGHTHAWK   MISSION 3  NIGHT    TIME 04:12
 +----------------+  EMV [#####.........]
 |    TACTICAL    |  ALT  0450 M   SPD 480
 |  map 16x16     |  HDG  274     THR 70%
 |  ^ you         |  GEAR UP  BAY CLOSED
 |  o SAM ring    |  GBU-27 x2   AIM-9 x2
 |  * target      |  CHAFF 10    FLARE 10
 |  M MiG         |  THREAT: SA-6 LOCK
 +----------------+  DMG [..............]
 > SAM LAUNCH! 5 O'CLOCK
 (bus report card, rows 19-22, as in stunt)
```

The tactical map is a 16 × 16 character window onto the theatre, centred on
you. It shows radar circles, SAM sites, the target, MiGs and waypoints. It is
redrawn a quarter per frame.

## Gameplay

### Flight

The model is arcade, not aerodynamics. The state is position (24-bit
fixed per axis), yaw/pitch/roll as binary angles, and speed.

- The stick controls roll rate and pitch rate. A bank angle turns the
  aircraft: `yawRate ∝ sin(roll)`. Throttle sets the target speed. Pitching
  up bleeds speed, and below the stall speed the nose drops.
- **Pitch is clamped to ±80°.** Class 1 composes `Ry·Rx·Rz`, which is
  exactly heading/pitch/bank, and passes straight to `SET_ORIENTATION` with
  no conversion, but Euler angles have a pole at ±90°. An F-117 is not an
  aerobatic aircraft, so this is a stated limitation, not a bug.
- Velocity is `forward(yaw, pitch) × speed`, from stunt's sin/cos tables
  and `smul`.
- The ground is `hMap` interpolated across the cell's triangle. Contact
  with the gear down, on the runway rectangle, with a small roll, below the
  landing speed and below the sink limit, is a landing, graded by sink rate.
  Anything else is a crash.
- Takeoff: on the runway, full throttle, and rotate above the takeoff speed.

### Stealth — the EMV meter

This is the core of the game and costs almost nothing on the 6502. Each
radar has a nominal range. **You are detected when
`distance < range × signature / 16`** and the radar has line of sight.
Signature is 0..16 and is shown on the C64 screen as the EMV bar:

| Factor | Effect |
|---|---|
| base | 4 |
| bay doors open (automatic while a bomb is armed) | +8 |
| gear down | +3 |
| throttle above 80% | +2 (IR, which matters to fighters) |
| altitude below 150 m AGL | range halved: under the radar horizon |
| a hill between you and the radar (`hMap` sampled at 4 points on the line) | not detected |

Detection raises a per-sector **alert**. Alert 1: SAM sites in the sector
track you. Alert 2: they launch, and the enemy airfield scrambles MiGs.
Alert decays slowly if you get out of sight.

### Enemies and weapons

- **SAM sites** (4–6): a launcher mesh, a radar dish that rotates (one
  `SET_ORIENTATION` every few frames, only when on screen), and a missile
  when alerted. Missiles use proportional-lite guidance: turn-rate-limited
  pursuit, with a limited burn time and a smoke puff every 4 frames.
- **MiGs** (up to 4 airborne, 2 of them "near" at a time): take off from
  the enemy field, pursue, and fire one missile each. The rest of the AI is
  turning towards you with a turn-rate limit.
- **Countermeasures**: chaff against radar SAMs and flares against IR
  missiles. Each missile rolls against the decoy once, when it is launched
  close enough.
- **Player weapons**: 2 × GBU-27 laser-guided bombs, plus 2 × AIM-9 for
  self-defence. A bomb is ballistic with a small steering authority toward
  the designated point, as long as the target box is still in line of sight.
- **Bomb cam** (a view key): the camera sits at the target looking up the
  bomb's path, as in the original.

### Missions

The missions run on one theatre, chosen on the C64 before the loop starts:

1. **Checkride** (day): take off, fly three waypoints, land. No enemies.
2. **Radar** (day): destroy the coastal early-warning radar. Taking it out
   removes one radar circle for the rest of the mission.
3. **Bridge** (night): drop the bridge span inland, behind two SAM sites.
4. **Factory** (night): deep strike past the enemy airfield. MiGs are
   already airborne.

The debrief shows the target hit or missed, whether you were detected (the
peak alert), the landing grade, and a score. **Mission 0 is the autopilot**,
which flies mission 2 end to end. It is the attract mode, and it is what the
PC gate drives.

### Views and keys

| Input | Action |
|---|---|
| Joystick 2 | pitch/roll; FIRE releases the selected weapon |
| `+`/`-` or `W`/`S` | throttle |
| `G` | gear |
| `B` | bay doors (manual override) |
| `SPACE` | cycle weapon |
| `C` / `F` | chaff / flare |
| `T` | designate the next target |
| `F1` | cockpit / chase / bomb cam |
| `F3` | autopilot |
| `RUN/STOP` (held) | quit |

Keyboard pitch/roll (`I J K L`) exists as well, so the PC run can fly
without a joystick model.

## The per-frame bus budget

stunt sends 7 commands a frame, and `game` runs at about 32 fps on the bench.
Every class-1 command here goes through stage2 (written twice, with a check
byte), so commands are what cost time, not bytes. Budget:

| Every frame | Commands |
|---|---|
| camera position + orientation | 2 |
| pitch ladder position + orientation (cockpit view) or player jet (chase view) | 2 |
| refresh ring step | 1–2 |
| `SCENE_COMMIT` | 1 |
| **base** | **6–7** |
| each "near" MiG or missile, pose | +2 / +1 |
| smoke: move the oldest puff of a ring of 12 | +1 while anything is smoking |
| ocean snap, radar dish, explosion growth | +1 occasionally |

**Hard cap: 12 commands a frame.** A scheduler hands out the slots above the
base by priority: nearest threats every frame, farther ones every second or
fourth frame. Anything beyond 12.8 km is invisible anyway and is not sent at
all. Target: **20–25 fps** with a fight on screen.

## Robustness: stunt's discipline, unchanged

- Every class-1 command goes through `stage2` with `GPU64_KEY_CHECKED`, and
  every send is absolute (`SET_POSITION`/`SET_ORIENTATION`, never
  `MOVE_LOCAL`); see memory notes *camera must be absolute* and
  *checked-command key*.
- The refresh ring covers the scene state, 64 tiles, every static node,
  and every dynamic node's existence **and visibility**. The period is about
  150 entries, so roughly 6 s at 25 fps.
- `SET_DMA_WINDOW` fence, `SEQ`/`SEQACK` on the commit, and the bus report card on rows
  19–22.
- Game rules never branch on a value read back from gpu64. Nothing is read
  back during flight at all. Ground height, hits and detection are
  computed on the C64 from its own tables.

## Node and memory budgets

| Nodes | Count |
|---|---|
| camera, ocean, pitch ladder, player jet | 4 |
| terrain tiles | ≤ 64 (fewer: all-sea tiles do not exist) |
| airbase ×2, buildings, bridge, radars, SAM sites | ~50 |
| MiGs ×4, missiles ×6, bombs ×2 | 12 |
| smoke ×12, explosions ×3, flares ×2, target box | 18 |
| point lights | 8 |
| **total** | **~160 of 256** |

C64 RAM, with BASIC banked out to give `$0801-$CFFF` (~50 KB): program ~16 KB, maps
6.2 KB, textures ~18 KB, object meshes ~8 KB, the tile scratch buffer 2 KB
and tables 1 KB, so **~51 KB. That is tight.** If it overflows, the fallback
is to generate the tileable terrain textures on the 6502 too (they are noise
plus a pattern), which saves about 6 KB.

## Verification before the bench

1. `tools/gen_nighthawk.py`: asserts vertex ranges, winding against an
   outward hint (stunt's `Mesh.tri`), face counts ≤ 255, and the RAM map.
2. `tools/examples.sh nighthawk`: prgsim runs the autopilot mission for a
   **full mission length, 2000+ frames** (see *VICE runs must be long
   enough*), with scenesim rendering every frame.
3. `tools/check_nighthawk.py` asserts from the sim's final C64 RAM: the tile
   blobs equal the Python generator's byte for byte, the autopilot took off,
   the bomb hit, the alert stayed 0 on the low-level route, and it landed. A
   picture cannot judge "landed", in the same way that `check_game.py` exists
   because a picture cannot judge "the door moved".
4. The scenesim triangle count per frame at 1500 m altitude looking at the
   whole theatre sets the **core-1 triangle budget**, the one number this
   design cannot predict from reading. If it is too slow, the fallback is a
   second, 2 × 2-cell LOD mesh per tile, swapped by `CREATE_OBJECT` on the
   same node, with the ring re-creating whichever LOD is current.

Tooling change (done 2026-09-26): `runsim.py` used to return the written
`$DC00` for a read of `$DC00`, so it could not model joystick 2. It now takes
`--joy=DIR[+DIR]:FIRST-LAST`, and a held direction also grounds that
keyboard column, which is the ghosting a real port 2 causes.

### Step-1 result: GO, with the full view distance

Measured 2026-09-26 with `gen_nighthawk.py --probe` (the whole terrain plus
the ocean, 450 frames: a low pass, a climb to 1500 m, then an orbit looking
across the theatre), rendered by `scenesim --time`. The worst frame was
**1.1 ms** of core-1 render time. The E1M1 stream measured **3.5 ms mean** on
the same PC, and E1M1 ran at about 32 fps on the bench, bound by the C64, not
by core 1. So the terrain costs under a third of a load that is already known
to be fine. The far plane stays at 12.8 km, with no fog and no LOD meshes.
`scenesim --time` is a relative measure only: compare streams on one PC.

### Step-2 as built: the flight stage

- **The sine tables see only the angle's high byte**, so any attitude
  below 1.4° is zero.
  - Every controller that has to hold a small angle must command at
    least 256: the flare pitch is `FLARE_PIT = 256`.
  - The centreline and bank-hold loops live with a 1.4° dead zone. At
    touchdown that is a residual roll of 128 and about 10 m of lateral
    error, which is acceptable.
- **Autopilot geometry is on half deltas.** World positions are 8.16 over
  256 units, so a raw difference needs 17 bits. `apSteer` halves dx and dz
  (with the borrow as bit 16) before anything else touches them.
- **The C64 screen layout as built:**
  - the 16×12 tactical map at columns 0–15, rows 2–13;
  - instruments at column 17, rows 2–8;
  - the message line at row 14;
  - debug rows 15–18 (position, frame time, ring, setup and tiles);
  - the bus report card at rows 19–21;
  - keys at rows 23–24.

  This is the user's "a bit of the cockpit for debugging". Stage B put
  the EMV/weapons lines in rows 9–13 beside the map, and the keys in rows
  22–24.

### Step-3 as built: radars, SAMs, the strike

Where it departs from the design above:

- **The alert is theatre-wide, not per sector.** There is one byte.
  - Past 64 the HUD reads TRACK. It is a warning only; nothing changes in
    the sites.
  - Past 128 a battery launches, but only if that battery itself sees you.
    The alert alone does not launch anything.
- **Chaff rolls when it is dropped, not when a missile is launched.** Each
  cloud rolls against every locked missile within 12 units per axis, so
  dropping early wastes cartridges. A decoyed missile flies on straight and
  cannot fuse. The design's "fly-through" hit would make chaff useless at
  short range, and that is exactly when the player uses it.
- **The GBU is unguided.** Steering toward the designated point overshot
  by tens of units with the per-frame authority available. Ballistics from
  the jet's own velocity hit the site when the drop follows the cue, and
  the cue (`DROP`) comes from the same ballistic prediction.
- **Mission 2 is the autopilot's mission too.** The autopilot opens the
  bay 10 units out and drops on the cue. Its route stays outside every
  SAM's closed-bay range, so the gate sees radars and the alert but no
  launch.
- **Cosmetics yield first.** Dish turns, explosion growth and smoke skip a
  frame once `cmdCount` reaches `LOWPRI` (14). The ring re-places every
  site, dish, missile, bomb and puff.
- **RAM.** The program is $0801–$B32F, which pushed it past $A000, so
  BASIC is banked out while the demo runs. Stage C has about 7.2 KB left
  below $D000, and the REU is untouched.

### Step-4 as built: MiGs, AIM-9s, flares, bomb cam, night, missions 3–4

Where it departs from the design above:

- **MiGs share the missile slots.** Slots 0–1 of the eight are MiGs
  (`misKind` MK_MIG), 2–7 are SAMs, MiG IR missiles and AIM-9s. One
  per-kind table row sets speed, turn rate, burn, fuse and map colour, so
  `misStep`, `misMove`, `misGuide` and the ring serve all four kinds. A
  MiG is a missile that never burns out, flies at 140/1024 units a frame
  and turns at half a SAM's rate.
- **MiG AI is CAP or hunt.** A MiG orbits a combat-air-patrol point (state
  2) until the jet is inside 8 units or the alert is past TRACK, then hunts
  (state 1, one-way). A hunting MiG fires one IR missile from inside 6
  units when the jet is ahead of it, then reloads for 250 frames. It pulls
  15° up below 1.5 units above the ground. Mission 4 starts with two on
  CAP over the factory; with the alert past LAUNCH, the enemy field at
  BASE1 scrambles more, one every 250 frames, `migLeft` of them.
- **Flares are chaff for IR** — the same `dcDrop`, rolling against missiles
  of its own kind only — but break a lock 224/256 rather than 150/256. At
  150 the autopilot, which drops one every 16 frames once a missile is
  inside 6 units, was shot down by a second IR shot 17% of the time.
- **The AIM-9 needs a proximity fuse.** With a SAM's 3°/frame turn and
  0.375-unit fuse it closed to 0.625 units of a manoeuvring MiG (measured,
  `mdMax` on row 9) and then orbited it for 100 frames until it burnt out:
  a 3.6-unit turn radius cannot close the last unit. At 6°/frame and a
  0.875-unit fuse it kills from the first shot. Lock needs a MiG ahead
  within 8 units either way (`findLock`); `M` fires, and the autopilot
  fires whenever it has a lock and no AIM-9 is flying.
- **Night is a low moon.** `SET_LIGHT` normalises its direction, so the
  first night light — the day sun's direction at a quarter the length —
  lit the ground at full brightness under a black sky. Night is now a
  moon at about 10° elevation, (200, 36, 80) with ambient 1: flat ground
  sits near level 3 of 15, and slopes facing the moon catch it. The runway
  and bridge lamps are unlit faces and stay bright. No palette change.
- **Bomb cam (F5)** puts the camera behind and above the falling GBU,
  looking down its path at 45°, and holds on the impact while the
  explosion burns, then returns to the selected view.
- **Missions 3 and 4 are night and on the autopilot.** Mission 3's route
  keeps low under the radars and is never seen (`PEAK 000`), so its MiGs
  never scramble. Mission 4 flies high past the CAP and has to fight: in
  the gate the autopilot shoots both MiGs down (`K 2`) and flares the one
  IR shot it draws.
- **RAM:** $0810–$BA8C with the variables under the KERNAL at $E000 and
  `$01=$35`; about 5.5 KB left below $D000. The REU is still unused.

## Risks, in order

1. **Core-1 fill and triangle rate at altitude.** The whole theatre is in
   view at 12.8 km, which is a very different load from E1M1's corridors.
   Measured in step 4 before anything else is built on it.
2. **C64 RAM.** Budgeted above, with a fallback.
3. **Frame rate under a dogfight.** The command cap and priority scheduler
   hold the frame rate.
4. **The bus floor.** Handled by stunt's machinery. A lost pose is
   overwritten the next frame, and a lost create is repaired by the ring.

## Sequence

1. Generator plus the terrain and ocean only, with a scripted camera flying
   over it. Measure step 4. **This is the go/no-go on the scale.**
2. Flight model, takeoff/landing, the C64 MFD screen, the pitch ladder.
3. Radars, the EMV meter, SAMs, the first mission.
4. MiGs, weapons, bomb cam, missions 3–4, night. (Done, PC-verified.)
5. Autopilot mission, `check_nighthawk.py`, `docs/demos.md`, then the bench.
