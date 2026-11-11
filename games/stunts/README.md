# Stunt Car Racer for gpu64

A Stunt Car Racer-style game that runs on gpu64's retained 3D scene (class 1).
Like `quake/`, it is not part of gpu64 itself. It drives the API as a real
product would.

## Files

| File | What it is |
|------|------------|
| `gpu64_demo_stunt.a` | The game (64tass). It `.include`s the SDK's `gpu64.inc` and `gpu64_rt.inc` (`-I sdk`). |
| `gen_stunt.py` | Generates `gpu64_demo_stunt.inc`: track meshes, scenery, cars, palette, textures and physics tables. |
| `gpu64_demo_stunt.inc` | Generated. Do not edit by hand. |
| `gpu64_demo_stunt.prg` | Built program. |
| `stunt_title.inc` | The intro: the HONDANI logo with its light sweep on both screens, then the start screen with the credits. Assembled at `$c000`. |
| `stunt_league.inc` | The menus and the league: main menu, practice, the division table, race results, end of season, controls. Every screen is `LEVEL_PICTURE`s from `stunts.reu`, the text one 9x9 glyph picture per character. |
| `stunt_sfx.inc` | The race's SID sound: engine pitch from speed (voice 1), boost hiss or brake squeal (voice 2), crash/car-to-car crunch/landing/countdown/lap one-shots (voice 3), all run once a frame from the race loop. |
| `stunt_tracks.py`, `stunt_cockpit.py`, `stunt_pics.py` | `gen_stunt.py`'s parts: the eight track layouts, the cockpit sprites and textures, and the `stunts.reu` pictures (title art, menu frame, track previews, headings, three glyph sets). |
| `PLAYING.md` | The player's guide: installing, starting, controls, league rules. |
| `gpu64_hondani.bin` | The RLE-packed logo, a copy of `quake/`'s (`tools/hondani.py`). |
| `gpu64_tune.bin` | Title music, **not shipped**: a SID tune relocated to `$a000` by `tools/sidreloc.py` (init `$a000`, play `$a003`, zero page `$fc-$fe`). `tools/examples.sh` substitutes a silent stub (`sdk/tune_silent.bin`) when this file is missing; supply your own tune to hear music. |

Build and PC-check: `python3 games/stunts/gen_stunt.py && tools/examples.sh stunt`.
`tools/examples.sh` builds it with `-I sdk` and generates `stunts.reu` on demand.

Memory: the game's code ends below `$a000`; the tune sits at `$a000`, the
logo at `$b490` and the logo's HDMI strip buffer at `$bc00`, all under
BASIC, which the title banks out. The title code is at `$c000-$cbff`, and
the logo unpacks into VIC bank 3 (`$cc00`/`$e000`).

`runsim.py` skips the title unless given `--title` (the `$02ab` flag, as
for Quake). With the title skipped the menus are skipped too: the program
races every track in turn from `--level-slot` against the fastest rival,
which is what `tools/examples.sh stunt` and per-track checks run. With `--title` it
goes through the menus; runsim's `--key`/`--joy` frames are page flips, and
a menu flips only when it changes and once a second, and `--ppm-frame`
captures the menu pages (race frames need `--c1-stream` and
`tools/hostsim/scenesim`). F3's autopilot is refused in league races except
under runsim (`$02aa` = `$aa`).

## Target: the shippable game (agreed 2026-10-03)

- **Look.** The original's, from the user's C64 screenshots (2026-10-03):
  - a flat blue sky over a mountain range on the horizon. The original's
    grey hills were replaced on 2026-10-04 by a rendered panorama
    (`stunt_mountains.py`): snow-capped ridged-fractal peaks, ray-cast from
    300 m, lit by a low sun and hazed with distance. It is dithered into
    the game's own palette and drawn unlit on a 16-panel ring that follows
    the camera's height.
  - three hot-air balloons in the sky (2026-10-04): the C64 User's Guide's
    "UP, UP, AND AWAY" sprite, C= logo and all, in red, yellow and blue.
    Each race picks three of 16 bearings round the camera (each 2-5 on
    from the last), each with its own distance (44-64) and height (8-18
    degrees up). They ride with the camera inside the mountain ring, drift
    with a slow wind and bob. They are world-space actors appended to the
    cockpit's `WORLD_TICK` block, so they add no commands.
  - a khaki road with a yellow edge line broken by dark red
  - the track on a solid embankment: walls from the road down to the
    ground in alternating red and white panels with dark seams. Only the
    bridge is a deck, where another road passes under it.
  - olive-khaki ground

  This is done in `gen_stunt.py`. Textures stay subtle (grain), not busy.
- **View.** The cockpit only, laid out as the original's and in its
  colours (WHITE levels 1-9 of the palette are the cockpit's own):
  - the roll cage, its struts and the side panels
  - the engine and its two intakes in front, with four exhaust stacks a
    side between the engine and the wheels. All eight throw flames on
    boost.
  - the two front tyres, which steer with the stick and roll with the
    speed (their own sprites, 32x64 texels drawn two pixels wide, so 40
    textures fit the 1 MB image)
  - a dashboard as the original's: the speed gauge, lap and boost
    (`L2 B67`), the gap to the rival in sixteenths of a segment (`-1958`,
    minus is behind), the lap and best times (`0:49.3`), the steering
    wheel, and a crack across the cage that grows with damage
  - at the start, the car lowered onto the track on chains

  The chase camera is dropped.
- **Physics** (reworked 2026-10-04):
  - Crests depend on speed. The road height the car follows is a
    quadratic B-spline through the segments' midpoints, not the drawn
    road's straight segments. A crest then pulls down with an
    acceleration of curvature times speed squared: slow cars follow it,
    and fast ones leave the ground. Gaps and bridge decks keep the linear
    road, so the jump thresholds are unchanged.
  - Grip is limited. The turn rate is the steering times a grip that falls
    as 1/speed above a corner speed, so the car cannot turn sharply when
    fast.
  - Steering is progressive. The wheel moves 12 steps a frame towards full
    lock (120), so 10 frames (about a quarter second) from centre, and
    returns at 24. There is no self-centring of the car, so it has to be
    steered.
  - The turning circle is tight. At walking pace the car turns on a
    hairpin's radius (STEER0 + 8 * vHi at full lock), so the original's
    tight bends can be driven slowly, but not fast.
  - The brakes are weak and slide. `BRAKE` is 256 a frame (it was 900,
    which stopped the car almost on the spot). Braking above `SLIDE_V`
    ($28) halves the grip, so a car braking into a bend runs wide.
  - A hard landing throws the car. The landing's damage also twists it
    off the road's heading (`landKick`, up to `KICK_MAX` = 17 degrees, in
    the direction it was already crooked), so a fast landing has to be
    caught with the wheel.
- **Tracks.** The original's eight: Little Ramp, Stepping Stones, Hump
  Back, Big Ramp, Ski Jump, Draw Bridge, High Jump and Roller Coaster
  (2026-10-04). Their plans and features were read off the original's own
  track previews (the c64-wiki's `StuntCarRacer_Rennkurse.gif`, one frame a
  track), with a camera fitted to them, then scaled to the one length every
  track here has. Heights are about 2.5 times flatter than the previews
  draw them, to stay inside the 8.8 world. The autopilot caps its speed
  in tight bends and brakes for them. Every track but Big Ramp, which
  already had its bumps, has two or three short crests on its straights
  (`crests()` in `stunt_tracks.py`): taken flat out, at about vHi 52 and
  up, they launch the car; taken slower, the car stays down.
- **Structure.** A league as in the original: divisions and a season of
  races against AI rivals, with promotion and relegation.
- **Controls.** A joystick in port 2 (push to accelerate, pull to brake,
  left/right to steer, fire to boost). The keyboard (W/S/A/D, SPACE) works
  too.
- **Sound.**
  - In game: SID engine sound tied to revs, plus effects for crashes,
    landings and boost. The tyres squeal while braking hard on the road,
    and touching the rival gives a falling noise crunch (at most one every
    16 frames while the cars rub), so a collision is heard, not just
    felt in the handling.
  - Title and menu: a title tune (not shipped; see the Files table), with
    the credits on the start screen. Before it, the HONDANI logo with
    Quake's light sweep.
- **Menus.** League table, track names and results are shown on the C64
  screen and also on HDMI, so someone with only the gpu64 HDMI output
  connected can play the whole game.
- **Delivery.** `gpu64_demo_stunt.prg` plus `stunts.reu` (decided
  2026-10-03, replacing the D64). `stunts.reu` is a G64P level pack holding
  only pictures, so `LEVEL_PICTURE` draws the title art, menus and font from
  it with nothing crossing the bus. The firmware is game-agnostic: the
  selected REU image is the only place it reads levels and pictures from,
  and it has no Stunt-specific code. The player selects `stunts.reu` in the
  RAD menu *after* pressing T to REU, because T clears the image
  selection, then runs the PRG. The title checks for its own picture and
  says "SELECT STUNTS.REU IN THE RAD MENU" if the image is wrong. Plus a
  player-facing README.
- **HDMI only.** Every screen (start, menus, league, results) is drawn as
  graphics on HDMI, not in text mode. The user plays with no VIC screen
  connected.
