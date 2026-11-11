# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
A reference model of the gpu64 class 0 command API, written from
docs/README.md and the files it indexes, rather than from the firmware.

That direction matters. The point of this file is to be a second opinion:
the conformance suite is checked against what the reference SAYS, so a test
that encodes a misreading of the spec fails here, on a PC, instead of
turning into an argument at the bench. Where the model and the firmware
disagree, one of them is wrong and the disagreement is the finding.

Not modelled, because the suite does not assert on them: the actual display,
the on-screen log overlay, cache behaviour, and anything about timing beyond
"a vblank happens every frame period".
"""

import math
import struct

import gpu64font
from gpu64class1 import Class1Mixin, ST_FRAME_READY

FB_W, FB_H, FB_PAGES = 320, 200, 3
BORDER_W, BORDER_H = 32, 36

# The 80x50 text mode (docs/class0-2d-reference.md, "Text mode"). A separate
# display mode, not a layer: while it is up the paged 320x200 surface does not
# exist, which is why the model refuses PAGE_FLIP and every non-zero class
# while mode is MODE_TEXT.
MODE_GRAPHICS, MODE_TEXT = 0, 1
TXT_COLS, TXT_ROWS = 80, 50
TXT_CHAR_W, TXT_CHAR_H = 8, 8
TXT_W, TXT_H = TXT_COLS * TXT_CHAR_W, TXT_ROWS * TXT_CHAR_H
TXT_BORDER_W, TXT_BORDER_H = 64, 72
TXT_CELLS = TXT_COLS * TXT_ROWS
TXT_PLANE_SCREEN, TXT_PLANE_FG, TXT_PLANE_BG = 0, 1, 2
TXT_WRITE_ASCII = 0x01
TXT_DEF_FG, TXT_DEF_BG = 14, 6

# The palette a reset gpu64 comes up with (Source/Firmware/gpu64_fb.cpp):
# entries 0-15 are the C64's own colours in the C64's own numbering, 16-254
# black, 255 white for the on-screen log. Only the PPM dump reads this --
# nothing in the API can read the palette back -- but a demo rendered with
# the wrong reset palette would look wrong for a reason that is not the
# demo's fault.
C64_PALETTE = (
    (  0,   0,   0), (255, 255, 255), (104,  55,  43), (112, 164, 178),
    (111,  61, 134), ( 88, 141,  67), ( 53,  40, 121), (184, 199, 111),
    (111,  79,  37), ( 67,  57,   0), (154, 103,  89), ( 68,  68,  68),
    (108, 108, 108), (154, 210, 132), (108,  94, 181), (149, 149, 149),
)

ERR_OK = 0x00
ERR_BAD_OPCODE = 0x01
ERR_BAD_CLASS = 0x02
ERR_OUT_OF_RANGE = 0x03
ERR_BAD_ARGS = 0x04
ERR_SINGULAR = 0x05
ERR_UNSUPPORTED = 0x06
ERR_BUSY = 0x07
ERR_OUT_OF_MEMORY = 0x08
ERR_QUEUE_FULL = 0x09
ERR_BAD_ID = 0x0A

# gpu64 (2026-09-10): the one-shot key destructive opcodes carry, and the ARG
# it rides in. See Source/Firmware/gpu64_api.h -- the firmware spends ARG15 on
# every dispatch of every class, so the key is valid for exactly the command
# that staged it.
KEY_DESTRUCTIVE = 0xA5
KEY_ARG = 15
# gpu64_api.h, GPU64_KEY_CHECKED: ARG15 = $D0|n, ARG14 = XOR of that key,
# CMD_HI, CMD_LO, ID_LO, ID_HI and ARG0..n-1.
KEY_CHECKED = 0xD0
# gpu64_api.h, gpu64_apiStrictExempt(): the class 1 opcodes strict mode lets
# through unchecked -- LOOP_START, ARENA_STATUS, and the three queries.
STRICT_EXEMPT = (0x06, 0x09, 0x15, 0x16, 0x36)
CHECK_ARG = 14

# Class 2 -- the raster layer (docs/class2-raster-reference.md, "Class 2 opcodes").
RASTER_REC_BYTES = 16
RASTER_WALL2_BYTES = 32
RASTER_SEC_BYTES = 8
RASTER_MAX_SECTORS = 128
RASTER_MAX_DIM = 1024
RASTER_MAX_LEVELS = 64
RASTER_MAX_TEXTURES = 255
RASTER_ARENA_BYTES = 8 * 1024 * 1024
RASTER_POLY_BYTES = 16
RASTER_VERT_BYTES = 8
RASTER_TEXINFO_BYTES = 16
RASTER_MAX_VERTS = 4096
RASTER_MAX_TEXINFO = 255
RASTER_MAX_WORLD_POLYS = 2048
RASTER_MAX_POLY_VERTS = 16
RASTER_MAX_LIGHTS = 8
RASTER_LIGHT_SHIFT = 40
POLY_MASKED = 0x01
POLY_FLATLIT = 0x02
POLY_TWOSIDED = 0x04
COL_MASKED = 0x01
SPR_MASKED = 0x01
SPR_FLIPX = 0x02
THING_MASKED = 0x01
THING_FLIPX = 0x02
THING_NODEPTH = 0x04
THING_FLATLIT = 0x08
THING_DIRECTIONAL = 0x10
BATCH_CHECKSUM = 0x01
BATCH_CAM3D = 0x02

ST_BUSY = 0x01
ST_ERROR = 0x02
ST_VB_PEND = 0x04
ST_VB_ARMED = 0x08

# $DF50-$DF68 since 2026-09-27 (was $DF0B-$DF23), off the Ultimate's
# Command Interface at $DF1B-$DF1F. $DF0B-$DF4F answers nothing: the firmware
# neither drives a read nor acts on a write there. See gpu64_api.h.
REG_REU_LAST = 0x0A
REG_CMD_HI, REG_CMD_LO = 0x50, 0x51
REG_STATUS, REG_ERRCODE = 0x52, 0x53
REG_ID_LO, REG_ID_HI = 0x54, 0x55
REG_ARG0, REG_ARG15 = 0x56, 0x65
REG_RESULT = 0x66
REG_SEQ, REG_SEQACK = 0x67, 0x68


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def idiv(n, d):
    """Integer division truncating toward zero, which is what C's / does.

    Python's // floors, and the two disagree for exactly the negative
    operands the wall projection is full of. Every division in r_walls()
    goes through here so the model and gpu64_raster_core.cpp cannot drift
    apart on a sign.
    """
    q = abs(n) // abs(d)
    return -q if (n < 0) != (d < 0) else q


# sin(2*pi*i/256) in 8.8, the same table gpu64_raster_core.cpp holds.
SINTAB = [int(round(256 * math.sin(2 * math.pi * i / 256))) for i in range(256)]


def fsin(a):
    return SINTAB[a & 0xFF]


def fcos(a):
    return SINTAB[(a + 64) & 0xFF]


# Which of eight 45-degree sectors a vector points into; see octant8() in
# gpu64_raster_core.cpp, which this must agree with bit for bit.
def octant8(x, y):
    if y >= 0:
        if x >= 0:
            return 0 if y <= x else 1
        return 2 if y >= -x else 3
    if x < 0:
        return 4 if -y < -x else 5
    return 6 if -y > x else 7


def fix_read(buf, i):
    return s16(buf[2 * i] | (buf[2 * i + 1] << 8))


def fix_sat(v):
    """Saturate to the 8.8 range rather than wrapping. A saturated highlight
    looks like clipping; a wrapped one looks like a hardware fault."""
    return max(-32768, min(32767, v))


# When true, round the way Source/Firmware/gpu64_api.cpp's fixWrite()
# actually does rather than the way docs/class0-2d-reference.md specifies, so a run
# can predict exactly which suite lines a given firmware will fail. See
# runsim.py --firmware-rounding.
FIRMWARE_ROUNDING = [False]


# Fault injection for the bulk-transfer instrument. A list of byte offsets
# within a C64-space blob write; each named byte comes back as the byte at
# offset $FF of its own 256-byte page instead of itself. That is the exact
# shape of the two hardware mismatches seen so far -- right page, wrong low
# address -- so switching this on lets the suite's own diagnostic lines be
# checked against a known answer on a PC, before a bench run is spent
# reading them. See runsim.py --alias-drop.
ALIAS_DROP = [[]]


def fix_write_product(buf, i, acc):
    """Bring a 16.16 accumulator back to 8.8, rounded half away from zero and
    saturated -- the contract docs/class0-2d-reference.md states for the $8x set."""
    if FIRMWARE_ROUNDING[0]:
        # acc += +-128 then an arithmetic shift right. The shift floors, so
        # for a negative accumulator the bias is applied twice and every
        # negative product comes back one LSB too far from zero.
        a = acc + (128 if acc >= 0 else -128)
        v = a >> 8
    elif acc >= 0:
        v = (acc + 128) // 256
    else:
        v = -((-acc + 128) // 256)
    fix_store(buf, i, v)


def fix_store(buf, i, v):
    v = fix_sat(v)
    buf[2 * i] = v & 0xFF
    buf[2 * i + 1] = (v >> 8) & 0xFF


def flt_read(buf, i):
    return struct.unpack_from('<f', buf, 4 * i)[0]


def flt_write(buf, i, v):
    struct.pack_into('<f', buf, 4 * i, v)


class Gpu64Model(Class1Mixin):
    def __init__(self, mem_read, mem_write, reu_size=16 * 1024 * 1024,
                 frame_period_us=20000, calibrated=True):
        self.mem_read = mem_read
        self.mem_write = mem_write
        self.reu = bytearray(reu_size)
        self.frame_period_us = frame_period_us if calibrated else 0
        self.calibrated = calibrated

        self.cmd_hi = 0
        # SET_DMA_WINDOW ($0C): the C64-space region a readback may write
        # into. Length 0 means unrestricted, which is the reset default and
        # what a program written before $0C gets.
        self.dma_win_base = 0
        self.dma_win_len = 0
        # The drop detector's pair. The C64 picks a sequence byte and writes
        # it before a dispatch; the firmware echoes it back on every dispatch,
        # so a mismatch means one of the writes was never sampled off the bus.
        # Nothing here drops writes -- the model is a perfect bus -- so SEQACK
        # always matches, which is exactly what makes it useful as an oracle:
        # a nonzero MISSED on hardware is then unambiguously the bus.
        self.seq = 0
        self.seqack = 0
        self.sniff_syncs = 0
        self.sniff_datas = 0
        self.seq_repeat = 0
        self.arg_dummy_reads = 0
        # Source/Firmware/gpu64_apidiag.h: why class 1 stopped accepting
        # commands. Nothing in this model produces a refusal, so the three
        # counters it feeds are constant zero; the lifecycle bytes below
        # are real.
        self.loop_stop_cause = 4                        # GPU64_LOOPSTOP_INIT
        self.class_refused_mode = 0
        self.commit_no_loop = 0
        self.commit_no_clock = 0
        self.loop_stops = 0
        self.last_refuse_state = 0
        self.last_refuse_op = 0
        self.last_refuse_class = 0
        # The command-stream provenance of a loop stop. Real here in the
        # sense that matters: a correctly written program's teardown makes
        # (disp_seq - disp_seq_prev) come out 1, which is the reading the
        # bench compares against.
        self.disp_seq = 0
        self.disp_seq_prev = 0
        self.disp_op = 0
        self.disp_op_prev = 0
        self.api_key = 0
        # The one-shot-key counters, GET_HEALTH bytes 76-79.
        self.key_refused = 0
        self.key_refused_op = 0
        self.cam_lost = 0
        self.scene_wipes = 0
        self.loop_stop_seq = 0
        self.loop_stop_seq_prev = 0
        self.loop_stop_prev_op = 0
        self.status = 0
        self.err = ERR_OK
        self.result = 0
        self.ident = [0, 0]
        self.check_refused = 0
        # gpu64_api.h, strict mode: set by the first checked SCENE_COMMIT.
        self.strict = False
        self.strict_refused = 0
        self.strict_refused_ops = {}
        self.arg = [0] * 16

        self.pages = [bytearray(FB_W * FB_H) for _ in range(FB_PAGES)]
        self.draw_page = 0
        self.visible_page = 0
        self.pending_visible = 0
        self.border = 0
        self.palette = bytearray(256 * 3)
        for i, rgb in enumerate(C64_PALETTE):
            self.palette[i * 3:i * 3 + 3] = bytes(rgb)
        self.palette[255 * 3:256 * 3] = b'\xff\xff\xff'
        self.log_enabled = True
        self.api_active = False
        self.mode = MODE_GRAPHICS
        self.txt_screen = bytearray([0x20]) * TXT_CELLS
        self.txt_fg = bytearray([TXT_DEF_FG]) * TXT_CELLS
        self.txt_bg = bytearray([TXT_DEF_BG]) * TXT_CELLS
        self.txt_charset = gpu64font.CHARSET_LOWER
        self.health_samples = 0
        self.flips_posted = 0

        # --- class 1 (the retained scene graph) ------------------------
        self.c1_init()

        # --- class 2 (raster) -----------------------------------------
        # ids are 1..255; each entry is (w, h, texels) with texels stored
        # column-major, texel(u, v) at texels[u * h + v].
        self.rtex = {}
        self.rarena_used = 0
        self.rview = (0, 0, FB_W, FB_H)
        self.rcolormap = None
        self.rlevels = 0
        self.rbatch = [0, 0, 0]          # accepted, rejected, pixels
        self.rcam = dict(x=0, y=0, ang=0, flags=0, eye=0x0080, ceil=0x0200,
                         proj=0xA000, floorc=0, ceilc=0, horizon=0)
        self.rsectors = []
        # Milestone 9's polygon layer: a 3D camera that has never been set
        # (proj 0), and the two level-lifetime tables a face record indexes.
        self.rcam3 = dict(x=0, y=0, z=0, yaw=0, pitch=0, proj=0, flags=0)
        self.rverts = []
        self.rtexinfo = []
        # Milestone 13's resident world: poly records in wire form, kept
        # across frames so DRAW_WORLD is ten register writes and no DMA.
        self.rworld = bytearray()
        self.rworldn = 0
        # Milestone 12's dynamic point lights: eight slots, each either None
        # (off) or (x, y, z, r2, fall).
        self.rlights = [None] * RASTER_MAX_LIGHTS
        self.rrequested = 0
        # Persistent, exactly like the firmware's static s_ZBuf: DRAW_SECTORS
        # clears it and DRAW_THINGS reads what DRAW_SECTORS left.
        self.zbuf = [0xFFFF] * (FB_W * FB_H)

        self.vb_armed = False
        self.flip_pending = False
        self.dispatches = 0
        self.reg_writes = 0
        # Every read of a gpu64-window register, the read-side twin of
        # reg_writes. Reported through GET_HEALTH bytes 48-63, the firmware's
        # bus-sampling instrument (Source/Firmware/gpu64_busstats.h), so a
        # probe that compares its own count against the firmware's can be
        # desk-checked here first -- on a bus that never drops anything, the
        # two must agree exactly.
        self.reg_reads = 0
        # The bad-address half of the bus-sampling instrument. A simulated
        # bus never mis-samples anything on its own; these move only when
        # runsim.py's --bus-fault injector is asked to move them, which is
        # how a probe's verdict tree gets exercised on a PC instead of
        # being first run at the bench.
        self.reg_reads_bad = 0
        self.reg_writes_bad = 0
        self.last_bad_addr = 0
        self.or_bad_addr = 0
        self.and_bad_addr = 0xFF
        self.last_bad_write = 0
        # The write-side twin of or_/and_bad_addr, GET_HEALTH 96-97.
        self.or_bad_write = 0
        self.and_bad_write = 0xFF
        self.bad_write_nib = [0] * 16
        # The model has no multiplexer, so these stay zero -- which is the
        # correct expectation for a host run and makes the demo's MUX row a
        # pure hardware reading.
        self.mux_miss = 0
        self.mux_fixed = 0
        self.mux_unfixed = 0
        # The host has no bus and no deadline, so the read window is
        # unbounded: minSlack saturates at 255 and nothing ever expires.
        self.read_slack_min = 255
        self.read_slack_over = 0
        self.read_slack_expired = 0
        self.read_slack_expired_big = 0
        self.read_slack_expired_mux = 0
        # The same two totals split by command class, so a demo's 3D traffic
        # can be told apart from the class 0 text and HUD it also draws --
        # the class 1 loop owns the framebuffer and cannot do the latter at
        # all, so a before/after that lumped them together would credit
        # class 1 with work that merely moved to the C64's own screen.
        # Register writes are attributed to the class of the dispatch they
        # were staged for, which is exact: ARG/ID writes always precede the
        # CMD_LO that consumes them.
        self.class_disp = [0, 0, 0, 0]
        self.class_writes = [0, 0, 0, 0]
        self.pending_writes = 0
        self.reu_cmd = 0
        self.reu_c64 = 0
        self.reu_addr = 0
        self.reu_len = 0
        self.reu_ctrl = 0
        # Advanced by the runner; a vblank is "every frame_period_us".
        self.now_us = 0
        self._next_vb = frame_period_us

    # --- time ----------------------------------------------------------
    def advance(self, us):
        """Let wall-clock time pass. Called by the runner between
        instructions so a poll loop waiting on a vblank can finish."""
        if not self.calibrated:
            return
        self.now_us += us
        while self.now_us >= self._next_vb:
            self._next_vb += self.frame_period_us
            self.status |= ST_VB_PEND
            if self.flip_pending:
                self.flip_pending = False
                self._commit_flip()
                self.status &= ~ST_BUSY
                # A class-1 loop frame pushed while that flip was outstanding
                # only becomes committable now. See c1_push_frame().
                #
                # Raising FRAME_READY HERE -- on the frame clock, with no
                # dispatch involved -- is deliberate and is the behaviour the
                # API documents. Do not "fix" this to fire only on a command:
                # the firmware did exactly that until 2026-09-08 (pollLoopFrame()
                # was reachable only from gpu64_3dDispatch()), which made a
                # read-only STATUS poll loop unable to ever see the bit rise,
                # and cost the Quake demo a 133 ms freeze on 2% of its frames
                # at the bench. gpu64_vsyncCommitFlip() now harvests from
                # inside its DMA hold, which is what this line models.
                if self.c1_ready_deferred:
                    self.c1_ready_deferred = False
                    if self.c1_render_us and self.now_us < self.c1_done_us:
                        self.c1_ready_on_disp = True    # finishes after it
                    else:
                        self.status |= ST_FRAME_READY
                        self.result = self.c1_frame_page

    def _prepare_flip(self):
        """The half of a flip CGpu64FrameBuffer::PrepareFlip() performs.

        Queues the current draw page for display and hands the draw page over
        immediately -- to the page that is neither still being scanned out
        (visible_page: a posted flip does not take effect until the display
        hardware's own next vsync, so the old page can stay up for another
        frame) nor the one just queued.

        The hand-over is here, not in _commit_flip(), because a class 1
        autonomous frame starts rendering as soon as SCENE_COMMIT returns and
        must not be aimed at the page about to go on screen -- Stage 16's
        Finding 2, see dev/progress_tracker.md."""
        self.pending_visible = self.draw_page
        # GET_HEALTH byte 18: what the Pi thinks it presented, against what
        # the program thinks it asked for. The firmware counts posts, which
        # is this half of the flip and not _commit_flip()'s.
        self.flips_posted += 1
        # The C carries a defensive `m_nDrawPage = m_nVisiblePage` default
        # ahead of this loop, for the case where the two exclusions cover
        # every page. With FB_PAGES == 3 that is unreachable on both sides,
        # so it is left out here rather than mirrored -- but if FB_PAGES ever
        # changes, change it in both places or the oracle stops agreeing.
        for p in range(FB_PAGES):
            if p != self.visible_page and p != self.pending_visible:
                self.draw_page = p
                break

    def _commit_flip(self):
        """The other half: what actually goes on screen. The draw page was
        already advanced by _prepare_flip()."""
        self.visible_page = self.pending_visible

    # --- the REU controller ------------------------------------------
    # Only enough of a 1764 to move a block: the suite uses it to get bytes
    # into REU space so that the "space = 1" side of a blob descriptor is
    # exercised at all. Everything else about the REU is somebody else's
    # test.
    def reu_write(self, off, val):
        if off == 0x01:                                 # command
            self.reu_cmd = val
            if val & 0x80:                              # execute
                self.reu_execute()
        elif 0x02 <= off <= 0x03:
            i = off - 0x02
            self.reu_c64 = (self.reu_c64 & ~(0xFF << (8 * i))) | (val << (8 * i))
        elif 0x04 <= off <= 0x06:
            i = off - 0x04
            self.reu_addr = (self.reu_addr & ~(0xFF << (8 * i))) | (val << (8 * i))
        elif 0x07 <= off <= 0x08:
            i = off - 0x07
            self.reu_len = (self.reu_len & ~(0xFF << (8 * i))) | (val << (8 * i))
        elif off == 0x0A:
            self.reu_ctrl = val

    def reu_read(self, off):
        if off == 0x00:
            return 0x10                                 # status: nothing pending
        return 0xFF

    def reu_execute(self):
        n = self.reu_len if self.reu_len else 0x10000
        kind = self.reu_cmd & 0x03
        c64, ext = self.reu_c64, self.reu_addr
        for i in range(n):
            if kind == 0:                               # C64 -> REU
                self.reu[(ext + i) % len(self.reu)] = self.mem_read((c64 + i) & 0xFFFF)
            elif kind == 1:                             # REU -> C64
                self.mem_write((c64 + i) & 0xFFFF, self.reu[(ext + i) % len(self.reu)])
        # Address autoincrement is the default; the suite does not depend on
        # where the pointers end up, so they are simply advanced.
        self.reu_c64 = (c64 + n) & 0xFFFF
        self.reu_addr = (ext + n) % len(self.reu)
        self.reu_len = 1

    # --- register window ------------------------------------------------
    def read_reg(self, off):
        if off <= REG_REU_LAST:
            return self.reu_read(off)
        if off < REG_CMD_HI:
            return 0xFF                     # nobody drives it: a floating bus
        self.reg_reads += 1
        if off == REG_STATUS:
            return self.status
        if off == REG_ERRCODE:
            return self.err
        if off == REG_RESULT:
            return self.result
        if off == REG_SEQACK:
            return self.seqack
        # gpu64_sniff.h's constant registers (GPU64_SNIFF_ENABLED builds).
        if off == 0x6A:
            return 0xA5
        if off == 0x6B:
            return 0x5A
        if off == 0x6C:
            return self.sniff_syncs & 0xFF
        if off == 0x6D:
            return self.sniff_datas & 0xFF
        return 0xFF

    # --- fault injection (runsim.py --bus-fault) ------------------------
    # An access the polling loop DID service, at an address it sampled
    # wrongly. Only the high nibble can move on real hardware -- A0-A3 come
    # off dedicated GPIOs and A4-A7 off the multiplexed ones -- so the
    # injected damage sets a high-nibble bit and leaves the low nibble
    # alone, which is the fingerprint a probe is looking for. A7 is the bit
    # real faults set (bad addresses always had it), and unlike A6 it takes
    # every register in $50-$68 out of the window.
    def read_reg_missampled(self, off):
        bad = (off | 0x80) & 0xFF
        self.reg_reads += 1
        self.reg_reads_bad += 1
        self.last_bad_addr = bad
        self.or_bad_addr |= bad
        self.and_bad_addr &= bad
        return 0xFF

    def write_reg_missampled(self, off, val):
        bad = (off | 0x80) & 0xFF
        self.reg_writes += 1
        self.pending_writes += 1
        self.reg_writes_bad += 1
        self.last_bad_write = bad
        self.or_bad_write |= bad
        self.and_bad_write &= bad
        i = bad >> 4
        if self.bad_write_nib[i] < 0xFF:
            self.bad_write_nib[i] += 1
        # The write itself is discarded, exactly as the firmware discards a
        # write to an address no register occupies.

    def write_reg(self, off, val):
        val &= 0xFF
        if off <= REG_REU_LAST:
            self.reu_write(off, val)
            return
        if off < REG_CMD_HI:
            return                          # not ours, not REU's
        # Every write into the gpu64 window, counted for its own sake: what a
        # command costs the C64 is these, not the dispatch. Comparing an
        # immediate-mode demo with a retained one is a comparison of this
        # number per frame.
        self.reg_writes += 1
        self.pending_writes += 1
        if off == REG_CMD_HI:
            self.cmd_hi = val
        elif off == REG_CMD_LO:
            self.dispatch(val)
        elif off == REG_ID_LO:
            self.ident[0] = val
        elif off == REG_ID_HI:
            self.ident[1] = val
        elif REG_ARG0 <= off <= REG_ARG15:
            self.arg[off - REG_ARG0] = val
        elif off == REG_SEQ:
            self.seq = val
        elif off == 0x6E:                   # gpu64_sniff.h SNIFF_SYNC
            self.sniff_syncs += 1
        elif off == 0x6F:                   # gpu64_sniff.h SNIFF_DATA
            self.sniff_datas += 1

    # --- argument decoding ----------------------------------------------
    def a_u16(self, i):
        return self.arg[i] | (self.arg[i + 1] << 8)

    def a_s16(self, i):
        return s16(self.a_u16(i))

    def a_blob(self, i):
        space = self.arg[i]
        addr = self.arg[i + 1] | (self.arg[i + 2] << 8) | (self.arg[i + 3] << 16)
        length = self.arg[i + 4] | (self.arg[i + 5] << 8)
        return space, addr, length

    def a_compact(self, i):
        space = self.arg[i]
        addr = self.arg[i + 1] | (self.arg[i + 2] << 8) | (self.arg[i + 3] << 16)
        return space, addr

    # --- blobs ------------------------------------------------------------
    def blob_read(self, space, addr, length):
        if length == 0:
            return ERR_OK, bytearray()
        if space == 1:
            if addr + length > len(self.reu):
                return ERR_OUT_OF_RANGE, None
            return ERR_OK, bytearray(self.reu[addr:addr + length])
        if space != 0:
            return ERR_OUT_OF_RANGE, None
        if addr + length > 65536:
            return ERR_OUT_OF_RANGE, None
        return ERR_OK, bytearray(self.mem_read(addr + i) for i in range(length))

    def blob_write(self, space, addr, data):
        if len(data) == 0:
            return ERR_OK
        if space == 1:
            if addr + len(data) > len(self.reu):
                return ERR_OUT_OF_RANGE
            self.reu[addr:addr + len(data)] = data
            return ERR_OK
        if space != 0:
            return ERR_OUT_OF_RANGE
        if addr + len(data) > 65536:
            return ERR_OUT_OF_RANGE
        # SET_DMA_WINDOW. The destination arrives in ARG bytes nothing
        # covers, so an unwindowed readback is the one way a dropped write
        # makes gpu64 scribble on the C64's own memory.
        if self.dma_win_len and (addr < self.dma_win_base or
                                 addr + len(data) >
                                 self.dma_win_base + self.dma_win_len):
            return ERR_OUT_OF_RANGE
        data = bytearray(data)
        for k in ALIAS_DROP[0]:
            if 0 <= k < len(data):
                end = (k | 0xFF)
                if end < len(data):
                    data[k] = data[end]
        for i, b in enumerate(data):
            self.mem_write(addr + i, b)
        return ERR_OK

    # --- drawing ----------------------------------------------------------
    def page(self):
        return self.pages[self.draw_page]

    def put(self, x, y, c):
        if 0 <= x < FB_W and 0 <= y < FB_H:
            self.page()[y * FB_W + x] = c

    def rect_fill(self, x, y, w, h, c):
        if w <= 0 or h <= 0:
            return
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(FB_W, x + w), min(FB_H, y + h)
        if x0 >= x1 or y0 >= y1:
            return
        p = self.page()
        row = bytes([c]) * (x1 - x0)
        for yy in range(y0, y1):
            p[yy * FB_W + x0:yy * FB_W + x1] = row

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.put(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = err * 2
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    # --- dispatch ---------------------------------------------------------
    def dispatch(self, op):
        self.dispatches += 1
        self.c1_harvest()
        c = self.cmd_hi if self.cmd_hi in (0, 1, 2) else 3
        self.class_disp[c] += 1
        self.class_writes[c] += self.pending_writes
        self.pending_writes = 0
        # Sampled before the publication below, as gpu64_apiDispatch() does
        # it: a caller that increments SEQ before every command and still
        # arrives with seq == seqack lost the SEQ write, not the command.
        if self.seq == self.seqack:
            self.seq_repeat += 1
        self.disp_seq_prev = self.disp_seq
        self.disp_seq = self.seq
        self.disp_op_prev = self.disp_op
        self.disp_op = op
        # Published before the class check, as gpu64_apiDispatch() does it:
        # the echo has to be class-agnostic or a rejected command would look
        # like a lost write.
        self.seqack = self.seq
        # gpu64 (2026-09-10, bench run 14): spend the destructive-opcode key.
        # Here, ahead of every early return and for every class, exactly as
        # gpu64_apiDispatch() does it -- so the key a command staged cannot
        # survive into the command after it. gpu64_api.h has the contract.
        self.api_key = self.arg[KEY_ARG]
        self.arg[KEY_ARG] = 0
        # gpu64 (2026-09-25, bench run 41): the optional per-command check,
        # ahead of every class exactly as gpu64_apiDispatch() makes it.
        if (self.api_key & 0xF0) == KEY_CHECKED:
            n = self.api_key & 0x0F
            x = self.api_key ^ self.cmd_hi ^ op ^ self.ident[0] ^ self.ident[1]
            for i in range(min(n, CHECK_ARG)):
                x ^= self.arg[i]
            if n > CHECK_ARG or (x & 0xFF) != self.arg[CHECK_ARG]:
                self.check_refused += 1
                self.err = ERR_BAD_ARGS
                self.status |= ST_ERROR
                return
            if self.cmd_hi == 1 and op == 0x08:
                self.strict = True
        elif (self.strict and self.cmd_hi == 1 and
              op not in STRICT_EXEMPT):
            # gpu64 (2026-09-27): strict mode, as gpu64_apiDispatch() has it.
            if self.api_key == KEY_DESTRUCTIVE:
                if op == 0x00:
                    self.strict = False
            else:
                self.strict_refused += 1
                self.strict_refused_ops[op] = \
                    self.strict_refused_ops.get(op, 0) + 1
                self.err = ERR_BAD_ARGS
                self.status |= ST_ERROR
                return
        if self.cmd_hi not in (0, 1, 2):
            self.err = ERR_BAD_CLASS
            self.status |= ST_ERROR
            return
        if self.cmd_hi != 0 and self.mode != MODE_GRAPHICS:
            self.last_refuse_state = self.diag_state_byte()
            self.last_refuse_op = op
            self.last_refuse_class = self.cmd_hi
            self.class_refused_mode += 1
            # Nothing outside class 0 has a page to draw into while text mode
            # is up.
            self.err = ERR_UNSUPPORTED
            self.status |= ST_ERROR
            return
        try:
            if self.cmd_hi == 2:
                res = self.execute_r2(op)
            elif self.cmd_hi == 1:
                res = self.execute_r1(op)
            elif (getattr(self, 'c1_loop_running', False) and op < 0x80 and
                  op not in (0x00, 0x06, 0x07, 0x09, 0x0A, 0x0B, 0x0C)):
                # gpu64 (2026-09-24, run 38): the class 1 loop owns the
                # display, so a class 0 op that touches it -- in practice a
                # class 1 command whose CMD_HI arrived as 0 -- is refused,
                # as gpu64_apiDispatch() now does.
                res = ERR_BUSY
            elif (self.strict and op in (0x07, 0x09, 0x0B) and
                  self.api_key != KEY_DESTRUCTIVE and (self.api_key & 0xF0) != KEY_CHECKED and
                  getattr(self, 'c1_loop_running', False) and
                  getattr(self, 'c1_last_commit_us', None) is not None and
                  self.now_us - self.c1_last_commit_us < 500000):
                # gpu64 (2026-09-28): gpu64_apiDispatch()'s class 0 phantom
                # guard. A strict program committing frames never sends
                # these three unkeyed; a new program taking over does so
                # after half a second without a commit.
                self.key_refused += 1
                self.key_refused_op = op
                res = ERR_BAD_ARGS
            else:
                res = self.execute(op)
        except IndexError:
            res = ERR_OUT_OF_RANGE
        self.err = res
        if res == ERR_OK:
            self.status &= ~ST_ERROR
            if not self.api_active and op != 0x07:
                self.log_enabled = False
            self.api_active = True
        else:
            self.status |= ST_ERROR

    def execute(self, op):
        a = self.arg
        if op == 0x00:
            return ERR_OK
        if op == 0x01:                                  # RESET_STATE
            self.status = 0
            self.vb_armed = False
            self.flip_pending = False
            self.draw_page = self.visible_page = self.pending_visible = 0
            self.txt_reset()
            return ERR_OK
        if op == 0x0B:                                  # FULL_RESET
            self.strict = False
            # The whole display back to its post-boot state. Unlike
            # RESET_STATE this also drops the display mode, the palette,
            # the border and the framebuffer contents -- see
            # gpu64_apiFullReset() in Source/Firmware/gpu64_api.cpp.
            self.status = 0
            # RESULT too: the firmware clears both AFTER gpu64_apiFullReset(),
            # because the sync inside it can harvest a loop frame and stamp
            # RESULT on the way past. See the case 0x0B comment in
            # Source/Firmware/gpu64_api.cpp.
            self.result = 0
            self.vb_armed = False
            self.flip_pending = False
            # gpu64 (2026-09-27): and the class 1 loop stops, as
            # gpu64_3dLoopFullReset() does -- or its deferred FRAME_READY is
            # orphaned and SCENE_COMMIT answers BUSY forever.
            self.c1_loop_stop(5)                    # GPU64_LOOPSTOP_FULL_RESET
            self.c1_ready_deferred = False
            self.c1_ready_on_disp = False
            self.mode = MODE_GRAPHICS
            for i, rgb in enumerate(C64_PALETTE):
                self.palette[i * 3:i * 3 + 3] = bytes(rgb)
            for i in range(len(C64_PALETTE), 255):
                self.palette[i * 3:i * 3 + 3] = b'\x00\x00\x00'
            self.palette[255 * 3:256 * 3] = b'\xff\xff\xff'
            for page in self.pages:
                page[:] = bytes(len(page))
            self.draw_page = self.visible_page = self.pending_visible = 0
            self.border = 0
            self.txt_reset()
            return ERR_OK
        if op == 0x02:                                  # VBLANK_ARM
            if a[0] > 1:
                return ERR_BAD_ARGS
            if a[0] == 1:
                if not self.calibrated:
                    return ERR_UNSUPPORTED
                self.vb_armed = True
                self.status |= ST_VB_ARMED
            else:
                self.vb_armed = False
                self.status &= ~ST_VB_ARMED
            return ERR_OK
        if op == 0x03:                                  # VBLANK_ACK
            self.status &= ~ST_VB_PEND
            return ERR_OK
        if op == 0x04:                                  # SET_DRAW_PAGE
            if a[0] >= FB_PAGES:
                return ERR_BAD_ARGS
            self.draw_page = a[0]
            return ERR_OK
        if op == 0x05:                                  # PAGE_FLIP
            if a[0] > 1:
                return ERR_BAD_ARGS
            if self.mode != MODE_GRAPHICS:
                return ERR_UNSUPPORTED
            if a[0] == 1:
                if not self.calibrated:
                    return ERR_UNSUPPORTED
                if self.flip_pending:
                    return ERR_BUSY
                self._prepare_flip()
                self.flip_pending = True
                self.status |= ST_BUSY
                return ERR_OK
            self._prepare_flip()
            self._commit_flip()
            return ERR_OK
        if op == 0x06:                                  # GET_INFO
            space, addr, length = self.a_blob(0)
            if length < 16:
                return ERR_BAD_ARGS
            info = bytearray(16)
            info[0:3] = b'G64'
            info[3], info[4] = 1, 0
            text = self.mode == MODE_TEXT
            w = TXT_W if text else FB_W
            h = TXT_H if text else FB_H
            info[5] = w & 0xFF
            info[6] = w >> 8
            info[7] = h & 0xFF
            info[8] = h >> 8
            info[9] = 8
            info[10] = 1 if text else FB_PAGES
            info[11] = 0x01 | 0x02 | 0x04       # classes 0, 1 and 2
            info[12] = TXT_BORDER_W if text else BORDER_W
            info[13] = TXT_BORDER_H if text else BORDER_H
            info[14] = self.frame_period_us & 0xFF
            info[15] = (self.frame_period_us >> 8) & 0xFF
            return self.blob_write(space, addr, info)
        if op == 0x07:                                  # LOG_ENABLE
            if a[0] > 1:
                return ERR_BAD_ARGS
            self.log_enabled = bool(a[0])
            return ERR_OK
        if op == 0x08:                                  # SET_BORDER
            self.border = a[0]
            return ERR_OK
        if op == 0x09:                                  # VBLANK_SYNC
            if not self.calibrated:
                return ERR_UNSUPPORTED
            if self.flip_pending:
                return ERR_BUSY
            return ERR_OK
        if op == 0x0C:                                  # SET_DMA_WINDOW
            base = a[0] | (a[1] << 8)
            length = a[2] | (a[3] << 8)
            if length and base + length > 65536:
                return ERR_OUT_OF_RANGE
            self.dma_win_base, self.dma_win_len = base, length
            self.result = (a[0] ^ a[1] ^ a[2] ^ a[3] ^ 0xA5) & 0xFF
            return ERR_OK
        if op == 0x0A:                                  # GET_HEALTH
            # A healthy Pi on a PC: no throttling ever, a plausible
            # temperature. The model cannot know the real thing -- what it
            # checks is that a program reads the block correctly.
            space, addr, length = self.a_blob(0)
            if length < 12:
                return ERR_BAD_ARGS
            n = (132 if length >= 132
                 else 128 if length >= 128 else 112 if length >= 112
                 else 96 if length >= 96
                 else 80 if length >= 80
                 else 64 if length >= 64 else 48 if length >= 48
                 else 36 if length >= 36 else 20 if length >= 20 else 12)
            h = bytearray(n)
            self.health_samples += 1
            t = 452                                     # 45.2 C
            h[8], h[9] = t & 0xFF, t >> 8
            h[10], h[11] = t & 0xFF, t >> 8
            if n >= 20:
                # Bytes 12-19 are the flip path's "impossible" counters
                # (Source/Firmware/gpu64_flip.h). Three of the four are
                # mailbox pathologies the model has no mailbox to have, so
                # they stay zero -- but flips posted is exact here, and it is
                # the one a pacing check reads: it is the Pi's own count of
                # the frames a program believes it committed.
                h[18] = self.flips_posted & 0xFF
                h[19] = (self.flips_posted >> 8) & 0xFF
            if n >= 36:
                # Bytes 20-35 are the hold-gap instrument
                # (Source/Firmware/gpu64_holdgap.h). There is no bus and no
                # ARM cycle counter here, so the model reports the same
                # saturated "no sample that close" sentinel the firmware
                # reports when nothing collided, a nominal armPerC64, and a
                # real assert count. That is enough to desk-check that a
                # program reads and renders the block correctly, which is
                # all this opcode's model has ever claimed to do.
                def w16(off, v):
                    v = 0xFFFF if v > 0xFFFF else v
                    h[off], h[off + 1] = v & 0xFF, v >> 8
                w16(20, 0xFFFF)                         # minGap
                w16(22, 0xFFFF)                         # minGapD2C
                w16(24, 1200)                           # armPerC64, ~1.2 GHz
                w16(26, 0)                              # bucket <1 cycle
                w16(28, 0)                              # bucket <2
                w16(30, 0)                              # bucket <4
                w16(32, 0)                              # dispatch->commit close
                w16(34, self.dispatches)                # every hold opened
            if n >= 48:
                # Bytes 36-47 are the hold gate
                # (Source/Firmware/gpu64_holdgate.h). Same claim as above:
                # there is no bus here, so the model reports the shape of a
                # run in which every arm fired on the very next cycle and
                # nothing was ever deferred -- armed == fired == dispatches,
                # ageMax 1, and zero read-modify-writes on CMD_LO.
                w16(36, self.dispatches)                # armed
                w16(38, self.dispatches)                # fired
                w16(40, 1)                              # ageMax, C64 cycles
                w16(42, 0)                              # declined
                w16(44, 0)                              # dispatchDeferred
                w16(46, 0)                              # dispatchFired
            if n >= 64:
                # Bytes 48-63 are the bus-sampling instrument
                # (Source/Firmware/gpu64_busstats.h). Unlike the two blocks
                # above this one the model CAN answer honestly: every count
                # here is exact, because the runner routes every register
                # access through this object and nothing is ever lost on the
                # way. So a probe's arithmetic is desk-checkable, and with
                # runsim.py's --bus-fault injector so is its verdict tree --
                # which is the half that would otherwise reach the bench
                # having never once been executed.
                def w32(o, v):
                    v &= 0xFFFFFFFF
                    h[o] = v & 0xFF
                    h[o + 1] = (v >> 8) & 0xFF
                    h[o + 2] = (v >> 16) & 0xFF
                    h[o + 3] = (v >> 24) & 0xFF
                w32(48, self.reg_reads)
                # Minus one: the firmware counts the CMD_LO write that
                # triggered this dispatch only *after* the command has run
                # and the bus is back (see the call to gpu64_busStatsWrite()
                # under gpu64_holdGapRelease() in rad_reu.cpp), so the block
                # it builds cannot include it. The model increments before
                # dispatching -- that ordering is what attributes the write
                # to the right class -- so it subtracts the difference here
                # rather than moving the counter.
                w32(52, self.reg_writes - 1)
                w16(56, self.reg_reads_bad)
                w16(58, self.reg_writes_bad)
                h[60] = self.last_bad_addr
                h[61] = self.or_bad_addr
                h[62] = self.and_bad_addr
                h[63] = self.last_bad_write
            if n >= 80:
                # Bytes 64-79 are the dispatch-side pair
                # (Source/Firmware/gpu64_busstats.h). Exact here for the
                # same reason the block above is: nothing is lost between
                # the 6502 and this object unless --bus-fault puts it there.
                #
                # dispatches counts this GET_HEALTH itself -- the firmware
                # increments at the top of gpu64_apiDispatch(), before the
                # opcode is even looked at -- so a caller differencing two
                # samples sees its own two calls in the total, exactly as
                # it does on hardware.
                w32(64, self.dispatches)
                w32(68, self.seq_repeat)
                # 72-75: the `sta ARG,y` dummy reads. runsim's 6502 does not
                # emit the dead cycle an indexed store spends reading its own
                # target, so this stays 0 here and is non-zero on hardware --
                # a difference in the emulator, not in the firmware.
                w32(72, self.arg_dummy_reads)
                # The one-shot key on the destructive class 1 opcodes
                # (Source/Firmware/gpu64_apidiag.h). 77 names the opcode of
                # the last refusal, which is the byte that identifies a
                # phantom command at the bench.
                h[76] = min(self.key_refused, 255)
                h[77] = self.key_refused_op
                h[78] = min(self.cam_lost, 255)
                h[79] = min(self.scene_wipes, 255)
            if n >= 128:
                # Bad write addresses by high nibble -- see
                # Source/Firmware/gpu64_busstats.h.
                for i in range(16):
                    h[98 + i] = self.bad_write_nib[i]
                for i, v in enumerate(
                        list(self.mux_miss.to_bytes(4, 'little')) +
                        list(self.mux_fixed.to_bytes(4, 'little'))):
                    h[114 + i] = v
                em = min(65535, self.read_slack_expired_mux)
                h[118] = em & 0xff
                h[119] = em >> 8
                uf = min(65535, self.mux_unfixed)
                h[120] = uf & 0xff
                h[121] = uf >> 8
                h[122] = min(255, self.mux_unfixed)
                h[123] = min(255, self.read_slack_over)
                ex = min(65535, self.read_slack_expired)
                h[124] = ex & 0xff
                h[125] = ex >> 8
                h[126] = max(0, min(255, self.read_slack_min))
                h[127] = min(255, self.read_slack_expired_big)
            if n >= 112:
                h[96] = self.or_bad_write
                h[97] = self.and_bad_write
            if n >= 96:
                # Bytes 80-95 are the class 1 refusal breakdown
                # (Source/Firmware/gpu64_apidiag.h). The model can answer
                # all of it honestly: it refuses in exactly the same four
                # places the firmware does, so a program that commits after
                # a LOOP_STOP -- or draws class 1 in text mode -- sees the
                # same counter move here as it would at the bench. What it
                # cannot reproduce is a refusal it was not asked for, which
                # is precisely the case the bench is being asked about.
                w16(80, self.commit_no_loop)
                w16(82, self.class_refused_mode)
                w16(84, self.commit_no_clock)
                h[86] = self.last_refuse_state
                h[87] = self.loop_stop_cause
                w16(88, self.loop_stops)
                h[90] = self.last_refuse_op
                h[91] = self.last_refuse_class
                h[92] = self.diag_state_byte()
                h[93] = self.loop_stop_seq
                h[94] = self.loop_stop_seq_prev
                h[95] = self.loop_stop_prev_op
            if n >= 132:
                # The self-verifying trailer: magic, then XOR and additive
                # sum over 0-129. Mirrors gpu64_api.cpp -- a client that
                # checks it here is checking the same thing at the bench.
                h[128] = ord('G')
                h[129] = ord('6')
                x = a_ = 0
                for i in range(130):
                    x ^= h[i]
                    a_ = (a_ + h[i]) & 0xff
                h[130] = x
                h[131] = a_
            return self.blob_write(space, addr, h)
        if 0x50 <= op < 0x60:                           # text mode
            return self.text(op)
        if op == 0x10:                                  # CLEAR
            self.rect_fill(0, 0, FB_W, FB_H, a[0])
            return ERR_OK
        if op == 0x20:                                  # SET_PIXEL
            self.put(self.a_s16(0), self.a_s16(2), a[4])
            return ERR_OK
        if op == 0x21:                                  # LINE
            self.line(self.a_s16(0), self.a_s16(2), self.a_s16(4), self.a_s16(6), a[8])
            return ERR_OK
        if op in (0x22, 0x23):                          # RECT / RECT_FILL
            x, y = self.a_s16(0), self.a_s16(2)
            w, h = self.a_u16(4), self.a_u16(6)
            c = a[8]
            if w == 0 or h == 0:
                return ERR_OK
            if op == 0x23:
                self.rect_fill(x, y, w, h, c)
            else:
                self.rect_fill(x, y, w, 1, c)
                self.rect_fill(x, y + h - 1, w, 1, c)
                if h > 2:
                    self.rect_fill(x, y + 1, 1, h - 2, c)
                    self.rect_fill(x + w - 1, y + 1, 1, h - 2, c)
            return ERR_OK
        if op == 0x30:                                  # PAL_SET
            i = a[0]
            self.palette[i * 3:i * 3 + 3] = bytes(a[1:4])
            return ERR_OK
        if op == 0x31:                                  # PAL_LOAD
            space, addr, length = self.a_blob(0)
            first, count = a[6], a[7]
            if count == 0:
                return ERR_OK
            if length != count * 3:
                return ERR_BAD_ARGS
            if first + count > 256:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            self.palette[first * 3:(first + count) * 3] = data
            return ERR_OK
        if op in (0x40, 0x41):                          # BLIT / BLIT_KEYED
            space, addr, length = self.a_blob(0)
            dx, dy = self.a_s16(6), self.a_s16(8)
            w, h = self.a_u16(10), self.a_u16(12)
            if w == 0 or h == 0:
                return ERR_OK
            if length != w * h:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            key = a[14] if op == 0x41 else -1
            for sy in range(h):
                yy = dy + sy
                if not (0 <= yy < FB_H):
                    continue
                for sx in range(w):
                    xx = dx + sx
                    if not (0 <= xx < FB_W):
                        continue
                    v = data[sy * w + sx]
                    if key >= 0 and v == key:
                        continue
                    self.page()[yy * FB_W + xx] = v
            return ERR_OK
        if op == 0x42:                                  # READ_RECT
            space, addr, length = self.a_blob(0)
            sx, sy = self.a_s16(6), self.a_s16(8)
            w, h = self.a_u16(10), self.a_u16(12)
            if w == 0 or h == 0:
                return ERR_OK
            if length != w * h:
                return ERR_BAD_ARGS
            if sx < 0 or sy < 0 or sx + w > FB_W or sy + h > FB_H:
                return ERR_BAD_ARGS
            out = bytearray(w * h)
            p = self.page()
            for yy in range(h):
                out[yy * w:(yy + 1) * w] = p[(sy + yy) * FB_W + sx:(sy + yy) * FB_W + sx + w]
            return self.blob_write(space, addr, out)
        if 0x80 <= op < 0xA0:
            return self.math(op)
        return ERR_BAD_OPCODE

    # --- matrix and vector -------------------------------------------------
    # --- 80x50 text mode ------------------------------------------------
    #
    # Three parallel 4000-byte planes and nothing else: there is no pixel
    # surface here, because the model only ever needs one at PPM time and
    # rendering the planes then (runsim.py's write_ppm) keeps the glyph
    # placement an independent derivation rather than a copy of the
    # firmware's inner loop.

    def txt_reset(self):
        self.txt_screen = bytearray([0x20]) * TXT_CELLS
        self.txt_fg = bytearray([TXT_DEF_FG]) * TXT_CELLS
        self.txt_bg = bytearray([TXT_DEF_BG]) * TXT_CELLS
        self.txt_charset = gpu64font.CHARSET_LOWER

    def txt_put(self, col, row, code, fg, bg):
        if col >= TXT_COLS or row >= TXT_ROWS:
            return                                      # clipped, as SET_PIXEL is
        i = row * TXT_COLS + col
        self.txt_screen[i] = code
        self.txt_fg[i] = fg
        self.txt_bg[i] = bg

    def text(self, op):
        a = self.arg
        if op == 0x50:                                  # TEXT_MODE
            if a[0] > 1:
                return ERR_BAD_ARGS
            if a[0] == self.mode:
                return ERR_OK
            if self.flip_pending:
                return ERR_BUSY
            self.mode = a[0]
            # A mode change REALLOCATES the surface -- CGpu64FrameBuffer::
            # SetMode() re-runs Circle's Initialize(), which re-sends the
            # whole init tag block. So the graphics pages do not survive a
            # switch in either direction: the firmware memsets all three
            # and puts the paging state back to page 0 drawn and visible.
            # A program that carries a framebuffer across a mode change
            # loses it, and this is where it finds that out.
            for page in self.pages:
                page[:] = bytes(len(page))
            self.draw_page = self.visible_page = self.pending_visible = 0
            return ERR_OK
        if op == 0x51:                                  # TEXT_CLEAR
            self.txt_screen = bytearray([0x20]) * TXT_CELLS
            self.txt_fg = bytearray([a[0]]) * TXT_CELLS
            self.txt_bg = bytearray([a[1]]) * TXT_CELLS
            return ERR_OK
        if op == 0x52:                                  # TEXT_PUT
            self.txt_put(a[0], a[1], a[2], a[3], a[4])
            return ERR_OK
        if op == 0x53:                                  # TEXT_WRITE
            space, addr, length = self.a_blob(0)
            if length == 0:
                return ERR_OK
            length = min(length, TXT_COLS)
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            col, row, fg, bg, flags = a[6], a[7], a[8], a[9], a[10]
            if row >= TXT_ROWS or col >= TXT_COLS:
                return ERR_OK
            # Truncated at the right-hand edge, never wrapped.
            for k in range(min(length, TXT_COLS - col)):
                b = data[k]
                if flags & TXT_WRITE_ASCII:
                    b = gpu64font.screen_code(b, self.txt_charset)
                self.txt_put(col + k, row, b, fg, bg)
            return ERR_OK
        if op == 0x54:                                  # TEXT_UPLOAD
            space, addr, length = self.a_blob(0)
            plane = a[6]
            first = a[7] | (a[8] << 8)
            if plane > TXT_PLANE_BG:
                return ERR_BAD_ARGS
            if length == 0:
                return ERR_OK
            if first >= TXT_CELLS or first + length > TXT_CELLS:
                return ERR_OUT_OF_RANGE
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            dst = (self.txt_screen, self.txt_fg, self.txt_bg)[plane]
            dst[first:first + length] = data
            return ERR_OK
        if op == 0x55:                                  # TEXT_CHARSET
            if a[0] > 1:
                return ERR_BAD_ARGS
            self.txt_charset = a[0]
            return ERR_OK
        if op == 0x56:                                  # TEXT_SCROLL
            n = min(a[0], TXT_ROWS)
            if n == 0:
                return ERR_OK
            moved = n * TXT_COLS
            kept = TXT_CELLS - moved
            for plane, fill in ((self.txt_screen, 0x20),
                                (self.txt_fg, a[1]),
                                (self.txt_bg, a[2])):
                plane[0:kept] = plane[moved:TXT_CELLS]
                plane[kept:TXT_CELLS] = bytearray([fill]) * moved
            return ERR_OK
        if op == 0x57:                                  # TEXT_FILL
            col, row = s8(a[0]), s8(a[1])
            w, h = a[2], a[3]
            for y in range(row, row + h):
                if y < 0 or y >= TXT_ROWS:
                    continue
                for x in range(col, col + w):
                    if x < 0 or x >= TXT_COLS:
                        continue
                    self.txt_put(x, y, a[4], a[5], a[6])
            return ERR_OK
        if op == 0x58:                                  # TEXT_REFRESH
            return ERR_OK
        return ERR_BAD_OPCODE

    def txt_surface(self):
        """The text screen as an 8bpp TXT_W x TXT_H buffer of palette indices."""
        buf = bytearray(TXT_W * TXT_H)
        glyphs = gpu64font.FONT[self.txt_charset & 1]
        for row in range(TXT_ROWS):
            for col in range(TXT_COLS):
                i = row * TXT_COLS + col
                code = self.txt_screen[i]
                fg, bg = self.txt_fg[i], self.txt_bg[i]
                if code & 0x80:                         # reverse video
                    fg, bg = bg, fg
                    code &= 0x7F
                g = glyphs[code]
                base = row * TXT_CHAR_H * TXT_W + col * TXT_CHAR_W
                for gy in range(TXT_CHAR_H):
                    bits = g[gy]
                    o = base + gy * TXT_W
                    for gx in range(TXT_CHAR_W):
                        buf[o + gx] = fg if bits & (0x80 >> gx) else bg
        return buf

    def math(self, op):
        a = self.arg
        elem = 2 if (op & 0xF0) == 0x80 else 4
        sub = op & 0x0F
        fixed = elem == 2

        def nbytes(rows, cols):
            n = rows * cols * elem
            return 0 if (n == 0 or n > 65535) else n

        def rd(buf, i):
            return fix_read(buf, i) if fixed else flt_read(buf, i)

        if sub == 0x00:                                 # MAT_MUL
            m, k, n = a[0], a[1], a[2]
            if m == 0 or k == 0 or n == 0:
                return ERR_BAD_ARGS
            ba, bb, bc = nbytes(m, k), nbytes(k, n), nbytes(m, n)
            if not (ba and bb and bc):
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(3)
            spB, adB = self.a_compact(7)
            spC, adC = self.a_compact(11)
            res, A = self.blob_read(spA, adA, ba)
            if res != ERR_OK:
                return res
            res, B = self.blob_read(spB, adB, bb)
            if res != ERR_OK:
                return res
            C = bytearray(bc)
            for i in range(m):
                for j in range(n):
                    if fixed:
                        acc = sum(rd(A, i * k + x) * rd(B, x * n + j) for x in range(k))
                        fix_write_product(C, i * n + j, acc)
                    else:
                        acc = 0.0
                        for x in range(k):
                            acc += rd(A, i * k + x) * rd(B, x * n + j)
                        flt_write(C, i * n + j, acc)
            return self.blob_write(spC, adC, C)

        if sub in (0x01, 0x02):                         # MAT_ADD / MAT_SUB
            m, n = a[0], a[1]
            if m == 0 or n == 0:
                return ERR_BAD_ARGS
            nb = nbytes(m, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(2)
            spB, adB = self.a_compact(6)
            spC, adC = self.a_compact(10)
            res, A = self.blob_read(spA, adA, nb)
            if res != ERR_OK:
                return res
            res, B = self.blob_read(spB, adB, nb)
            if res != ERR_OK:
                return res
            C = bytearray(nb)
            for i in range(m * n):
                if fixed:
                    v = rd(A, i) + rd(B, i) if sub == 1 else rd(A, i) - rd(B, i)
                    fix_store(C, i, v)
                else:
                    v = rd(A, i) + rd(B, i) if sub == 1 else rd(A, i) - rd(B, i)
                    flt_write(C, i, v)
            return self.blob_write(spC, adC, C)

        if sub == 0x03:                                 # MAT_SCALE
            m, n = a[0], a[1]
            if m == 0 or n == 0:
                return ERR_BAD_ARGS
            nb = nbytes(m, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(2)
            spC, adC = self.a_compact(6)
            res, A = self.blob_read(spA, adA, nb)
            if res != ERR_OK:
                return res
            C = bytearray(nb)
            if fixed:
                s = s16(a[10] | (a[11] << 8))
                for i in range(m * n):
                    fix_write_product(C, i, rd(A, i) * s)
            else:
                s = struct.unpack('<f', bytes(a[10:14]))[0]
                for i in range(m * n):
                    flt_write(C, i, rd(A, i) * s)
            return self.blob_write(spC, adC, C)

        if sub == 0x04:                                 # MAT_TRANSPOSE
            m, n = a[0], a[1]
            if m == 0 or n == 0:
                return ERR_BAD_ARGS
            nb = nbytes(m, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(2)
            spC, adC = self.a_compact(6)
            res, A = self.blob_read(spA, adA, nb)
            if res != ERR_OK:
                return res
            C = bytearray(nb)
            for i in range(m):
                for j in range(n):
                    if fixed:
                        fix_store(C, j * m + i, rd(A, i * n + j))
                    else:
                        flt_write(C, j * m + i, rd(A, i * n + j))
            return self.blob_write(spC, adC, C)

        if sub == 0x05:                                 # MAT_IDENTITY
            n = a[0]
            if n == 0:
                return ERR_BAD_ARGS
            nb = nbytes(n, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spC, adC = self.a_compact(1)
            C = bytearray(nb)
            for i in range(n):
                if fixed:
                    fix_store(C, i * n + i, 256)
                else:
                    flt_write(C, i * n + i, 1.0)
            return self.blob_write(spC, adC, C)

        if sub == 0x06:                                 # MAT_INVERSE
            n = a[0]
            if n == 0 or n > 64:
                return ERR_BAD_ARGS
            nb = nbytes(n, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(1)
            spC, adC = self.a_compact(5)
            res, A = self.blob_read(spA, adA, nb)
            if res != ERR_OK:
                return res
            # Gauss-Jordan in double precision whatever the element format,
            # so an 8.8 operand keeps its precision through the inversion.
            M = [[0.0] * (2 * n) for _ in range(n)]
            for i in range(n):
                for j in range(n):
                    v = rd(A, i * n + j)
                    M[i][j] = v / 256.0 if fixed else v
                M[i][n + i] = 1.0
            for col in range(n):
                piv, best = -1, 0.0
                for r in range(col, n):
                    if abs(M[r][col]) > best:
                        best, piv = abs(M[r][col]), r
                if piv < 0 or best < 1e-12:
                    return ERR_SINGULAR
                M[col], M[piv] = M[piv], M[col]
                d = M[col][col]
                for j in range(2 * n):
                    M[col][j] /= d
                for r in range(n):
                    if r == col:
                        continue
                    f = M[r][col]
                    if f == 0.0:
                        continue
                    for j in range(2 * n):
                        M[r][j] -= f * M[col][j]
            C = bytearray(nb)
            for i in range(n):
                for j in range(n):
                    v = M[i][n + j]
                    if fixed:
                        fix_write_product(C, i * n + j, int(round(v * 65536.0)))
                    else:
                        flt_write(C, i * n + j, v)
            return self.blob_write(spC, adC, C)

        return ERR_BAD_OPCODE

    # ==================================================================
    # Class 2 -- the raster layer
    #
    # Written from docs/class2-raster-reference.md, not from
    # Source/Firmware/gpu64_raster_core.cpp. Where this and the firmware
    # disagree, one of them is wrong and the disagreement is the finding --
    # tools/rastercheck exists to surface exactly that, on a PC.
    # ==================================================================

    def r_lookup(self, texid):
        return self.rtex.get(texid)

    def r_light_row(self, light):
        if self.rcolormap is None or self.rlevels == 0:
            return None
        lvl = min(light, self.rlevels - 1)
        return memoryview(self.rcolormap)[lvl * 256:(lvl + 1) * 256]

    def r_set_light(self, slot, x, y, z, radius, strength):
        if radius == 0 or strength == 0:
            self.rlights[slot] = None
            return
        r2 = radius * radius
        self.rlights[slot] = (x, y, z, r2,
                              (strength << RASTER_LIGHT_SHIFT) // r2)

    def r_lights_live(self):
        return any(l is not None for l in self.rlights)

    def r_light_adjust(self, level, wx, wy, wz):
        # Mirrors lightAdjust() in gpu64_raster_core.cpp: falloff linear in
        # d squared, subtracted from the colormap index because higher index
        # is darker.
        sub = 0
        for l in self.rlights:
            if l is None:
                continue
            lx, ly, lz, r2, fall = l
            dx, dy, dz = wx - lx, wy - ly, wz - lz
            d2 = dx * dx + dy * dy + dz * dz
            if d2 >= r2:
                continue
            sub += (fall * (r2 - d2)) >> RASTER_LIGHT_SHIFT
        if sub <= 0:
            return level
        return 0 if sub >= level else level - sub

    def r_put(self, x, y, c):
        self.page()[y * FB_W + x] = c

    def execute_r2(self, op):
        a = self.arg
        if op == 0x00:                                  # RASTER_RESET
            self.rtex = {}
            self.rarena_used = 0
            self.rview = (0, 0, FB_W, FB_H)
            self.rcolormap = None
            self.rlevels = 0
            self.rbatch = [0, 0, 0]
            self.rrequested = 0
            self.rcam = dict(x=0, y=0, ang=0, flags=0, eye=0x0080,
                             ceil=0x0200, proj=0xA000, floorc=0, ceilc=0,
                             horizon=0)
            self.rsectors = []
            self.rcam3 = dict(x=0, y=0, z=0, yaw=0, pitch=0, proj=0, flags=0)
            self.rverts = []
            self.rtexinfo = []
            self.rworld = bytearray()
            self.rworldn = 0
            self.rlights = [None] * RASTER_MAX_LIGHTS
            # The depth buffer's lifecycle matches the firmware's: RESET
            # makes it empty again, so a program that sends things but no
            # sectors does not inherit the previous one's depth.
            self.zbuf = [0xFFFF] * (FB_W * FB_H)
            return ERR_OK

        if op == 0x01:                                  # SET_VIEW
            x, y = self.a_u16(0), self.a_u16(2)
            w, h = self.a_u16(4), self.a_u16(6)
            if w == 0 or h == 0:
                return ERR_BAD_ARGS
            if x + w > FB_W or y + h > FB_H:
                return ERR_BAD_ARGS
            self.rview = (x, y, w, h)
            return ERR_OK

        if op == 0x02:                                  # SET_COLORMAP
            space, addr, length = self.a_blob(0)
            levels = a[6]
            if levels == 0:
                self.rcolormap, self.rlevels = None, 0
                return ERR_OK
            if levels > RASTER_MAX_LEVELS:
                return ERR_BAD_ARGS
            if length != levels * 256:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            self.rcolormap, self.rlevels = data, levels
            return ERR_OK

        if op == 0x03:                                  # RASTER_STATS
            space, addr, length = self.a_blob(0)
            if length < 16:
                return ERR_BAD_ARGS
            acc, rej, pix = self.rbatch
            free_kb = (RASTER_ARENA_BYTES - self.rarena_used) >> 10
            info = bytearray(16)
            info[0:2] = b'R2'
            info[2] = acc & 0xFF
            info[3] = (acc >> 8) & 0xFF
            info[4] = rej & 0xFF
            info[5] = (rej >> 8) & 0xFF
            info[6] = self.rrequested & 0xFF
            info[7] = (self.rrequested >> 8) & 0xFF
            for i in range(4):
                info[8 + i] = (pix >> (8 * i)) & 0xFF
            live = len(self.rtex)
            info[12] = live & 0xFF
            info[13] = (live >> 8) & 0xFF
            info[14] = free_kb & 0xFF
            info[15] = (free_kb >> 8) & 0xFF
            return self.blob_write(space, addr, info)

        if op == 0x04:                                  # FILL_VIEW
            vx, vy, vw, vh = self.rview
            self.rect_fill(vx, vy, vw, vh, a[0])
            # The depth of the pixels this erases goes with them: a pixel
            # just painted background has nothing in it. Without this a
            # thing batch would be occluded by geometry no longer drawn.
            for y in range(vy, vy + vh):
                for x in range(vx, vx + vw):
                    self.zbuf[y * FB_W + x] = 0xFFFF
            return ERR_OK

        if op == 0x10:                                  # UPLOAD_TEXTURE
            space, addr, length = self.a_blob(0)
            texid = self.ident[0] | (self.ident[1] << 8)
            w, h = self.a_u16(6), self.a_u16(8)
            flags = a[10]
            if texid == 0 or texid > RASTER_MAX_TEXTURES:
                return ERR_BAD_ID
            if w == 0 or h == 0 or w > RASTER_MAX_DIM or h > RASTER_MAX_DIM:
                return ERR_BAD_ARGS
            if h & (h - 1):
                return ERR_BAD_ARGS
            if length != w * h:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            need = (w * h + 63) & ~63
            if need > RASTER_ARENA_BYTES - self.rarena_used:
                return ERR_OUT_OF_MEMORY
            self.rarena_used += need
            if flags & 0x01:                            # source is row-major
                col = bytearray(w * h)
                for u in range(w):
                    for v in range(h):
                        col[u * h + v] = data[v * w + u]
                data = col
            self.rtex[texid] = (w, h, data)
            return ERR_OK

        if op == 0x11:                                  # FREE_TEXTURE
            texid = self.ident[0] | (self.ident[1] << 8)
            if texid == 0 or texid > RASTER_MAX_TEXTURES:
                return ERR_BAD_ID
            if texid not in self.rtex:
                return ERR_BAD_ID
            del self.rtex[texid]
            return ERR_OK

        if op == 0x05:                                  # SET_CAMERA
            proj = self.a_u16(10)
            eye, ceil = self.a_s16(6), self.a_s16(8)
            if proj == 0 or eye <= 0 or ceil <= eye:
                return ERR_BAD_ARGS
            self.rcam = dict(x=self.a_s16(0), y=self.a_s16(2), ang=a[4],
                             flags=a[5], eye=eye, ceil=ceil, proj=proj,
                             floorc=a[12], ceilc=a[13],
                             horizon=s8(a[14]))
            return ERR_OK

        if op in (0x20, 0x21):                          # DRAW_COLUMNS / SPANS
            res, recs, count = self.r_pull_batch()
            if res != ERR_OK or count == 0:
                return res
            if op == 0x20:
                self.r_columns(recs, count, a[9])
            else:
                self.r_spans(recs, count)
            return ERR_OK

        if op == 0x22:                                  # DRAW_SPRITE
            return self.r_sprite()

        if op == 0x23:                                  # DRAW_WALLS
            res, recs, count = self.r_pull_batch()
            if res != ERR_OK or count == 0:
                return res
            self.r_walls(recs, count, a[9])
            return ERR_OK

        if op == 0x06:                                  # SET_SECTORS
            space, addr, length = self.a_blob(0)
            count = self.a_u16(6)
            if count == 0:
                self.rsectors = []
                return ERR_OK
            if count > RASTER_MAX_SECTORS:
                return ERR_BAD_ARGS
            if length != count * RASTER_SEC_BYTES:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            built = []
            for i in range(count):
                r = data[i * RASTER_SEC_BYTES:(i + 1) * RASTER_SEC_BYTES]
                f = s16(r[0] | (r[1] << 8))
                c = s16(r[2] | (r[3] << 8))
                if c <= f:
                    # Rejected whole. The table that was working stays.
                    return ERR_BAD_ARGS
                built.append(dict(floor=f, ceil=c, floorc=r[4], ceilc=r[5],
                                  light=r[6], flags=r[7]))
            self.rsectors = built
            return ERR_OK

        if op == 0x07:                                  # SET_CAMERA3D
            proj = self.a_u16(8)
            if proj == 0:
                return ERR_BAD_ARGS
            self.rcam3 = dict(x=self.a_s16(0), y=self.a_s16(2),
                              z=self.a_s16(4), yaw=a[6], pitch=s8(a[7]),
                              proj=proj, flags=a[10])
            return ERR_OK

        if op == 0x08:                                  # SET_LIGHT
            slot = a[0]
            if slot >= RASTER_MAX_LIGHTS:
                return ERR_BAD_ARGS
            self.r_set_light(slot, self.a_s16(1), self.a_s16(3),
                             self.a_s16(5), self.a_u16(7), a[9])
            return ERR_OK

        if op == 0x12:                                  # UPLOAD_VERTS
            space, addr, length = self.a_blob(0)
            count = self.a_u16(6)
            if count == 0:
                self.rverts = []
                return ERR_OK
            if count > RASTER_MAX_VERTS:
                return ERR_BAD_ARGS
            if length != count * RASTER_VERT_BYTES:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            self.rverts = [
                (s16(data[i * 8] | (data[i * 8 + 1] << 8)),
                 s16(data[i * 8 + 2] | (data[i * 8 + 3] << 8)),
                 s16(data[i * 8 + 4] | (data[i * 8 + 5] << 8)))
                for i in range(count)]
            return ERR_OK

        if op == 0x13:                                  # UPLOAD_TEXINFO
            space, addr, length = self.a_blob(0)
            count = self.a_u16(6)
            if count == 0:
                self.rtexinfo = []
                return ERR_OK
            if count > RASTER_MAX_TEXINFO:
                return ERR_BAD_ARGS
            if length != count * RASTER_TEXINFO_BYTES:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            self.rtexinfo = [
                tuple(s16(data[i * 16 + 2 * k] | (data[i * 16 + 2 * k + 1] << 8))
                      for k in range(8))
                for i in range(count)]
            return ERR_OK

        if op == 0x14:                                  # UPLOAD_POLYS
            space, addr, length = self.a_blob(0)
            count = self.a_u16(6)
            first = self.a_u16(8)
            if count == 0:
                if first == 0:
                    self.rworld = bytearray()
                    self.rworldn = 0
                return ERR_OK
            if first + count > RASTER_MAX_WORLD_POLYS:
                return ERR_BAD_ARGS
            if length != count * RASTER_POLY_BYTES:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            end = (first + count) * RASTER_POLY_BYTES
            if len(self.rworld) < end:
                self.rworld.extend(bytes(end - len(self.rworld)))
            self.rworld[first * RASTER_POLY_BYTES:end] = data
            self.rworldn = max(self.rworldn, first + count)
            return ERR_OK

        if op == 0x27:                                  # DRAW_WORLD
            first = self.a_u16(0)
            count = self.a_u16(2)
            self.rrequested = count
            self.rbatch = [0, 0, 0]
            if count == 0:
                return ERR_OK
            if first + count > self.rworldn:
                return ERR_BAD_ARGS
            if self.rcam3['proj'] == 0 or not self.rverts:
                return ERR_BAD_ARGS
            self.r_polys(bytes(self.rworld[first * RASTER_POLY_BYTES:]),
                         count, a[9])
            return ERR_OK

        if op == 0x26:                                  # DRAW_POLYS
            res, recs, count = self.r_pull_batch(RASTER_POLY_BYTES)
            if res != ERR_OK or count == 0:
                return res
            if self.rcam3['proj'] == 0 or not self.rverts:
                return ERR_BAD_ARGS
            self.r_polys(recs, count, a[9])
            return ERR_OK

        if op == 0x24:                                  # DRAW_SECTORS
            res, recs, count = self.r_pull_batch(RASTER_WALL2_BYTES)
            if res != ERR_OK or count == 0:
                return res
            if not self.rsectors:
                return ERR_BAD_ARGS
            self.r_sectors(recs, count, a[9])
            return ERR_OK

        if op == 0x25:                                  # DRAW_THINGS
            res, recs, count = self.r_pull_batch()
            if res != ERR_OK or count == 0:
                return res
            if (a[8] & BATCH_CAM3D) and self.rcam3['proj'] == 0:
                return ERR_BAD_ARGS
            self.r_things(recs, count, a[9], a[8])
            return ERR_OK

        return ERR_BAD_OPCODE

    def r_pull_batch(self, stride=None):
        stride = RASTER_REC_BYTES if stride is None else stride
        space, addr, length = self.a_blob(0)
        count = self.a_u16(6)
        flags = self.arg[8]

        self.rrequested = count
        self.rbatch = [0, 0, 0]
        if count == 0:
            return ERR_OK, None, 0

        want = count * stride + (2 if flags & BATCH_CHECKSUM else 0)
        if length != want or want > 65536:
            return ERR_BAD_ARGS, None, 0

        res, data = self.blob_read(space, addr, length)
        if res != ERR_OK:
            return res, None, 0

        if flags & BATCH_CHECKSUM:
            body = count * stride
            want16 = data[body] | (data[body + 1] << 8)
            if (sum(data[:body]) & 0xFFFF) != want16:
                # "A failed dispatch does nothing" -- nothing is drawn.
                return ERR_BAD_ARGS, None, 0

        return ERR_OK, data, count

    def r_columns(self, recs, count, key):
        vx, vy, vw, vh = self.rview
        for i in range(count):
            r = recs[i * RASTER_REC_BYTES:(i + 1) * RASTER_REC_BYTES]
            x = r[0] | (r[1] << 8)
            y0 = s16(r[2] | (r[3] << 8))
            y1 = s16(r[4] | (r[5] << 8))
            texid, light = r[6], r[7]
            u = r[8] | (r[9] << 8)
            v = s16(r[10] | (r[11] << 8))
            dv = s16(r[12] | (r[13] << 8))
            flags = r[14]

            if x < vx or x >= vx + vw or y1 < y0:
                self.rbatch[1] += 1
                continue
            tex = None
            if texid != 0:
                tex = self.r_lookup(texid)
                if tex is None:
                    self.rbatch[1] += 1
                    continue
            self.rbatch[0] += 1

            if y0 < vy:
                v += dv * (vy - y0)
                y0 = vy
            if y1 >= vy + vh:
                y1 = vy + vh - 1
            if y0 > y1:
                continue

            cmap = self.r_light_row(light)
            if tex is None:
                c = u & 0xFF
                if cmap is not None:
                    c = cmap[c]
                for y in range(y0, y1 + 1):
                    self.r_put(x, y, c)
                self.rbatch[2] += y1 - y0 + 1
                continue

            tw, th, texels = tex
            uu = u % tw
            base = uu * th
            hmask = th - 1
            masked = bool(flags & COL_MASKED)
            for y in range(y0, y1 + 1):
                t = texels[base + ((v >> 8) & hmask)]
                v += dv
                if masked and t == key:
                    continue
                self.r_put(x, y, cmap[t] if cmap is not None else t)
                self.rbatch[2] += 1

    def r_spans(self, recs, count):
        vx, vy, vw, vh = self.rview
        for i in range(count):
            r = recs[i * RASTER_REC_BYTES:(i + 1) * RASTER_REC_BYTES]
            y = s16(r[0] | (r[1] << 8))
            x0 = s16(r[2] | (r[3] << 8))
            x1 = s16(r[4] | (r[5] << 8))
            texid, light = r[6], r[7]
            u = s16(r[8] | (r[9] << 8))
            v = s16(r[10] | (r[11] << 8))
            du = s16(r[12] | (r[13] << 8))
            dv = s16(r[14] | (r[15] << 8))

            if y < vy or y >= vy + vh or x1 < x0:
                self.rbatch[1] += 1
                continue
            tex = None
            if texid != 0:
                tex = self.r_lookup(texid)
                # A span wraps u every pixel, so w has to be a mask too.
                if tex is None or (tex[0] & (tex[0] - 1)):
                    self.rbatch[1] += 1
                    continue
            self.rbatch[0] += 1

            if x0 < vx:
                n = vx - x0
                u += du * n
                v += dv * n
                x0 = vx
            if x1 >= vx + vw:
                x1 = vx + vw - 1
            if x0 > x1:
                continue

            cmap = self.r_light_row(light)
            if tex is None:
                c = u & 0xFF
                if cmap is not None:
                    c = cmap[c]
                for x in range(x0, x1 + 1):
                    self.r_put(x, y, c)
                self.rbatch[2] += x1 - x0 + 1
                continue

            tw, th, texels = tex
            wmask, hmask = tw - 1, th - 1
            for x in range(x0, x1 + 1):
                t = texels[((u >> 8) & wmask) * th + ((v >> 8) & hmask)]
                self.r_put(x, y, cmap[t] if cmap is not None else t)
                u += du
                v += dv
            self.rbatch[2] += x1 - x0 + 1


    # ------------------------------------------------------------------
    # DRAW_WALLS.
    #
    # A line-for-line mirror of gpu64_rasterWalls() in
    # Source/Firmware/gpu64_raster_core.cpp. Written from the same
    # description rather than translated from the C, and then diffed
    # against it by tools/rastercheck -- which is the only reason either
    # one can be trusted about a projection this fiddly.
    # ------------------------------------------------------------------

    NEAR = 0x0040                                       # 0.25 world units

    def r_to_view(self, x, y):
        c = fcos(self.rcam['ang'])
        sn = fsin(self.rcam['ang'])
        dx = x - self.rcam['x']
        dy = y - self.rcam['y']
        return ((dx * sn - dy * c) >> 8, (dx * c + dy * sn) >> 8)

    def r_walls(self, recs, count, key):
        vx, vy, vw, vh = self.rview
        vx0, vx1 = vx, vx + vw
        vy0, vy1 = vy, vy + vh
        cam = self.rcam

        if cam['proj'] == 0 or cam['eye'] <= 0 or cam['ceil'] <= cam['eye']:
            self.rbatch[1] += count
            return

        depth = [0] * FB_W
        proj = cam['proj']
        centre_x = vx0 + vw // 2
        horizon = vy0 + vh // 2 + cam['horizon']
        eye_h = cam['eye']
        top_h = cam['ceil'] - cam['eye']
        page = self.page()

        for i in range(count):
            r = recs[i * RASTER_REC_BYTES:(i + 1) * RASTER_REC_BYTES]
            avx, avz = self.r_to_view(s16(r[0] | (r[1] << 8)),
                                      s16(r[2] | (r[3] << 8)))
            bvx, bvz = self.r_to_view(s16(r[4] | (r[5] << 8)),
                                      s16(r[6] | (r[7] << 8)))
            au = s16(r[10] | (r[11] << 8))
            bu = s16(r[12] | (r[13] << 8))
            texid, light, flags = r[8], r[9], r[14]

            if avz < self.NEAR and bvz < self.NEAR:
                self.rbatch[1] += 1
                continue

            tex = self.rtex.get(texid) if texid != 0 else None
            if texid != 0 and tex is None:
                self.rbatch[1] += 1
                continue

            if avz < self.NEAR:
                t = idiv((self.NEAR - avz) << 16, bvz - avz)
                avx += (bvx - avx) * t >> 16
                au += (bu - au) * t >> 16
                avz = self.NEAR
            elif bvz < self.NEAR:
                t = idiv((self.NEAR - bvz) << 16, avz - bvz)
                bvx += (avx - bvx) * t >> 16
                bu += (au - bu) * t >> 16
                bvz = self.NEAR

            sxa = centre_x + (idiv(avx * proj, avz) >> 8)
            sxb = centre_x + (idiv(bvx * proj, bvz) >> 8)
            if sxb <= sxa:
                self.rbatch[1] += 1
                continue

            self.rbatch[0] += 1

            iza = idiv(1 << 22, avz)
            izb = idiv(1 << 22, bvz)
            uza = (au * iza) >> 8
            uzb = (bu * izb) >> 8

            x0, x1 = max(sxa, vx0), min(sxb - 1, vx1 - 1)
            if x0 > x1:
                continue
            span = sxb - sxa

            if tex is not None:
                tw, th, tdata = tex
                hmask = th - 1

            for x in range(x0, x1 + 1):
                t = x - sxa
                iz = iza + idiv((izb - iza) * t, span)
                if iz <= 0 or iz <= depth[x]:
                    continue
                depth[x] = iz
                z = idiv(1 << 22, iz)
                if z <= 0:
                    continue

                yb = horizon + (idiv(eye_h * proj, z) >> 8)
                yt = horizon - (idiv(top_h * proj, z) >> 8)
                if yb <= yt:
                    continue

                lvl = light
                if not (flags & 0x02):
                    lvl = min(light + (z >> 9), 255)
                cmap = self.r_light_row(lvl)

                if cam['flags'] & 0x01:
                    for y in range(vy0, min(yt - 1, vy1 - 1) + 1):
                        page[y * FB_W + x] = cam['ceilc']
                        self.rbatch[2] += 1
                    for y in range(max(yb, vy0), vy1):
                        page[y * FB_W + x] = cam['floorc']
                        self.rbatch[2] += 1

                ya, yz = max(yt, vy0), min(yb - 1, vy1 - 1)
                if ya > yz:
                    continue

                if tex is None:
                    c = (r[10] | (r[11] << 8)) & 0xFF
                    if cmap is not None:
                        c = cmap[c]
                    for y in range(ya, yz + 1):
                        page[y * FB_W + x] = c
                    self.rbatch[2] += yz - ya + 1
                    continue

                uz = uza + idiv((uzb - uza) * t, span)
                uu = (idiv(uz << 8, iz) >> 8) % tw
                col = uu * th
                dv = idiv(th << 8, yb - yt)
                v = (ya - yt) * dv
                for y in range(ya, yz + 1):
                    tx = tdata[col + ((v >> 8) & hmask)]
                    v += dv
                    if (flags & 0x01) and tx == key:
                        continue
                    page[y * FB_W + x] = cmap[tx] if cmap is not None else tx
                    self.rbatch[2] += 1

    # ------------------------------------------------------------------
    # DRAW_SECTORS. Same projection as r_walls, with the two things a
    # level with steps in it needs: heights that come from a sector table
    # rather than from the camera, and a two-sided wall that draws a band
    # above the far ceiling and a band below the far floor and leaves the
    # middle see-through.
    #
    # Depth is per pixel here, as z in 8.8 with 0xFFFF for empty and
    # nearer meaning smaller -- a portal column has no single depth, so
    # r_walls's one-1/z-per-column buffer cannot express it.
    # ------------------------------------------------------------------

    Z_EMPTY = 0xFFFF

    @staticmethod
    def r_zstore(z):
        if z < 0:
            return 0
        return min(z, Gpu64Model.Z_EMPTY - 1)

    @staticmethod
    def r_lit(base, z, flatlit):
        return base if flatlit else min(base + (z >> 9), 255)

    def r_sectors(self, recs, count, key):
        vx, vy, vw, vh = self.rview
        vx0, vx1 = vx, vx + vw
        vy0, vy1 = vy, vy + vh
        cam = self.rcam

        if cam['proj'] == 0 or not self.rsectors:
            self.rbatch[1] += count
            return

        zbuf = self.zbuf
        for y in range(vy0, vy1):
            for x in range(vx0, vx1):
                zbuf[y * FB_W + x] = self.Z_EMPTY
        proj = cam['proj']
        centre_x = vx0 + vw // 2
        horizon = vy0 + vh // 2 + cam['horizon']
        eye = cam['eye']
        page = self.page()

        def band(x, z, y_top, y_bot, yt, yb, tex, colour, cmap, flags, uu):
            yt, yb = max(yt, vy0), min(yb, vy1)
            if yt >= yb or y_bot <= y_top:
                return
            zs = self.r_zstore(z)
            if tex is None:
                c = cmap[colour] if cmap is not None else colour
                for y in range(yt, yb):
                    if zs >= zbuf[y * FB_W + x]:
                        continue
                    zbuf[y * FB_W + x] = zs
                    page[y * FB_W + x] = c
                    self.rbatch[2] += 1
                return
            tw, th, tdata = tex
            col = (uu % tw) * th
            hmask = th - 1
            dv = idiv(th << 8, y_bot - y_top)
            v = (yt - y_top) * dv
            for y in range(yt, yb):
                if zs >= zbuf[y * FB_W + x]:
                    v += dv
                    continue
                tx = tdata[col + ((v >> 8) & hmask)]
                v += dv
                if (flags & 0x01) and tx == key:
                    continue
                zbuf[y * FB_W + x] = zs
                page[y * FB_W + x] = cmap[tx] if cmap is not None else tx
                self.rbatch[2] += 1

        # The depth a flat writes is never nearer than the wall whose
        # column painted it: a plane is infinite and a sector's floor is
        # not, so without the clamp a low ceiling two rooms away wins the
        # rows above the wall that hides it. Lighting still uses the row's
        # true distance.
        def flat(x, z_wall, yt, yb, drop, colour, base, flatlit):
            yt, yb = max(yt, vy0), min(yb, vy1)
            for y in range(yt, yb):
                dy = y - horizon
                if dy == 0 or (dy > 0) != (drop > 0):
                    continue
                z = idiv(drop * proj, dy << 8)
                if z <= 0:
                    continue
                zs = self.r_zstore(max(z, z_wall))
                if zs >= zbuf[y * FB_W + x]:
                    continue
                cmap = self.r_light_row(self.r_lit(base, z, flatlit))
                zbuf[y * FB_W + x] = zs
                page[y * FB_W + x] = cmap[colour] if cmap is not None else colour
                self.rbatch[2] += 1

        nsec = len(self.rsectors)
        for i in range(count):
            r = recs[i * RASTER_WALL2_BYTES:(i + 1) * RASTER_WALL2_BYTES]
            avx, avz = self.r_to_view(s16(r[0] | (r[1] << 8)),
                                      s16(r[2] | (r[3] << 8)))
            bvx, bvz = self.r_to_view(s16(r[4] | (r[5] << 8)),
                                      s16(r[6] | (r[7] << 8)))
            au = s16(r[8] | (r[9] << 8))
            bu = s16(r[10] | (r[11] << 8))
            front, back, light, flags = r[12], r[13], r[14], r[15]

            if avz < self.NEAR and bvz < self.NEAR:
                self.rbatch[1] += 1
                continue
            if front >= nsec:
                self.rbatch[1] += 1
                continue
            two_sided = back != 0xFF
            if two_sided and back >= nsec:
                self.rbatch[1] += 1
                continue

            fs = self.rsectors[front]
            bs = self.rsectors[back] if two_sided else None

            tex = [None, None, None]
            bad = False
            for k in range(3):
                tid = r[16 + k]
                if tid:
                    tex[k] = self.rtex.get(tid)
                    if tex[k] is None:
                        bad = True
            if bad:
                self.rbatch[1] += 1
                continue
            tex_m, tex_u, tex_l = tex

            if avz < self.NEAR:
                t = idiv((self.NEAR - avz) << 16, bvz - avz)
                avx += (bvx - avx) * t >> 16
                au += (bu - au) * t >> 16
                avz = self.NEAR
            elif bvz < self.NEAR:
                t = idiv((self.NEAR - bvz) << 16, avz - bvz)
                bvx += (avx - bvx) * t >> 16
                bu += (au - bu) * t >> 16
                bvz = self.NEAR

            sxa = centre_x + (idiv(avx * proj, avz) >> 8)
            sxb = centre_x + (idiv(bvx * proj, bvz) >> 8)
            if sxb <= sxa:
                self.rbatch[1] += 1
                continue

            self.rbatch[0] += 1

            iza = idiv(1 << 22, avz)
            izb = idiv(1 << 22, bvz)
            uza = (au * iza) >> 8
            uzb = (bu * izb) >> 8

            x0, x1 = max(sxa, vx0), min(sxb - 1, vx1 - 1)
            if x0 > x1:
                continue
            span = sxb - sxa
            flatlit = bool(flags & 0x02)
            flats = bool(cam['flags'] & 0x01) and not (flags & 0x04)

            for x in range(x0, x1 + 1):
                t = x - sxa
                iz = iza + idiv((izb - iza) * t, span)
                if iz <= 0:
                    continue
                z = idiv(1 << 22, iz)
                if z <= 0:
                    continue

                yf = horizon + (idiv((eye - fs['floor']) * proj, z) >> 8)
                yc = horizon + (idiv((eye - fs['ceil']) * proj, z) >> 8)

                if flats:
                    if not (fs['flags'] & 0x01):
                        flat(x, z, vy0, yc, eye - fs['ceil'], fs['ceilc'],
                             fs['light'], flatlit)
                    flat(x, z, yf, vy1, eye - fs['floor'], fs['floorc'],
                         fs['light'], flatlit)

                cmap = self.r_light_row(self.r_lit(light, z, flatlit))

                uu = 0
                if tex_m is not None or tex_u is not None or tex_l is not None:
                    uz = uza + idiv((uzb - uza) * t, span)
                    uu = idiv(uz << 8, iz) >> 8

                if not two_sided:
                    band(x, z, yc, yf, yc, yf, tex_m, r[19], cmap, flags, uu)
                    continue

                ybf = horizon + (idiv((eye - bs['floor']) * proj, z) >> 8)
                ybc = horizon + (idiv((eye - bs['ceil']) * proj, z) >> 8)

                if bs['floor'] > fs['floor']:
                    band(x, z, ybf, yf, ybf, yf, tex_l, r[21], cmap, flags, uu)
                if bs['ceil'] < fs['ceil'] and not (fs['flags'] & 0x01):
                    band(x, z, yc, ybc, yc, ybc, tex_u, r[20], cmap, flags, uu)

                # The far sector's flats, seen through the window. Nothing
                # else paints them: flats are painted by the columns of the
                # walls standing on them, and a corridor's own side walls
                # seen end-on cover almost no columns.
                if flats:
                    w_top, w_bot = max(yc, ybc), min(yf, ybf)
                    if not (bs['flags'] & 0x01):
                        flat(x, z, w_top, w_bot, eye - bs['ceil'],
                             bs['ceilc'], bs['light'], flatlit)
                    flat(x, z, w_top, w_bot, eye - bs['floor'],
                         bs['floorc'], bs['light'], flatlit)

    # ------------------------------------------------------------------
    # DRAW_THINGS: billboards in world space, depth-tested per pixel
    # against what DRAW_SECTORS left in the buffer. Depth is one value for
    # the whole card, and it is written as well as tested at drawn pixels,
    # so a batch is order-independent.
    # ------------------------------------------------------------------

    def r_things(self, recs, count, key, batch_flags=0):
        vx, vy, vw, vh = self.rview
        vx0, vx1 = vx, vx + vw
        vy0, vy1 = vy, vy + vh
        cam3 = bool(batch_flags & BATCH_CAM3D)
        cam = self.rcam3 if cam3 else self.rcam

        if cam['proj'] == 0:
            self.rbatch[1] += count
            return

        zbuf = self.zbuf
        page = self.page()
        proj = cam['proj']
        centre_x = vx0 + vw // 2
        horizon = vy0 + vh // 2 + (0 if cam3 else cam['horizon'])
        eye = self.rcam['eye']
        c3 = self.rcam3
        cyaw, syaw = fcos(c3['yaw']), fsin(c3['yaw'])
        cpit, spit = fcos(c3['pitch'] & 0xFF), fsin(c3['pitch'] & 0xFF)

        for i in range(count):
            r = recs[i * RASTER_REC_BYTES:(i + 1) * RASTER_REC_BYTES]
            wx, wy = s16(r[0] | (r[1] << 8)), s16(r[2] | (r[3] << 8))
            base = s16(r[4] | (r[5] << 8))
            world_h = r[6] | (r[7] << 8)
            world_w = r[8] | (r[9] << 8)
            texid, light, flags = r[10], r[11], r[12]

            # Eight views: byte 13 is where the thing faces, and the id is
            # the first of eight consecutive ones.
            usetex = texid
            if flags & THING_DIRECTIONAL:
                cx = c3['x'] if cam3 else self.rcam['x']
                cy = c3['y'] if cam3 else self.rcam['y']
                dx, dy = cx - wx, cy - wy
                g = (r[13] - 16) & 0xFF
                cc, ss = fcos(g), fsin(g)
                fx = (dx * cc + dy * ss) >> 8
                fy = (dy * cc - dx * ss) >> 8
                usetex = (texid + octant8(fx, fy)) & 0xFF

            # hbase/htop are heights measured downward from the eye, so
            # the row arithmetic below is one expression for both cameras.
            if cam3:
                dx, dy = wx - c3['x'], wy - c3['y']
                dzb = base - c3['z']
                dzt = dzb + world_h
                fwd = (dx * cyaw + dy * syaw) >> 8
                vxx = (dx * syaw - dy * cyaw) >> 8
                vzz = (fwd * cpit + dzb * spit) >> 8
                vz_top = (fwd * cpit + dzt * spit) >> 8
                hbase = (fwd * spit - dzb * cpit) >> 8
                htop = (fwd * spit - dzt * cpit) >> 8
            else:
                vxx, vzz = self.r_to_view(wx, wy)
                vz_top = vzz
                hbase = eye - base
                htop = hbase - world_h

            if vzz < self.NEAR:
                self.rbatch[1] += 1
                continue
            if vz_top < self.NEAR:
                vz_top = self.NEAR
            if world_w == 0 or world_h == 0:
                self.rbatch[1] += 1
                continue
            tex = self.r_lookup(usetex) if (texid and usetex) else None
            if tex is None:
                self.rbatch[1] += 1
                continue

            self.rbatch[0] += 1

            wpx = idiv(world_w * proj, vzz) >> 8
            sxc = centre_x + (idiv(vxx * proj, vzz) >> 8)
            y_bot = horizon + (idiv(hbase * proj, vzz) >> 8)
            y_top = horizon + (idiv(htop * proj, vz_top) >> 8)
            hpx = y_bot - y_top
            if wpx <= 0 or hpx <= 0:
                continue

            x_left = sxc - wpx // 2
            x0, x1 = max(x_left, vx0), min(x_left + wpx - 1, vx1 - 1)
            y0, y1 = max(y_top, vy0), min(y_top + hpx - 1, vy1 - 1)
            if x0 > x1 or y0 > y1:
                continue

            tw, th, texels = tex
            ustep = (tw << 16) // wpx
            vstep = (th << 16) // hpx

            masked = bool(flags & THING_MASKED)
            flip = bool(flags & THING_FLIPX)
            nodepth = bool(flags & THING_NODEPTH)
            lvl = self.r_lit(light, vzz, bool(flags & THING_FLATLIT))
            if self.r_lights_live():
                # Once per thing, at the billboard's centre.
                lvl = self.r_light_adjust(lvl, wx, wy, base + (world_h >> 1))
            cmap = self.r_light_row(lvl)
            zs = self.r_zstore(vzz)

            ustart = (x0 - x_left) * ustep
            vrow = (y0 - y_top) * vstep

            for sy in range(y0, y1 + 1):
                sv = min(vrow >> 16, th - 1)
                vrow += vstep
                ucol = ustart
                for sx in range(x0, x1 + 1):
                    su = min(ucol >> 16, tw - 1)
                    ucol += ustep
                    if not nodepth and zs >= zbuf[sy * FB_W + sx]:
                        continue
                    if flip:
                        su = tw - 1 - su
                    t = texels[su * th + sv]
                    if masked and t == key:
                        continue
                    if not nodepth:
                        zbuf[sy * FB_W + sx] = zs
                    page[sy * FB_W + sx] = cmap[t] if cmap is not None else t
                    self.rbatch[2] += 1

    # ------------------------------------------------------------------
    # DRAW_POLYS -- milestone 9's polygon layer.
    #
    # Written from docs/class2-raster-reference.md and dev/milestone9_poly_design.md,
    # not from gpu64_raster_core.cpp. Every division goes through idiv() and
    # every shift is arithmetic, because the two implementations have to
    # agree on a truncation, not merely on a picture.
    # ------------------------------------------------------------------

    def r_poly_plane(self, v, plane, proj, kl, kr, kt, kb):
        vx, vy, vz = v[0], v[1], v[2]
        if plane == 0:
            return vz - self.NEAR
        if plane == 1:
            return (vx * proj - kl * vz) >> 8
        if plane == 2:
            return (kr * vz - vx * proj) >> 8
        if plane == 3:
            return (kt * vz - vy * proj) >> 8
        return (vy * proj - kb * vz) >> 8

    def r_poly_clip(self, verts, plane, proj, kl, kr, kt, kb):
        out = []
        n = len(verts)
        for i in range(n):
            a = verts[i]
            b = verts[(i + 1) % n]
            da = self.r_poly_plane(a, plane, proj, kl, kr, kt, kb)
            db = self.r_poly_plane(b, plane, proj, kl, kr, kt, kb)
            if da >= 0:
                out.append(a)
            if (da >= 0) != (db >= 0):
                num, den = -da, db - da
                # 8 components: view x/y/z, s, t, and the world position the
                # dynamic lights need (milestone 12).
                out.append(tuple(a[k] + idiv((b[k] - a[k]) * num, den)
                                 for k in range(8)))
        return out

    def r_polys(self, recs, count, key):
        vx, vy, vw, vh = self.rview
        vx0, vx1 = vx, vx + vw
        vy0, vy1 = vy, vy + vh
        cam = self.rcam3

        if cam['proj'] == 0 or not self.rverts:
            self.rbatch[1] += count
            return

        proj = cam['proj']
        centre_x = vx0 + vw // 2
        centre_y = vy0 + vh // 2
        kl = (vx0 - centre_x) * 256
        kr = (vx1 - centre_x) * 256
        kt = (centre_y - vy0) * 256
        kb = (centre_y - vy1) * 256

        cyaw, syaw = fcos(cam['yaw']), fsin(cam['yaw'])
        cpit = fcos(cam['pitch'] & 0xFF)
        spit = fsin(cam['pitch'] & 0xFF)

        zbuf = self.zbuf
        page = self.page()

        for i in range(count):
            r = recs[i * RASTER_POLY_BYTES:(i + 1) * RASTER_POLY_BYTES]
            first = r[0] | (r[1] << 8)
            nverts, tinfo, texid = r[2], r[3], r[4]
            colour, light, flags = r[5], r[6], r[7]

            if nverts < 3 or nverts > RASTER_MAX_POLY_VERTS:
                self.rbatch[1] += 1
                continue
            if first + nverts > len(self.rverts):
                self.rbatch[1] += 1
                continue

            tex = ti = None
            if texid:
                tex = self.r_lookup(texid)
                if tex is None or (tex[0] & (tex[0] - 1)):
                    # Both coordinates are masked per pixel, so a width that
                    # is not a power of two is a rejected record here even
                    # though the same texture is legal for DRAW_COLUMNS.
                    self.rbatch[1] += 1
                    continue
                if tinfo == 0 or tinfo > len(self.rtexinfo):
                    self.rbatch[1] += 1
                    continue
                ti = self.rtexinfo[tinfo - 1]

            poly = []
            for k in range(nverts):
                wx, wy, wz = self.rverts[first + k]
                dx, dy, dz = wx - cam['x'], wy - cam['y'], wz - cam['z']
                fwd = (dx * cyaw + dy * syaw) >> 8
                rgt = (dx * syaw - dy * cyaw) >> 8
                pvz = (fwd * cpit + dz * spit) >> 8
                pvy = (dz * cpit - fwd * spit) >> 8
                if ti is not None:
                    sc = ((wx * ti[0] + wy * ti[1] + wz * ti[2]) >> 8) + ti[3]
                    tc = ((wx * ti[4] + wy * ti[5] + wz * ti[6]) >> 8) + ti[7]
                else:
                    sc = tc = 0
                poly.append((rgt, pvy, pvz, sc, tc, wx, wy, wz))

            poly = self.r_poly_clip(poly, 0, proj, kl, kr, kt, kb)
            if len(poly) < 3:
                self.rbatch[1] += 1          # entirely behind the near plane
                continue
            for plane in (1, 2, 3, 4):
                if len(poly) < 3:
                    break
                poly = self.r_poly_clip(poly, plane, proj, kl, kr, kt, kb)
            if len(poly) < 3:
                self.rbatch[0] += 1          # off the side: drew nothing
                continue

            scr = []
            for v in poly:
                vz = max(v[2], self.NEAR)
                sx = (centre_x << 8) + idiv(v[0] * proj, vz)
                sy = (centre_y << 8) - idiv(v[1] * proj, vz)
                w = idiv(1 << 30, vz)
                scr.append([sx, sy, w, v[3] * w, v[4] * w,
                            v[5] * w, v[6] * w, v[7] * w])

            n = len(scr)
            area = 0
            for k in range(n):
                a, b = scr[k], scr[(k + 1) % n]
                area += a[0] * b[1] - b[0] * a[1]
            if area == 0:
                self.rbatch[0] += 1
                continue
            if area < 0 and not (flags & POLY_TWOSIDED):
                self.rbatch[1] += 1
                continue

            self.rbatch[0] += 1

            min_y = min(v[1] for v in scr)
            max_y = max(v[1] for v in scr)
            y_top = max((min_y + 255) >> 8, vy0)
            y_bot = min((max_y + 255) >> 8, vy1)

            tw = th = 0
            texels = None
            if tex is not None:
                tw, th, texels = tex
            flatlit = bool(flags & POLY_FLATLIT)
            masked = bool(flags & POLY_MASKED)
            lit = self.r_lights_live()

            for y in range(y_top, y_bot):
                Y = y << 8
                hit_l = hit_r = None
                for k in range(n):
                    a, b = scr[k], scr[(k + 1) % n]
                    if not ((a[1] <= Y < b[1]) or (b[1] <= Y < a[1])):
                        continue
                    num, den = Y - a[1], b[1] - a[1]
                    h = [a[j] + idiv((b[j] - a[j]) * num, den)
                         for j in (0, 2, 3, 4, 5, 6, 7)]
                    if hit_l is None:
                        hit_l = hit_r = h
                    elif h[0] < hit_l[0]:
                        hit_l = h
                    elif h[0] > hit_r[0]:
                        hit_r = h

                if hit_l is None or hit_r[0] <= hit_l[0]:
                    continue

                xs = max((hit_l[0] + 255) >> 8, vx0)
                xe = min((hit_r[0] + 255) >> 8, vx1)
                if xs >= xe:
                    continue

                dx88 = hit_r[0] - hit_l[0]
                off = (xs << 8) - hit_l[0]
                w = hit_l[1] + idiv((hit_r[1] - hit_l[1]) * off, dx88)
                sq = hit_l[2] + idiv((hit_r[2] - hit_l[2]) * off, dx88)
                tq = hit_l[3] + idiv((hit_r[3] - hit_l[3]) * off, dx88)
                dw = idiv((hit_r[1] - hit_l[1]) << 8, dx88)
                ds = idiv((hit_r[2] - hit_l[2]) << 8, dx88)
                dt = idiv((hit_r[3] - hit_l[3]) << 8, dx88)

                lq = [0, 0, 0]
                dl = [0, 0, 0]
                if lit:
                    for j in range(3):
                        d = hit_r[4 + j] - hit_l[4 + j]
                        lq[j] = hit_l[4 + j] + idiv(d * off, dx88)
                        dl[j] = idiv(d << 8, dx88)

                for x in range(xs, xe):
                    ww, ss, tt = w, sq, tq
                    lw = (lq[0], lq[1], lq[2])
                    w += dw
                    sq += ds
                    tq += dt
                    for j in range(3):
                        lq[j] += dl[j]
                    if ww <= 0:
                        continue
                    z = idiv(1 << 30, ww)
                    zs = self.r_zstore(z)
                    if zs >= zbuf[y * FB_W + x]:
                        continue
                    if texels is not None:
                        ui = (idiv(ss, ww) >> 8) & (tw - 1)
                        vi = (idiv(tt, ww) >> 8) & (th - 1)
                        c = texels[ui * th + vi]
                        if masked and c == key:
                            continue
                    else:
                        c = colour
                    lvl = self.r_lit(light, z, flatlit)
                    if lit:
                        lvl = self.r_light_adjust(lvl, idiv(lw[0], ww),
                                                  idiv(lw[1], ww),
                                                  idiv(lw[2], ww))
                    cmap = self.r_light_row(lvl)
                    zbuf[y * FB_W + x] = zs
                    page[y * FB_W + x] = cmap[c] if cmap is not None else c
                    self.rbatch[2] += 1

    def r_sprite(self):
        a = self.arg
        texid = self.ident[0] | (self.ident[1] << 8)
        if texid == 0 or texid > RASTER_MAX_TEXTURES:
            return ERR_BAD_ID
        tex = self.r_lookup(texid)
        if tex is None:
            return ERR_BAD_ID

        x, y = self.a_s16(0), self.a_s16(2)
        w, h = self.a_u16(4), self.a_u16(6)
        light, key = a[8], a[9]
        clip_y0, clip_y1 = self.a_s16(10), self.a_s16(12)
        flags = a[14]

        self.rbatch = [0, 0, 0]
        self.rrequested = 1
        if w == 0 or h == 0:
            self.rbatch[1] = 1
            return ERR_OK
        self.rbatch[0] = 1

        tw, th, texels = tex
        ustep = (tw << 16) // w
        vstep = (th << 16) // h

        vx, vy, vw, vh = self.rview
        x0, x1 = x, x + w - 1
        y0, y1 = y, y + h - 1
        y0 = max(y0, clip_y0)
        y1 = min(y1, clip_y1)
        x0 = max(x0, vx)
        x1 = min(x1, vx + vw - 1)
        y0 = max(y0, vy)
        y1 = min(y1, vy + vh - 1)
        if x0 > x1 or y0 > y1:
            return ERR_OK

        cmap = self.r_light_row(light)
        masked = bool(flags & SPR_MASKED)
        flip = bool(flags & SPR_FLIPX)
        ustart = (x0 - x) * ustep
        vrow = (y0 - y) * vstep

        for sy in range(y0, y1 + 1):
            sv = min(vrow >> 16, th - 1)
            ucol = ustart
            for sx in range(x0, x1 + 1):
                su = min(ucol >> 16, tw - 1)
                ucol += ustep
                if flip:
                    su = tw - 1 - su
                t = texels[su * th + sv]
                if masked and t == key:
                    continue
                self.r_put(sx, sy, cmap[t] if cmap is not None else t)
                self.rbatch[2] += 1
            vrow += vstep
        return ERR_OK
