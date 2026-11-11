# Stunt Car Racer for gpu64

Race a supercharged stunt car around elevated tracks full of ramps, jumps and
gaps. Beat your rival to the line, climb from Division 4 to Division 1, and
win the league.

This is a homage to Geoff Crammond's 1989 classic, rebuilt for the gpu64
cartridge on the original's eight tracks, with new artwork. The C64 runs
the game. The gpu64 draws it in 3D on its HDMI screen.

## What you need

- A Commodore 64 with the gpu64 cartridge.
- A monitor or TV on the gpu64's **HDMI** output. Every screen of the game
  is drawn there, so the C64's own video output can stay unconnected.
- A joystick in **port 2**, or the keyboard.
- Two files, which always go together:
  - `gpu64_demo_stunt.prg`, the game
  - `stunts.reu`, its graphics, fonts and tracks

## Installing

Copy the two files to the gpu64's SD card:

| File | Folder on the card |
|------|--------------------|
| `gpu64_demo_stunt.prg` | `RAD_PRG` |
| `stunts.reu` | `REU` |

## Starting the game

1. Switch on. The RAD menu appears.
2. Press **T** until the menu reads **REU**. Do this first: pressing T clears
   the image you have selected.
3. Select **`stunts.reu`** as the REU image.
4. Run **`gpu64_demo_stunt.prg`**.

The HONDANI logo appears, then the title screen with Led Storm playing.
Press fire.

If the screen says **SELECT STUNTS.REU IN THE RAD MENU**, a different REU
image is selected, or none is. Go back to the RAD menu and repeat steps 2–4.

## Controls

| Joystick (port 2) | Keys | In a race | In the menus |
|-------------------|------|-----------|--------------|
| Up | W | Accelerate | Move up |
| Down | S (or CRSR DOWN in menus) | Brake | Move down |
| Left | A | Steer left | |
| Right | D | Steer right | |
| Fire | SPACE (or RETURN in menus) | Boost | Select |
| | RUN/STOP | Hold to give up the race | Back |
| | F3 | Autopilot, practice only | |

**Steering** is progressive, as with a real wheel: holding left or right
turns the wheel further for about a quarter second, and letting go brings it back
to the centre. The car does not straighten itself, so keep steering. The
faster you go, the less the tyres can turn the car, so brake for the
hairpins.

**Braking** is gradual, as on a real car: brake early. Braking hard at
speed makes the tyres squeal and lose half their grip, so the car slides
wide if you brake in the middle of a bend.

**Boost** burns fuel from the tank shown on the dashboard, and all eight
exhausts flame while it burns. The tank refills
each time you cross the start line. Save some boost for the run-up to a jump.

## The dashboard

| Where | Shows |
|-------|-------|
| Left box, top | **L**: the lap. **B**: the boost left in the tank. |
| Left box, bottom | Your distance to the rival: plus when you lead, minus when you are behind. |
| Middle | The speed gauge. |
| Right box, top | This lap's time. |
| Right box, bottom | Your best lap. |

## The main menu

- **START A NEW SEASON**: begin the league in Division 4.
- **CONTINUE THE SEASON**: go back to the season you were playing.
- **PRACTISE A TRACK**: race any of the eight tracks against a practice
  rival. Up and down choose the track. No points are at stake.
- **CONTROLS**: the table above, on screen.
- **RUN/STOP** quits to BASIC.

## The league

There are twelve drivers in four divisions of three. You start at the
bottom, in Division 4.

- Each division has **two tracks**. In a season you race each of your two
  rivals once on each track, so **four races**. Your rivals also race each
  other.
- Every race is **three laps**, head to head against one rival. The car
  is lowered onto the track by a crane, then a countdown starts the race.
- **Points**: a win scores **2**, and the fastest lap of the race scores
  **1** more.
- Giving up a race (holding RUN/STOP) counts as a loss.
- At the end of the season, the driver at the top of each division is
  **promoted**, and the one at the bottom is **relegated**. Finish top of
  Division 1 to become **LEAGUE CHAMPION**.
- Rivals get faster with each division. THE BARON in Division 1 is the
  fastest.

| Division | Tracks |
|----------|--------|
| 4 | Little Ramp, Stepping Stones |
| 3 | Hump Back, Big Ramp |
| 2 | Ski Jump, Draw Bridge |
| 1 | High Jump, Roller Coaster |

The season is remembered while the game is running. RUN/STOP on the
division screen returns to the main menu, and CONTINUE THE SEASON picks it
up again. Quitting to BASIC or switching off ends the league.

## Damage

Hard landings, falling off the track and bumping your rival all damage the
car, and a crack grows across the windscreen. A fall puts you back on the
track by crane, and that costs time. You hear a crunch when you touch
your rival, and a hard landing knocks the car off line, so catch it with
the wheel. If the damage reaches the limit, the
car is wrecked and the race is lost.

## Tips

- Jumps need speed. Line up straight, boost on the run-up, and stay off the
  brake in the air.
- Land with the car level, and don't land on the edge of the road.
- A hump or a crest is taken at your speed: slowly, the car follows the
  road over it; fast, it leaves the ground and flies. Ease off before one
  if you don't want to jump, and land straight if you do. Every track has
  a few crests on its straights that throw a car taken flat out.
- Drive into the back of your rival and you both take damage.

## Credits

- Game for gpu64 by Hondani.
- Inspired by *Stunt Car Racer* by Geoff Crammond (MicroStyle, 1989).
- Music: *Led Storm* by Tim Follin, C64 version by Michal Hoffmann
  (Smalltown Boy), MultiStyle Labs 2003.
