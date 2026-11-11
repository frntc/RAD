# Install the firmware and run a program

For owners of a [RAD cartridge](https://github.com/frntc/RAD) with a
Raspberry Pi 3A+ who just want to try gpu64. No building needed.

## 1. Download

From the project's **Releases** page, download:

| File | What |
|---|---|
| `gpu64-<version>-sdcard.zip` | Firmware, every program, and game data, laid out for the SD card |
| `gpu64-*.d64` | The same programs as disk images (optional) |
| `SHA256SUMS` | Checksums |

Check the download: `sha256sum -c SHA256SUMS` (Linux/macOS), or
`certutil -hashfile <file> SHA256` on Windows.

## 2. Prepare the SD card

Use a card that already boots your RAD (FAT32, with RAD's bootloader files
and its `config.txt`). If you have none, set one up first from RAD's own
instructions.

1. Copy your card's contents somewhere as a backup.
2. Unzip `gpu64-<version>-sdcard.zip` **onto the card**, merging folders and
   overwriting `kernel_rad.img`. You now have `kernel_rad.img`, `RAD_PRG/`
   and `REU/`.
3. Open `config.txt` on the card and make sure it contains
   `kernel=kernel_rad.img` and a line `armstub=rad-prefetch.bin` that is **not**
   commented out. Without the armstub the picture and menu are unstable.

The card boots `kernel_rad.img`, not `kernel8.img`. Copying the firmware under
the wrong name is the most common reason "nothing changed".

## 3. Run something

1. Card into the Pi in the cartridge, cartridge into the C64, HDMI cable into
   the Pi, power on.
2. In the RAD menu **press `T` until it reads `REU`**. Without this, none of
   gpu64 runs, and nothing tells you why.
3. Choose a program from `RAD_PRG` (start with `gpu64_demo_hello`). The HDMI
   screen should show a picture and the C64 screen a line of text.

Try next: `gpu64_demo_bounce`, `gpu64_demo_cube`, `gpu64_demo_sectors`, then
the games.

## Disk images

Every program is also on a `.d64`, for people who load from a floppy
emulator (SD2IEC, Ultimate, IECBuddy, VICE):

| Image | Contents |
|---|---|
| `gpu64-examples.d64` | `HELLO` ... `CUBE` ... `SECTORS`: simple to advanced |
| `gpu64-probes-1.d64`, `-2.d64` | Hardware probes and the conformance suite (for bug reports) |
| `gpu64-nighthawk.d64` | A flight game |
| `gpu64-stunts.d64` | A racing game. Needs `REU/stunts.reu` on the SD card |
| `gpu64-quake.d64` | Quake programs. Need `REU/quake.reu` (see its README) |

```
LOAD"HELLO",8,1
RUN
```

The program still needs the cartridge in REU mode. Programs that load game
data take it from the SD card's `REU/` folder, so use the SD card image for
games.

## If it does not work

- **HDMI shows a plain copy of the C64 text screen:** no command succeeded.
  Check REU mode (step 3.2), then the firmware name and armstub (step 2).
- **Menu or picture unstable:** `armstub=rad-prefetch.bin` is commented out.
- **A program refuses to start or the HDMI is black:** run
  `gpu64-probes-1.d64`'s first probe and file an issue with what it prints.
