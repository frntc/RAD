# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
quake_pics.py - the level file's picture section (format v7).

Full-screen and menu pictures for the title, the skill menu and the
intermission, drawn on the HDMI screen by LEVEL_PICTURE ($1A) straight
into a class 0 page. Each is w x h palette indices in the Quake palette;
255 is transparent to a keyed draw, as in Quake's own .lmp files.

The picture indices become PIC_* symbols in the game's include, and are
part of the catalogue hash, so a level from another converter run is
refused rather than showing the wrong picture.
"""
import struct
from collections import OrderedDict

import hondani
import quake_assets

KEY = 255

LMPS = [('CONBACK', 'gfx/conback.lmp'),     # 320x200, the Quake title
        ('QPLAQUE', 'gfx/qplaque.lmp'),     # 32x144, the menu's left plaque
        ('TTL_SGL', 'gfx/ttl_sgl.lmp'),     # 128x24, "SINGLE PLAYER"
        ('LOADING', 'gfx/loading.lmp'),     # 144x24
        ('COMPLETE', 'gfx/complete.lmp'),   # 192x24, intermission heading
        ('INTER', 'gfx/inter.lmp')]         # 160x144, time/secrets/kills
DOTS = 6                                    # gfx/menudot1..6, 16x24
WAD_PICS = ['NUM_%d' % i for i in range(10)] + ['NUM_COLON', 'NUM_SLASH']
SKILLS = ['EASY', 'NORMAL', 'HARD', 'NIGHTMARE']

# The controls menu (games/quake/gpu64_game_controls.inc), laid out as
# Quake's M_Keys_Draw: the actions in its bindnames wording, and a name for
# every key of the C64's matrix, indexed column * 8 + row. The game carries
# the same names for its own screen; keep the two lists in step.
CT_ACTS = ['walk forward', 'backpedal', 'turn left', 'turn right',
           'step left', 'step right', 'run', 'jump / swim up']
KEY_NAMES = ['DEL', 'RETURN', 'CRSR RT', 'F7', 'F1', 'F3', 'F5', 'CRSR DN',
             '3', 'W', 'A', '4', 'Z', 'S', 'E', 'L SHIFT',
             '5', 'R', 'D', '6', 'C', 'F', 'T', 'X',
             '7', 'Y', 'G', '8', 'B', 'H', 'U', 'V',
             '9', 'I', 'J', '0', 'M', 'K', 'O', 'N',
             '+', 'P', 'L', '-', '.', ':', '@', ',',
             'POUND', '*', ';', 'HOME', 'R SHIFT', '=', 'UP ARROW', '/',
             '1', 'LT ARROW', 'CTRL', '2', 'SPACE', 'C=', 'Q', 'RUN STOP']


def lmp(data):
    w, h = struct.unpack_from('<ii', data, 0)
    return w, h, bytes(data[8:8 + w * h])


def conchars(wad_data):
    """The 128x128 console font. It is gfx.wad's one raw lump (type 0x44),
    which quake_assets.read_wad skips; index 0 is its transparent colour."""
    magic, n, dirofs = struct.unpack_from('<4sii', wad_data, 0)
    for i in range(n):
        fp, dsz, sz, ty, cmp, _, nm = struct.unpack_from('<iiibbh16s',
                                                         wad_data,
                                                         dirofs + i * 32)
        if nm.split(b'\0')[0].upper() == b'CONCHARS':
            return wad_data[fp:fp + 128 * 128]
    raise ValueError('gfx.wad has no CONCHARS')


def text(font, s, scale=2, gold=True):
    """s in the console font at `scale`, transparent (KEY) background. The
    gold half (char + 128) is what Quake's menus print with."""
    w, h = 8 * len(s) * scale, 8 * scale
    out = bytearray([KEY]) * (w * h)
    for i, ch in enumerate(s):
        c = ord(ch) + (128 if gold else 0)
        sx, sy = (c & 15) * 8, (c >> 4) * 8
        for y in range(8):
            for x in range(8):
                v = font[(sy + y) * 128 + sx + x]
                if v == 0:
                    continue
                for dy in range(scale):
                    row = (y * scale + dy) * w + (i * 8 + x) * scale
                    for dx in range(scale):
                        out[row + dx] = v
    return w, h, bytes(out)


def build(pak_f, pak_ents, pak_read):
    """Returns ([(w, h, pixels)], OrderedDict of PIC_* symbols)."""
    pics, sym = [], OrderedDict()

    def add(name, pic):
        sym['PIC_' + name] = len(pics)
        pics.append(pic)

    add('HONDANI', hondani.hdmi_pixels())
    for name, path in LMPS:
        add(name, lmp(pak_read(pak_f, pak_ents, path)))
    # What Quake's menus are drawn over: the console background through
    # Draw_FadeScreen, which blacks out every other pixel in a checkerboard.
    w, h, px = pics[sym['PIC_CONBACK']]
    add('CONFADE', (w, h, bytes(0 if (x ^ y) & 1 else px[y * w + x]
                                for y in range(h) for x in range(w))))
    for i in range(DOTS):
        p = lmp(pak_read(pak_f, pak_ents, 'gfx/menudot%d.lmp' % (i + 1)))
        add('DOT%d' % i, p)
    wad_data = pak_read(pak_f, pak_ents, 'gfx.wad')
    wad = quake_assets.read_wad(wad_data)
    for name in WAD_PICS:
        add(name, wad[name])
    font = conchars(wad_data)
    for s in SKILLS:
        add('SK_' + s, text(font, s))
    add('SK_CONTROLS', text(font, 'CONTROLS'))     # the menu's fifth line
    ttl = lmp(pak_read(pak_f, pak_ents, 'gfx/ttl_cstm.lmp'))
    add('TTL_CSTM', ttl)
    sym['TTL_CSTM_X'] = (320 - ttl[0]) // 2
    add('CT_HINT0', text(font, 'RETURN or fire to change', 1))
    add('CT_HINT1', text(font, 'Press a key, RETURN cancels', 1))
    for i, s in enumerate(CT_ACTS):
        add('CT_ACT%d' % i, text(font, s, 1))
    add('CT_DEFAULTS', text(font, 'defaults', 1))
    add('CT_DONE', text(font, 'done', 1))
    # M_Keys_Draw's cursor: console characters 12/13 blinking, '=' while
    # waiting for the key. Drawn with M_DrawCharacter, i.e. not gold.
    add('CT_CUR0', text(font, chr(12), 1, gold=False))
    add('CT_CUR1', text(font, chr(13), 1, gold=False))
    add('CT_GRAB', text(font, '=', 1, gold=False))
    for i, s in enumerate(KEY_NAMES):
        add('KEY%d' % i, text(font, s, 1))
    # The pause menu (<- in play): Quake's PAUSED plaque as the title, then
    # RESUME and the skill menu's CONTROLS label, with the menu dot.
    pause = lmp(pak_read(pak_f, pak_ents, 'gfx/pause.lmp'))
    add('PAUSE', pause)
    sym['PAUSE_X'] = (320 - pause[0]) // 2
    add('PM_RESUME', text(font, 'RESUME'))
    # The skill menu's start-level line, one picture per pack slot: the
    # game draws PIC_LV_E1M1 + slot.
    for i in range(8):
        add('LV_E1M%d' % (i + 1), text(font, 'LEVEL E1M%d' % (i + 1)))
    # The pause menu's third line, back to the skill menu. After the level
    # lines so every earlier picture keeps its number.
    add('PM_MAINMENU', text(font, 'MAIN MENU'))
    # The controls menu's mouse line, its value drawn with the KEY digit
    # pictures. Last again, for the same reason.
    add('CT_MOUSE', text(font, 'mouse speed', 1))
    sym['PIC_COUNT'] = len(pics)
    # Where the logo sits on both screens, and its palette base.
    sym['LOGO_X'], sym['LOGO_Y'] = hondani.X, hondani.Y
    sym['LOGO_BASE'] = hondani.LOGO_BASE
    return pics, sym


def section(pics, put):
    """Lay the pixels out with put() and return the section's bytes:
    u16 count, u16 0, then count x <HHI> w, h, blob offset."""
    tab = [struct.pack('<HHI', w, h, put(px)) for w, h, px in pics]
    return struct.pack('<HH', len(pics), 0) + b''.join(tab)
