# GPU cartridge for the C64

This project utilizes RAD hardware cartridge for the Commodore 64 which provides REU and GeoRAM memory and prg loading capabilities. The cartridge is quite generic card that bridges C64 expansion port TTL levels to Raspberry 3A+ GPIO interface. Due to the generic design it is possible to use it for different purposes, including using the RPI as HDMI interface to screen. The RPI is using bare metal code Circle v49.

This project is to prove a concept of using RPI and its HDMI connection, to keep frame buffer and back buffer in the RPI memory. Additionally, the API (IO address memory space) does not need to be limited to transferring graphics data from C64 to the framebuffer. It can also provide API for graphical operations in 2D and 3D, including high-level OpenGL-like API handling the whole 3D scene. The graphical mode will be 320x200 with 256 colors selectable from a palette of 16 million colors. Having one pixel defined as a single byte is much more practical than using 4 bits per pixel as in the original C64. Ideally, the gpu cartridge will sniff the C64 bus for any operations targeting VICII chip and replicate native C64 graphical operations. The code that will be able to display graphics originally target for VICII chip can be repurposed from the VICE project. The 2D/3D API needs to be designed from scratch getting ideas from existing popular APIs.

These are two separate, mutually exclusive modes, not one merged feature: the 320x200x256 mode is a **pure framebuffer** — no sprite objects, no sprite/background collision detection, none of VIC-II's other stateful hardware features. Sprites and collisions only exist in the VIC-II bus-sniffing/replication mode. See the progress tracker's milestone 7 note for how the two modes stay separate.

## Operating modes

gpu64 has **three** mutually exclusive operating modes, not two — all sharing one property that must never be violated: **the C64 is never DMA-halted for anything but a brief, bounded burst.** There is no mode, present or planned, where the cartridge takes the bus for the duration of a session. The C64 always free-runs; the cartridge only ever steals a few cycles at a time, and only when it actually needs to move bytes.

1. **VIC-II mirror mode** (milestone 3, proven on hardware). C64 runs its own program with its native VIC-II fully in charge; gpu64 periodically steals the bus for one brief, predictable burst (`gpu64_mirrorSnapshot()`) to read screen/colour RAM and mirrors it to HDMI. The C64 never notices.
2. **gpu64 API mode** (milestones 2/4/6 — 2D framebuffer today, 3D later). C64 is fully in charge and only *commands* gpu64 over the IO2 register window; rendering happens GPU-side (see [api_design.md](api_design.md)). A command that carries a payload (e.g. a blit) causes a real DMA-held burst to pull it — bounded, predictable in length, and over the instant the burst completes. Mutually exclusive with mirror mode: engaging the API stops the mirror (`gpu64ApiActive`, see [rad_reu.cpp](../Source/Firmware/rad_reu.cpp)).
3. **C64 VIC-II only** (existing RAD behaviour, not previously named as a gpu64 mode). gpu64 doesn't touch the bus for graphics at all; the C64's own native video output is the only display in play, HDMI shows nothing. This is RAD's own "no memory expansion" (`meType == None`) launch path — gpu64's code never runs in this mode, so there is trivially no bus interference, but it's worth naming explicitly as the third mode rather than leaving it as an undocumented side effect of a RAD menu setting.

A wrong claim was in circulation during milestone 4 design and is worth recording so it isn't repeated: `reuUsingPolling()`'s `SET_GPIO(bDMA_OUT)` *releases* the bus, it does not hold it — `CLR_GPIO(bDMA_OUT)`/`SET_GPIO(bDMA_OUT)` bracket each individual DMA burst (see `handle_transfer.h`, `gpu64_mirrorSnapshot()`). The C64 free-runs between bursts in every mode above; it always has, since milestone 3.

## Target hardware configurations

gpu64 must coexist with REU across three deployments, in ascending order of constraint:

1. **RAD cartridge alone**, running in REU mode — RAD's own firmware emulates the REU registers, so gpu64 shares the same firmware process as the REU emulation.
2. **C64 Ultimate** — REU is software-defined inside the Ultimate; the expansion port is real hardware. gpu64 talks to the same physical bus as a genuine hardware REU would.
3. **Original C64 with an expansion port multiplier** — a real hardware REU cartridge and the gpu64/RAD cartridge are plugged in side by side, both decoding the same bus simultaneously. This is the tightest constraint: gpu64's IO decoding must never overlap what a real 17xx-series REU chip decodes, or the two will collide on the bus.

## IO address space allocation

> **Current map (2026-09-27):** gpu64's registers moved to $DF50–$DF68 and $DF0B–$DF4F is no longer decoded. The addresses below are historical; see [uci_register_remap_design.md](uci_register_remap_design.md), "As built".

The C64 expansion port exposes two 256-byte IO windows to cartridges: **IO1** ($DE00–$DEFF) and **IO2** ($DF00–$DFFF).

GeoRAM emulation is out of scope for gpu64 — gpu64 is only ever active alongside REU mode, never GeoRAM mode.

A real REU (1700/1750/1764 and clones) only decodes 11 registers within IO2, at $DF00–$DF0A (`IO_ADDRESS & 0x1F` in RAD's own REU emulation in [rad_reu.cpp](../Source/Firmware/rad_reu.cpp)). It does not decode IO1, and it does not decode the rest of IO2 ($DF0B–$DFFF) — that range is open bus as far as a real/faithfully-emulated REU is concerned.

Decision: **leave IO1 untouched entirely** (reserved, unused by gpu64) and **give gpu64 the unused remainder of IO2, $DF0B–$DFFF** (245 bytes). This keeps gpu64 out of the 11 bytes REU actually needs, in all three target configurations:

1. RAD-as-REU: RAD's own firmware owns both the REU registers and the gpu64 registers, no real conflict possible, but keeping the split clean matters for the other two cases.
2. C64 Ultimate: the RAD cartridge's own REU emulation must be switched off (Ultimate supplies REU itself via its software-defined REU); gpu64 then runs standalone against $DF0B–$DFFF while the Ultimate's REU answers $DF00–$DF0A on the same bus.
3. Original C64 + expansion port multiplier + real hardware REU: same reasoning as (2), with a physical 1750-class chip instead of the Ultimate's software one.

**Open question to verify empirically**: this relies on the Ultimate's (and any real REU's) IO2 decode being a true partial decode of $DF00–$DF0A only, not a full-256-byte mirror of those registers. Real 17xx REUs and the Ultimate's REU are expected to behave this way (1750-compatible), but it should be confirmed on hardware — probe $DF0B+ from a test PRG while the Ultimate's REU is active — before the protocol design leans on it.

**gpu64's actual register addresses within $DF0B–$DFFF are moving**: the
current $DF0B–$DF21 layout collides with the Ultimate's Command Interface
(UCI), which optionally maps $DF1B–$DF1F — see
[uci_register_remap_design.md](uci_register_remap_design.md) for the
collision, the decision (shift the whole block to $DF20–$DF36), and the
migration checklist. Not yet implemented; docs/api_design.md still reflects
the old addresses until it lands.

Within $DF0B–$DFFF, register layout is TBD as the API is designed (see progress tracker) — likely a small command/status register set plus a data-latch or DMA-style bulk path for pixel/vertex data, rather than one byte per operation.
