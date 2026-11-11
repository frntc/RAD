# Keeping a retained scene correct

gpu64's class 1 scene graph is **retained**: a node you create keeps its
position, orientation, scale and visibility until you change them. That is
what makes the API cheap — a frame costs one small command per thing that
actually moved, not a re-description of the world.

It is also what makes a single lost register write permanent.

This page is about that trade and the one protocol that closes it. It is
short, it is not optional, and a game that skips it will eventually show a
room in the wrong place and never recover.

## What can be lost, and what covers it

The C64 side of the cartridge port is sampled by a polling loop that
occasionally misses a write. The rate is low — roughly one write in tens of
thousands — but it is not zero, it is not going to be fixed, and it varies by
more than an order of magnitude between sessions, so it cannot be budgeted for
by measuring it once.

`SEQ`/`SEQACK` detect a dropped write, but only for two of them:

| Write | Covered by `SEQACK`? |
|---|---|
| `SEQ` | yes |
| `CMD_LO` (the opcode, which triggers the dispatch) | yes |
| `ARG0`-`ARG15` | **no** |
| `ID_LO`/`ID_HI` | **no** |

So the failure that matters is not a command that vanished — you would see
that — it is a command that **half arrived**. The dispatch runs, `ERRCODE`
says `OK`, `SEQACK` matches, and one argument byte holds whatever was there
before.

Three worked examples, all observed:

- **A dropped orientation byte.** The view tips by a coordinate and stays
  tipped. The player has not looked down and it looks like they are looking
  down.
- **A dropped `ID_LO`.** The command lands on whichever node was addressed
  last. Move the camera and hit a monster's id instead: the player is
  teleported across the room and sees it from outside, through the backfaces
  of its near walls.
- **A dropped strength byte on `SET_POINT_LIGHT`.** Zero strength turns the
  light off without destroying the node. The room goes dark for the rest of
  the session.

## Why detection is the wrong tool

You could read the state back — `GET_TRANSFORM` ($36) exists and will tell you
where a node really is. But a readback is a *read*, reads are the less reliable
direction, and you would need one per node per frame to catch anything
promptly. You would spend most of a frame proving that nothing went wrong.

Re-sending is cheaper than checking. **Every transform and state opcode in
class 1 is absolute and idempotent** — `SET_POSITION`, not a translate;
`SET_ORIENTATION` replaces rather than composes — so sending a node's state
again is always safe, costs nothing when the state was already right, and
repairs it when it was not.

(`MOVE_LOCAL`, `MOVE_WORLD` and `ROTATE_LOCAL` are the exceptions: they are
deltas, they are *not* idempotent, and a re-send of one moves the node twice.
Use them to drive motion if you like, but keep your own absolute copy of the
result and refresh from that.)

## The protocol: bounded lifetime

Give every retained node's state a **bounded lifetime**. Re-assert it on a
rotating schedule whether or not it changed, so no lost write can outlive one
turn of the rotation.

```
    every REFRESH_EVERY frames:
        send entry[i] of the refresh ring
        i = (i + 1) mod N
```

Each ring entry re-sends one node's one piece of state — position, or
orientation, or a light's strength and radius, or a visibility flag. With N
entries firing every `REFRESH_EVERY` frames, any lost store is corrected
within `N * REFRESH_EVERY` frames.

Note which knob is which. **`REFRESH_EVERY` alone sets the cost per frame; the
entry count sets the cycle length.** Adding entries lengthens the bound on how
long damage survives — it does not add traffic. So err heavily towards covering
everything.

The `quake3d` demo uses twenty-six entries every two frames: a full cycle in 52
frames, under nine tenths of a second, for half a command per frame against the
eleven a frame already sends. That is the shape to copy.

### Which nodes need an entry

This is the part that is easy to get backwards. **A node that never changes is
a node that never heals.**

A monster that walks across the room sends its position every frame anyway, so
a dropped store costs it a single frame and fixes itself. The node that needs
the ring is the one placed once during setup and never mentioned again — the
level geometry, the static props, the torch on the wall. Those are most of
what a room is made of, and nothing else will ever correct them.

But "does this node move" is the wrong question, and the `quake3d` demo got it
wrong the first time. Its ring shipped covering the monster standing in the
corner — obviously static, obviously unhealable — and a fault-injected run came
back with a *different* monster stranded at the wrong coordinates while every
other node had repaired itself. That one turns on the spot: its orientation
goes out every frame, which makes it look like a moving node, and its position
is written once during setup and never again. Half of it healed and half of it
could not, and nothing about the node said which half.

So the test is **per piece of state, not per node, and it is mechanical**:

> Anything you do not write unconditionally every frame needs a ring entry.

That covers "only when it changed", "only when the player is firing", and
"only during setup". State you re-send every frame regardless does not need an
entry — but listing it anyway costs one command per cycle and saves you
re-auditing the table every time the animation changes. The demo lists all of
it for that reason.

### And state you never write at all

There is one more case, and a dropped `ID_LO` is how you reach it. A command
whose id write was lost lands on **whichever node was addressed last**, so a
node can end up holding state its owner never sent it.

The demo hit this twice. First its muzzle flash came back holding a yaw of
20992 — nothing in that program ever sets a light's orientation, and the value
had arrived from a `SET_ORIENTATION` meant for a monster. A light's yaw is
unused, so it did no harm. Then, with every monster covered, **the level node**
came back with a yaw of 3072, by the same route. The program never rotates the
room; it had no entry; and a room wearing a stray yaw is the whole level tilted
for the rest of the session.

So size the ring by what each node **type** accepts, not by what your program
happens to set. Every field a node has needs a writer, and if your program is
not that writer then a misaddressed command is the only one — position,
orientation, scale, visibility, plus the type-specific `SET_SPRITE` and
`SET_POINT_LIGHT`. Most of those entries send a constant. That is precisely
the point: a value that never changes is a value with no other writer.

### Refresh from a shadow, not from a recomputation

A constant is easy to re-send. Dynamic state that you write only *sometimes*
is the awkward case, and the demo's muzzle flash is the worked example: the
light is moved to the player's eye on the frames he is firing, and on every
other frame nothing writes it at all. By the rule above it needs an entry — but
what should that entry send? Not "his eye now": that would drag the light
around behind him between shots, and it would make the faulted run and the
clean run disagree for a reason that has nothing to do with the fault.

Send **the last value you intended**. The demo keeps the twelve staged bytes in
`flashPosSave` as it sends them, zeroed at load, and the ring entry re-sends
that block. A program that never fires refreshes zeroes, which is exactly what
the node should hold; a program that fired ten seconds ago refreshes where it
fired. The shadow is the intent, and the ring's job is to make the scene match
the intent.

This one was not found by reading the code. It was found by the fault sweep
below: in a run where the player never fired once, a `SET_POSITION` meant for
the camera lost its `ID_LO`, landed on the flash, and stayed there — a field
the program does write, in a run where it never did.

### Re-stage before you retry

When `SEQACK` does report a drop and you re-issue the command, **write the
arguments again first**. The whole reason the retry exists is that the lost
write may have been one of them, and the `ARG` window still holds whatever did
land. Re-firing `CMD_LO` on its own re-runs the command with the same broken
arguments.

The `ARG` registers are write-only; reading them back samples a floating bus,
not what you staged. Compare against your own source bytes instead.

### Do not mark it sent until it is

If you keep a shadow copy to answer "has this changed since I last sent it",
update the shadow **only after a send whose `SEQACK` matched**. A command that
may or may not have landed has to stay recorded as not sent. Mark it sent and
a dropped update becomes permanent, because no later frame will ever see a
difference to correct.

## What the ring cannot repair: setup

The ring re-asserts node **state**. There are two things it structurally
cannot reach:

- **Node existence.** If the `CREATE_OBJECT` itself was the command that half
  arrived, there is no node for a transform to be applied to.
- **Creation arguments.** `CREATE_OBJECT`'s mesh id and `CREATE_SPRITE`'s
  texture id are read once, at creation, and no transform opcode ever mentions
  them again. A fault-injected run left a level wearing mesh 62228 and a
  monster wearing texture 16388 long after every transform had healed.

Setup is also where the odds are worst in absolute terms: a scene build is a
few hundred register writes, so at the observed drop rate roughly one power-on
in a hundred loses one of them — rare enough to never see in testing, common
enough to happen to a player.

The repair is almost embarrassingly simple: **build the scene twice.** Every
creation opcode replaces in place over a live id, so a second pass rebuilds
anything that half arrived and re-states it. It costs one extra pass of setup
commands, once, before the loop starts, on a C64 that has nothing else to do
yet — and it requires your setup routine to be ordered create-then-state per
node, which it almost certainly already is.

What that does not fix is a `CREATE` whose own `ID_LO` was lost: the node
exists under some other id, and the second pass creates the missing one and
leaves an orphan behind. In a 256-slot table with a handful of tenants that is
cheaper to leave than to detect. If you do want to detect it, `ARENA_STATUS`
and a node count are how.

## A level the Pi built

A level loaded with `LOAD_LEVEL` turns both of the rules above upside down.
Its nodes, their meshes, their positions and its palette were put there by the
Pi out of a file on its SD card, and your program holds a copy of none of it —
so there is no shadow to refresh from. Two opcodes ask the Pi to refresh from
the file instead:

- **`LEVEL_NODE` ($1C)**, one node per call. It puts back the node's type, its
  mesh, its visibility, scale and orientation, and with `ARG0` bit 0 its
  position. Run it over the level's node ids on a ring, one a frame; the first
  id past the end answers `BAD_ID`, which is where your ring wraps. Clear bit 0
  for a node you move yourself — a door's position is yours, and the file's is
  the closed one.
- **`LEVEL_PALETTE` ($17)**, no arguments. It puts back any palette entries
  that differ from the level's.

Both answer 0 in `RESULT` when nothing was wrong, and cost next to nothing
then, so there is no reason to call them only when something looks off.

These exist because of what the E1M1 demo's first bench session showed:
**creation is not only a setup-time risk.** Every `CREATE_*` replaces a live id,
and a single flipped bit in `CMD_LO` turns a `SET_POSITION` ($30) into
`CREATE_OBJECT` ($20), a `SET_ORIENTATION` ($31) into `CREATE_CAMERA` ($21), a
`SET_VISIBLE` ($24) into `CREATE_SPRITE` ($25). With the id intact it lands on
the node you meant; with a flipped `ID_LO` it lands on a wall. Either way the
node is now a different kind of node, and no transform ever turns it back.
A flipped `CMD_HI` is the same story in class 0: `SET_POSITION` becomes
`PAL_SET` and a palette entry becomes a coordinate.

Your own camera is exposed the same way, and it is yours to repair. If a
periodic `SET_ACTIVE_CAMERA` answers `BAD_ID`, the node is no longer a camera:
`CREATE_CAMERA` it again, re-activate it, and send its position and
orientation before the frame is committed.

## The same idea for replies

A read that is not serviced returns the last value on the bus, not an answer.
Treat any `ERRCODE` outside `$00`-`$0C` as "ask again", retry a bounded number
of times, and if the retries run out report **success**, not failure: the
command was dispatched, `SEQACK` confirms that independently, and only the
reply was lost. Never let an unreadable answer become a control-flow decision
— see [error-codes.md](error-codes.md).

The refresh ring is what makes that safe to say. If something really did go
wrong, the next turn of the rotation repairs it.

A reply that comes back by DMA — `GET_INFO`, `GET_HEALTH`, and every other
writeback opcode — has a worse failure than a lost answer. Its destination
address travels in `ARG` bytes, which `SEQACK` does not cover, so one dropped
write sends the transfer to an arbitrary address in your own memory. Declare a
`SET_DMA_WINDOW` ($0C) over the buffer you read into, give the window its own
refresh entry like any other retained state, and a misaddressed readback
becomes `OUT_OF_RANGE` instead of a corrupted program. Declare it before your
**first** readback — including any your start-up routine makes before you think
the program has begun — and verify the data arrived rather than trusting that
the command returned. The full argument is in
[class0-2d-reference.md](class0-2d-reference.md#the-readback-fence-set_dma_window-0c).

## Testing it

`tools/prgsim/runsim.py` can inject the fault, which is the only practical way
to know whether your refresh scheme actually converges — the real rate is far
too low to reproduce on purpose, and varies by more than an order of magnitude
between sessions.

```
    --bus-fault=drop:N     fail every Nth register access
    --bus-fault-until=F    ... but stop injecting after rendered frame F
    --dump-scene           print gpu64's retained node table at the end
```

Run your program three ways and diff the scene dumps: clean, faulted
throughout, and faulted-then-clean. The third is the one that matters — it
separates "the program repaired the damage" from "the damage is still
arriving". A converging program's dump matches the clean run node for node.
Anything that differs is a piece of state with no ring entry, and the dump
names it.

---

See also: [class1-3d-mesh-reference.md](class1-3d-mesh-reference.md) for the
opcodes, [error-codes.md](error-codes.md) for the readback rules, and
`dev/progress_tracker.md` for the bench campaign that measured all of
this.
