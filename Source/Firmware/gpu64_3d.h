// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__
         {_________         {______________		Expansion Unit

 gpu64 milestone 6 -- the class 1 (3D) subsystem, public interface.

 Wire protocol: dev/milestone6_3d_design.md. That document is the spec;
 this header is the firmware side of it and deliberately does not restate
 the rationale.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

*/
#ifndef _gpu64_3d_h
#define _gpu64_3d_h

#include <circle/types.h>

// gpu64: the whole class 1 subsystem behind one toggle, the same way the
// ladder and the multicore spike are. Off = gpu64_apiDispatch() keeps
// answering class 1 with BAD_CLASS exactly as it does today, core 1 stays
// parked, and nothing below is linked in.
//
// ON by default on this branch, and that is a real change to the shipping
// configuration: it brings CMultiCoreSupport::Initialize() up in every
// build, which milestone 6a showed is safe on its own but which the
// milestone 4 releases did not do. Comment it out to get the proven
// single-core build back -- that is the bisect handle if anything in
// class 0 regresses.
#define GPU64_3D_ENABLED

// --- class 1 opcodes ----------------------------------------------------
// Full table with argument layouts: dev/milestone6_3d_design.md. Only the
// ones a given phase implements are dispatched; the rest are BAD_OPCODE
// until they exist, which is what keeps a half-built class honest.

// System and loop -- $00-$0F
#define GPU64_3D_OP_SCENE_RESET		0x00
#define GPU64_3D_OP_SET_VIEWPORT	0x01
#define GPU64_3D_OP_SET_PERSPECTIVE	0x02
#define GPU64_3D_OP_SET_LIGHT		0x03
#define GPU64_3D_OP_BUILD_COLORMAP	0x04
#define GPU64_3D_OP_SET_BACKGROUND	0x05
#define GPU64_3D_OP_LOOP_START		0x06
#define GPU64_3D_OP_LOOP_STOP		0x07
#define GPU64_3D_OP_SCENE_COMMIT	0x08

// gpu64 (2026-09-10): LOOP_STOP, SCENE_RESET, DESTROY_NODE and FREE_RESOURCE
// all carry the one-shot key in ARG15 and refuse with BAD_ARGS without it --
// GPU64_KEY_DESTRUCTIVE in gpu64_api.h, which has the whole reasoning. It
// started here, on 2026-09-10 morning, as an ARG[0] key on LOOP_STOP alone,
// out of bench run 11's $07-nobody-sent; run 14 the same afternoon found the
// same mechanism reaching something that destroyed the live scene's active
// camera, and ARG[0] turned out to be the wrong register anyway -- the
// per-frame node stream writes it constantly, so a phantom arriving
// mid-frame stands a real chance of finding a coordinate byte that happens
// to be $A5 already sitting there. ARG15 is read by no opcode and is spent
// by every dispatch.

// gpu64: not in the design doc's original table -- added in phase 1 because
// "you get OUT_OF_MEMORY eventually" is truthful and undebuggable against a
// 32 MB arena. RESULT is the free arena in 128 KB units, which is exactly
// 0..256 for a 32 MB arena and so fits the one byte RESULT has.
#define GPU64_3D_OP_ARENA_STATUS	0x09

// gpu64 (milestone 18): the level loader. LOAD_LEVEL hands one .g64lev of
// the level pack in the selected REU image to class 1 -- a whole level's
// geometry, textures, palette and entity table, which at 620 KB the C64 cannot upload and has no
// reason to: it is level data, not game state. LOAD_LEVEL validates and arms;
// LEVEL_STEP performs one bounded slice of the build and reports progress, so
// the DMA hold per command stays in the same range as an ordinary REU
// transfer instead of halting the C64 for the whole load.
//
// Two new opcode numbers rather than arguments on UPLOAD_MESH, which is what
// the v1 surface freeze (dev/milestone17*, gpu64-v1-surface-frozen)
// requires of anything added after it.
//
// LOAD_LEVEL is keyed: it recreates the resource table and the scene from
// scratch, so a phantom would destroy a running session exactly the way
// SCENE_RESET would.
#define GPU64_3D_OP_LOAD_LEVEL		0x13
#define GPU64_3D_OP_LEVEL_STEP		0x14

// LEVEL_STEP's RESULT: 0..99 is percent complete and more steps remain; this
// means the build finished. A distinct value rather than "100", so that a
// client cannot mistake the last progress report for completion.
#define GPU64_3D_LEVEL_DONE		0xff

// gpu64 (milestone 18): CLIP_MOVE, the answer to "can I walk here?".
//
// Collision is the one piece of game logic that cannot live on the C64 --
// E1M1's clipnodes are 43 KB and its planes another 36 KB -- and the Pi is
// already holding them. So the C64 sends where it is and where it would like
// to be, and gets back where it actually ends up. It stays authoritative:
// gpu64 never moves the camera, it only answers.
//
// Everything travels in one block in the C64's own memory rather than in the
// ARG registers, for two reasons. The first is that it does not fit: a
// position and a displacement in 16.16 are 24 bytes on their own. The second
// is the better one -- a block is one DMA burst where the registers would be
// twenty-four separate writes, i.e. twenty-four independent chances for the
// sampling defect to drop one, and a block can carry a checksum where the
// register file cannot.
//
// CLIP_MOVE is NOT keyed. It destroys no retained state; the state it could
// damage is the C64's own memory, and SET_DMA_WINDOW ($0C) is what confines
// that. A program that calls CLIP_MOVE should set the window.
//
// It is also the one class-1 opcode that answers while the render loop is
// running, because it reads nothing but the level file, which is immutable
// once loaded, and writes nothing but the caller's block.
#define GPU64_3D_OP_CLIP_MOVE		0x15

// The block, 56 bytes plus 16 per mover. ARG0-5 name it the way every other
// blob argument is named (space, 24-bit address, 16-bit length) and the length
// must cover all of it. Input is bytes 0..29 and the mover records from 56;
// the Pi writes 32..55 and never touches the input half, so a readback that
// fails its checksum can simply be retried.
//
//    0..11   in   start   x, y, z   s32 16.16 world units, gpu64 axes, y up
//   12..23   in   delta   x, y, z   s32 16.16, the displacement wanted
//      24    in   hull    1 = player, 2 = the larger monster hull
//      25    in   mode    GPU64_CLIP_MODE_* in gpu64_level.h
//      26    in   model   0 = the world, or a brush model to move instead
//      27    in   magic   GPU64_3D_CLIPMOVE_MAGIC_IN
//      28    in   check   XOR of bytes 0..27, byte 29, and every mover byte
//      29    in   movers  how many mover records follow, 0..16
//   30..31        reserved, not read, not summed
//   32..43  out   end     x, y, z   s32 16.16, where the move actually ended
//      44   out   flags   GPU64_CLIP_* in gpu64_level.h -- also in RESULT
//      45   out   cont    GPU64_CLIP_CONT_* at the end position's feet,
//                         from hull 0 -- the only hull that knows liquids
//      46   out   frac    0..255 of the horizontal displacement covered
//      47   out   bumps   slide iterations used, diagnostic
//      48   out   magic   GPU64_3D_CLIPMOVE_MAGIC_OUT
//      49   out   check   XOR of bytes 32..48
//      50   out   wlevel  Quake's waterlevel 0..3 (dry, feet, waist, eyes)
//      51   out   ~wlevel 0xff - byte 50; outside the checksum, which
//                         predates them, so they carry their own check
//   52..55  out   zero
//
// Then `movers` records of 16 bytes each, the first at byte 56. Each one is a
// brush model standing somewhere in the world: a closed door, a lowered
// platform, a button not yet pressed. The world's own hull knows nothing
// about them -- they are separate clipnode trees -- so a trace that does not
// name them walks straight through every door on the map.
//
//    +0        model   brush model index; 0 is an empty slot, skipped
//    +1        flags   reserved, zero
//    +2..3     reserved, zero
//    +4..15    ofs     x, y, z  s32 16.16, where the model is NOW relative to
//                      its closed position -- i.e. how far the C64 has already
//                      opened it. A closed door is all zeroes.
//
// The mover list is an argument, not retained state: gpu64 does not know which
// doors are open, the C64 does, and it says so on every call. That is what
// keeps CLIP_MOVE a pure function of its block, and therefore safe to answer
// while the render loop is running.
//
// Both magics are checked, not just the checksums: a checksum alone accepts
// an all-zero block, which is exactly what a buffer the C64 never filled in
// looks like (gpu64-getinfo-readback-was-unfenced -- validate with a magic,
// not with a poison).
#define GPU64_3D_CLIPMOVE_BYTES		56
#define GPU64_3D_CLIPMOVE_IN_BYTES	32
#define GPU64_3D_CLIPMOVE_OUT_OFF	32
#define GPU64_3D_CLIPMOVE_OUT_BYTES	24
#define GPU64_3D_CLIPMOVE_MAGIC_IN	0xc5
#define GPU64_3D_CLIPMOVE_MAGIC_OUT	0x5c
#define GPU64_3D_CLIPMOVE_MOVER_OFF	56
#define GPU64_3D_CLIPMOVE_MOVER_BYTES	16
// Must match GPU64_LEVEL_MAX_MOVERS; opClipMove() asserts that it does.
#define GPU64_3D_CLIPMOVE_MAX_MOVERS	16

// gpu64 (milestone 18): LEVEL_ENT, one entity out of the loaded level.
//
// A level's entity lump is where its game is: 369 records for E1M1, naming
// the player start, fourteen doors and their buttons, the triggers that open
// them, forty-two monsters and every item on the map. It is already in the
// Pi's memory because the level file carries it, and it cannot travel to the
// C64 wholesale -- 16 KB of records and a string pool -- so the C64 asks for
// one record at a time and keeps the handful it cares about.
//
// The block protocol is CLIP_MOVE's, for the same reasons and with the same
// two obligations: check the output magic AND the output checksum, and set
// SET_DMA_WINDOW before calling. Unkeyed, and exempt from the pre-execute
// drain, again because the level file is immutable once loaded.
//
// The record is pre-digested for a 6502. Classnames arrive as a `kind` byte
// (a game switches on it; the ranges are in gpu64_level.h), target and
// targetname as opaque 16-bit ids that compare equal when the strings do, a
// brush mover's travel as a ready displacement, and a brush model's nodes as
// a contiguous run of scene node ids -- so moving a door is SET_POSITION on
// `nodeFirst`..`nodeFirst + nodeCount - 1` and nothing has to be looked up.
#define GPU64_3D_OP_LEVEL_ENT		0x16

// The block, 96 bytes. Input is bytes 0..3; the Pi writes 16..95.
//
//     0..1   in   index   which entity, 0 .. entCount-1
//        2   in   magic   GPU64_3D_LEVELENT_MAGIC_IN
//        3   in   check   XOR of bytes 0..2
//     4..15       reserved, not read
//   16..27  out   origin  x, y, z   s32 16.16 world units, gpu64 axes
//   28..39  out   mins    the brush model's box, zero if not a brush model
//   40..51  out   maxs
//   52..63  out   travel  closed -> open displacement, zero if not a mover
//   64..65  out   yaw     u16, a full turn is 65536
//   66..67  out   spawnflags
//   68..69  out   param0  s16, which key this is depends on the classname
//   70..71  out   param1
//   72..73  out   target      opaque id, 0 = none
//   74..75  out   targetname  opaque id, 0 = none
//   76..77  out   nodeFirst   scene node id of this model's first chunk
//   78..79  out   entCount    how many entities the level has, every call
//      80   out   nodeCount   chunks in the run, 0 if the model draws nothing
//      81   out   kind        see Gpu64_LevelEnt::nKind
//      82   out   model       brush model index, 0 = not a brush model
//      83   out   reserved, zero
//      84   out   magic   GPU64_3D_LEVELENT_MAGIC_OUT
//      85   out   check   XOR of bytes 16..84
//   86..95  out   zero
//
// entCount is in every answer rather than in a call of its own, because a
// game's first act is to walk the table and it would otherwise need a second
// opcode to know where to stop.
#define GPU64_3D_LEVELENT_BYTES		96
#define GPU64_3D_LEVELENT_IN_BYTES	4
#define GPU64_3D_LEVELENT_OUT_OFF	16
#define GPU64_3D_LEVELENT_OUT_BYTES	80
#define GPU64_3D_LEVELENT_MAGIC_IN	0xc6
#define GPU64_3D_LEVELENT_MAGIC_OUT	0x6c

// gpu64 (milestone 18, bench run 38): LEVEL_PALETTE, put the loaded level's
// palette back.
//
// A level's palette is installed once, by LEVEL_STEP, and the C64 never holds
// a copy -- so it is the one piece of retained state a state refresh ring
// could not repair from a shadow of its own. And it does get damaged: a
// corrupted CMD_HI turns a class 1 SET_POSITION ($30) into class 0 PAL_SET,
// which overwrites one palette entry with camera coordinates. Run 38's scene
// "lost its colours" a few minutes in, one entry at a time, and the injected
// runs in prgsim reproduce it.
//
// No arguments. Compares the installed palette with the level's and rewrites
// only the entries that differ; if any did, it pushes the palette to the
// VideoCore and rebuilds the colormap (into the shadow state while the loop
// runs), because a phantom BUILD_COLORMAP could have baked the damage in.
// RESULT is the number of entries repaired, saturated at 255 -- 0 is the
// normal answer, and costs a 768-byte compare and nothing else, so a program
// can afford it on a refresh ring. BAD_ARGS until a level load has finished.
//
// Unkeyed, idempotent, and exempt from the pre-execute drain: core 1 reads
// neither the palette nor the shadow state.
#define GPU64_3D_OP_LEVEL_PALETTE	0x17

// gpu64 (milestone 20, title screens): LEVEL_PICTURE, draw one of the level
// file's pictures onto the class 0 draw page.
//
// The title, the skill menu and the intermission are Quake's own 2D art --
// a 320x200 console background alone is 64 KB, which is more than the C64
// has to send it from. So the art rides in the level file (v7 "pictures",
// see gpu64_level.h) and the C64 names it by index, like a texture.
//
// ARG0 picture index, ARG1-2 x and ARG3-4 y (signed, clipped like BLIT),
// ARG5 flags: bit 0 = keyed, index 255 is transparent (Quake's own hole);
// bit 1 = install the level file's palette first -- a title that played
// with the palette gets Quake's colours back without a level being loaded.
// Other ARG5 bits are BAD_ARGS, as is an index past the table or a card
// with no level file. The picture lands on the draw page; PAGE_FLIP shows it.
//
// Works before LOAD_LEVEL -- it reads the preloaded file, not the loaded
// level -- and only while the class 1 loop is stopped: the loop owns the
// pages, so a LEVEL_PICTURE against a running loop answers BUSY and draws
// nothing. That is also what makes a phantom one harmless in a game. The
// number keeps LEVEL_NODE's rule: every one-bit flip of $1A is undefined in
// class 1 or keyed ($12), and its class 0 twin is undefined.
#define GPU64_3D_OP_LEVEL_PICTURE	0x1A

// gpu64 (milestone 18, bench run 38): LEVEL_NODE, put one level node back the
// way LEVEL_STEP built it.
//
// The level's node table -- which mesh each chunk instances -- lives only in
// the level file, so a C64 cannot repair a level node from a shadow of its
// own the way it repairs its camera. And level nodes do get damaged: every
// CREATE_* replaces a live ID by design, and one flipped CMD_LO bit turns
// SET_POSITION ($30) into CREATE_OBJECT ($20), SET_ORIENTATION ($31) into
// CREATE_CAMERA ($21), SET_VISIBLE ($24) into CREATE_SPRITE ($25). Aimed at a
// level node by a flipped ID byte, any of them leaves a wall that is now a
// camera, a sprite, or a mesh id nobody uploaded -- permanently.
//
// Staged ID = the node id (node base + index). ARG0 bit 0 = also put the
// position back from the file; clear it for a node the program moves itself
// (a door), whose position is the program's to refresh. Other ARG0 bits are
// BAD_ARGS. Restores type OBJECT, the file's mesh id, visible, scale 1.0 and
// zero orientation; if the node was the active camera, it no longer is.
// RESULT bit 0 = type/mesh/visibility/scale/orientation was wrong, bit 1 =
// position was wrong -- 0 is the normal answer. BAD_ID for an id outside the
// level's node run; BAD_ARGS until a level load has finished.
//
// Unkeyed, idempotent, shadow-redirected like SET_POSITION. The number is
// chosen for its neighbours: every one-bit flip of $1C lands on an opcode
// that is undefined in both classes or answers BUSY while the loop runs.
#define GPU64_3D_OP_LEVEL_NODE		0x1C

// gpu64 (milestone 20): WORLD_TICK, one block a frame for everything that
// moves in a Quake game -- the monsters, the items, the gun and the status
// bar -- plus every question the game's rules need answered: can this
// monster see the player, where did this shot land, where does this monster
// end up if it walks. dev/milestone20_quake_combat_design.md has the
// reasoning; in short, per-actor SET_POSITION/SET_ORIENTATION would be ~70
// commands a frame, each one a chance for the bus to drop or flip it, and a
// block is one DMA burst with a checksum.
//
// The actor records are applied to the scene exactly as SET_POSITION would
// apply them -- through the shadow scene while the loop runs, so the op is
// drain-exempt like SET_POSITION and never makes the C64 wait for a render.
// They are absolute and applied only when the WHOLE input checks: a lost
// block costs one frame of stale pose, never a wrong one. The queries read
// the level file only, like CLIP_MOVE, and are answered into the same block.
//
// Unkeyed. Strict mode covers it like any other command that changes state,
// so a program in strict mode sends it with the checked-command key.
//
// The number follows LEVEL_NODE's rule, more strictly: every one-bit flip of
// $6C is undefined in class 1, and its class 0 twin (a lost CMD_HI) is
// undefined too. $18 was the first choice and is one bit from SCENE_COMMIT
// and UPLOAD_MESH -- this block is sent every frame, so it is the command
// most exposed to a flip.
#define GPU64_3D_OP_WORLD_TICK		0x6C

// The block. ARG0-5 name it (space, 24-bit address, 16-bit length), and the
// length must cover the output too. Every position is s32 16.16 world units
// on gpu64 axes -- the format SET_POSITION and CLIP_MOVE already use.
//
// Header, 16 bytes:
//      0   magic    GPU64_3D_WTICK_MAGIC_IN
//      1   actors   0..GPU64_3D_WTICK_MAX_ACTORS
//      2   traces   0..GPU64_3D_WTICK_MAX_TRACES
//      3   moves    0..GPU64_3D_WTICK_MAX_MOVES
//      4   movers   0..GPU64_3D_WTICK_MAX_MOVERS
//      5   frame    anything; echoed back, so a stale answer is recognisable
//    6-7   check    16-bit sum of bytes 0-5, 8-15 and every input record byte
//   8-15   reserved, summed, not read
//
// Then the input records, back to back from byte 16 in this order:
//
// actor, GPU64_3D_WTICK_ACTOR_BYTES each
//    +0-1  node      a node that already exists; WORLD_TICK creates nothing
//    +2-3  resource  OBJECT: mesh id. SPRITE: texture id. $FFFF: unchanged.
//                    LIGHT and CAMERA: must be $FFFF.
//      +4  flags     GPU64_3D_WTICK_VISIBLE | GPU64_3D_WTICK_VIEWSPACE;
//                    other bits refuse the record
//      +5  yaw       a full turn is 256, as the high byte of SET_ORIENTATION's
//      +6  pitch
//      +7  roll
//   +8-19  pos       x, y, z; relative to the eye when VIEWSPACE is set
//
// trace, GPU64_3D_WTICK_TRACE_IN_BYTES each -- a hitscan or line of sight
//   +0-11  start
//  +12-23  end
//     +24  hull      0 = point (shots, sight), 1 = player, 2 = large monster
//  +25-27  reserved, zero
//
// move, GPU64_3D_WTICK_MOVE_IN_BYTES each -- CLIP_MOVE for a monster
//   +0-11  start     origin, or eye height with GPU64_CLIP_MODE_EYE
//  +12-23  delta
//     +24  hull      1 or 2
//     +25  mode      GPU64_CLIP_MODE_* in gpu64_level.h
//  +26-27  reserved, zero
//
// mover, GPU64_3D_CLIPMOVE_MOVER_BYTES each -- CLIP_MOVE's records exactly.
// They block every trace and every move in the block.
//
// Output, written by the Pi from OUT = 16 + the input records' bytes:
//   OUT+0  magic     GPU64_3D_WTICK_MAGIC_OUT
//   OUT+1  frame     the header's, echoed
//   OUT+2  applied   actor records applied
//   OUT+3  reserved, zero
// OUT+4-7  refused   bit n = actor record n was refused (no such node, a
//                    resource of the wrong kind, a bad flag); little-endian
// then one 16-byte answer per trace:
//   +0-11  end       where the sweep stopped
//  +12-13  frac      u16, 65535 = it went the whole way
//     +14  flags     GPU64_3D_WTICK_TR_*
//     +15  hit       GPU64_CLIP_CONT_* of what it hit; EMPTY for no hit
// then one 16-byte answer per move, CLIP_MOVE's bytes 32-47 exactly:
//   +0-11  end;  +12 flags;  +13 contents;  +14 frac;  +15 bumps
// then the trailer, 4 bytes:
//    +0-1  check     16-bit sum of every output byte before the trailer
//      +2  magic     GPU64_3D_WTICK_MAGIC_OUT, again
//      +3  frame     echoed again
//
// A magic at both ends because a block that only half arrived passes a
// check at the front (gpu64-health-block-self-verifying). The Pi writes
// nothing and changes nothing, and answers BAD_ARGS, if: the magic is wrong,
// a count is over its maximum, the length does not cover input and output,
// the check fails, a trace hull is not 0-2, a move hull is not 1-2, a mover
// names a model the level does not have, or there are queries or movers and
// no level has finished loading. Actor-only blocks need no level. The C64
// sends it again.
// RESULT is the number of refused actor records, saturated at 255.
#define GPU64_3D_WTICK_MAGIC_IN		0xd8
#define GPU64_3D_WTICK_MAGIC_OUT	0x8d
#define GPU64_3D_WTICK_HDR_BYTES	16
#define GPU64_3D_WTICK_ACTOR_BYTES	20
#define GPU64_3D_WTICK_TRACE_IN_BYTES	28
#define GPU64_3D_WTICK_MOVE_IN_BYTES	28
#define GPU64_3D_WTICK_OUT_HDR_BYTES	8
#define GPU64_3D_WTICK_ANSWER_BYTES	16
#define GPU64_3D_WTICK_TRAILER_BYTES	4
#define GPU64_3D_WTICK_MAX_ACTORS	32
#define GPU64_3D_WTICK_MAX_TRACES	24
#define GPU64_3D_WTICK_MAX_MOVES	24
#define GPU64_3D_WTICK_MAX_MOVERS	16

#define GPU64_3D_WTICK_VISIBLE		0x01
#define GPU64_3D_WTICK_VIEWSPACE	0x02

#define GPU64_3D_WTICK_TR_CLEAR		0x01	// reached the end untouched
#define GPU64_3D_WTICK_TR_STARTSOLID	0x02
#define GPU64_3D_WTICK_TR_ALLSOLID	0x04

// Resources -- $10-$1F
#define GPU64_3D_OP_UPLOAD_MESH		0x10
#define GPU64_3D_OP_UPLOAD_TEXTURE	0x11
#define GPU64_3D_OP_FREE_RESOURCE	0x12

// Scene nodes -- $20-$2F
#define GPU64_3D_OP_CREATE_OBJECT	0x20
#define GPU64_3D_OP_CREATE_CAMERA	0x21
#define GPU64_3D_OP_DESTROY_NODE	0x22
#define GPU64_3D_OP_SET_ACTIVE_CAMERA	0x23
#define GPU64_3D_OP_SET_VISIBLE		0x24
#define GPU64_3D_OP_CREATE_SPRITE	0x25
#define GPU64_3D_OP_CREATE_LIGHT	0x26
#define GPU64_3D_OP_SET_SPRITE		0x27
#define GPU64_3D_OP_SET_POINT_LIGHT	0x28

// Transforms -- $30-$3F
#define GPU64_3D_OP_SET_POSITION	0x30
#define GPU64_3D_OP_SET_ORIENTATION	0x31
#define GPU64_3D_OP_MOVE_LOCAL		0x32
#define GPU64_3D_OP_MOVE_WORLD		0x33
#define GPU64_3D_OP_ROTATE_LOCAL	0x34
#define GPU64_3D_OP_SET_SCALE		0x35
#define GPU64_3D_OP_GET_TRANSFORM	0x36

// Immediate mode -- $40-$4F
#define GPU64_3D_OP_CLEAR_VIEWPORT	0x40
#define GPU64_3D_OP_DRAW_MESH		0x41
#define GPU64_3D_OP_DRAW_NODE		0x42

#ifdef GPU64_3D_ENABLED

// --- core 0 entry points ------------------------------------------------

// Brings the subsystem to its power-on state: ring buffer emptied, core 1
// released. Called once at boot, before core 1 is started.
void gpu64_3dInit( void );

// Session reset -- called from gpu64_apiReset(), i.e. on every resetREU().
// Per dev/milestone6_3d_design.md's Resource lifecycle section this is
// where every resource of the session is freed, so RUN/STOP+RESTORE cannot
// leave the next program running against stale IDs.
void gpu64_3dReset( void );

// gpu64 (2026-09-10): is the handshake-mode loop running? Only
// gpu64_apiDiagStateByte() needs this -- see gpu64_apidiag.h.
boolean gpu64_3dLoopRunning( void );

// gpu64 (2026-09-27): FULL_RESET's half of the loop. Stops it and drops the
// in-flight frame's bookkeeping, so the next SCENE_COMMIT re-arms it instead
// of answering BUSY forever -- see the case 0x0B comment in gpu64_api.cpp.
// Call only after a drain (gpu64_apiFullReset() has one).
void gpu64_3dLoopFullReset( void );

// gpu64 (2026-09-28): is a program committing frames right now -- the loop
// running and a SCENE_COMMIT accepted within the last half second? The
// class 0 phantom guard in gpu64_apiDispatch() uses it to tell a running
// game (whose FULL_RESET can only be a phantom) from a new program taking
// over a loop the last one left running (whose FULL_RESET is real).
boolean gpu64_3dCommitsLive( void );

// Executes one class 1 command from core 0, exactly as doSystem()/doDraw()
// do for class 0: returns a GPU64_ERR_* code, having already pushed
// whatever core 1 needs to do onto the ring.
//
// Through Stage 15a this never blocked beyond a full ring (GPU64_ERR_QUEUE_
// FULL). Stage 15b (dev/gap_filling_plan.md) changes that: CLEAR_
// VIEWPORT/DRAW_MESH/DRAW_NODE now push and return without waiting for their
// own draw, but every *other* class 1 opcode drains the ring before it runs
// (it is about to mutate state a still-queued render also reads), and a
// render pushed against a full ring waits, bus held, for space rather than
// rejecting outright -- see gpu64_3dSync()'s comment for what that means for
// RESULT/ERRCODE timing on the two draws. GPU64_ERR_QUEUE_FULL survives only
// for the pathological case both of those still refuse: a non-render op
// pushed against a full ring, which the design's "a failed dispatch does
// nothing" rule still answers by rejecting rather than waiting.
u8 gpu64_3dDispatch( u8 op );

// Core 1's entry point, called from CGpu64MultiCore::Run(). Never returns.
void gpu64_3dWorker( void );

// Stage 15b's observation-point hook for gpu64_api.cpp: waits for the ring
// to fully drain, publishing the most recently queued CLEAR_VIEWPORT/
// DRAW_MESH/DRAW_NODE's RESULT to gpu64Regs once it is known complete.
// Returns FALSE only on the drain timeout backstop (a wedged core 1).
//
// Call this before anything that would otherwise race a render this file has
// queued but not yet run on core 1: a page flip/vsync commit, or a class 0
// opcode that reads or writes the framebuffer. (Every non-render class 1
// opcode already does the equivalent internally, via its own dispatch --
// callers here do not need to call this before *those*.)
//
// This is also how a program gets a DRAW_MESH/DRAW_NODE's own RESULT (the
// triangle count) back on demand: RESULT is valid as of the last observation
// point, not the instant dispatch returns, so issuing any op that reaches
// this -- including just the next DRAW_NODE -- and then reading RESULT gets
// it. See docs/class1-3d-mesh-reference.md's "Deferred RESULT" note.
boolean gpu64_3dSync( void );

// gpu64 (2026-09-08): harvest a finished loop frame from OUTSIDE a dispatch.
//
// GPU64_STATUS_FRAME_READY is raised by pollLoopFrame(), and until now
// pollLoopFrame() ran only from gpu64_3dDispatch() -- i.e. only when the C64
// wrote CMD_LO. A read of STATUS is not a dispatch, so a program that sat in
// a read-only poll loop waiting for the bit could never see it rise:
// SCENE_COMMIT clears FRAME_READY on its way out, and nothing short of the
// next command put it back. docs/class1-3d-mesh-reference.md promised the
// opposite ("a program that polls STATUS between commits sees it as soon as
// it's true") and tools/prgsim modelled the promise rather than the code,
// which is why every desk check passed. On the bench 2026-09-08 the Quake
// demo froze for its full 8000-poll budget (~133 ms, eight displayed frames)
// on 162 of 8173 frames -- once a second, and exactly the judder the user
// reported. Every one of those stalls ended in an ACCEPTED commit, which is
// the fingerprint: the loop was never busy, the bit was simply orphaned.
//
// gpu64_vsyncCommitFlip() calls this from inside its DMA hold. That is the
// only safe home for it: it reads gpu64_3dRing.tail, a line core 1 writes,
// which reuUsingPolling() must never touch while the C64 is free-running --
// but with the bus held there is no per-C64-cycle deadline, the same
// shipped-and-verified exemption logGpu64_3dStats() already relies on. It
// also means the harvest happens once a frame on the frame clock, regardless
// of whether the C64 sends anything at all.
void gpu64_3dPollLoopFrame( void );

// Formats the subsystem's counters for the on-screen log (bring-up aid,
// phase 0's only output). Returns the end pointer.
char *gpu64_3dReport( char *p );

#endif	// GPU64_3D_ENABLED

#endif
