# DOOM on ESP32-S3 over serial

This project runs classic shareware DOOM on an ESP32-S3 and sends video to a
host over a framed serial protocol. It supports **both** output styles:

1. **ANSI terminal video** — 80x50 half-block cells, xterm-256 colours, dirty-cell updates.
2. **Raw framebuffer streaming** — 160x100 indexed RLE frames plus a 768-byte palette frame.

There is **no SD card and no FAT filesystem**. The WAD lives in its own flash
partition and is exposed to the engine as `/doom/doom1.wad` through a tiny
read-only VFS.

Everything — ESP-IDF, toolchains, Python, CMake, Ninja, the WAD, and the host
bridge — is installed under this folder:

```
/mnt/data/supervisor/media/backup_hdd/doom-s3-serial
```

---

## 1. Hardware requirements

- ESP32-S3 board with **PSRAM** and **8 MB flash** (default partition layout).
- USB-Serial-JTAG is used by default. A UART1 fallback is available.
- A terminal with xterm-256 colour support for ANSI mode.

### How do I know whether my board has PSRAM?

Check the module/board marking or part number:

| Marking contains | PSRAM? |
|---|---|
| `N8R8`, `N8R2`, `N16R8` | Yes |
| `N8`, `N16` (no `R` code) | No |

The `R8`/`R2` suffix means 8 MB / 2 MB PSRAM. A bare ESP32-S3 chip has no PSRAM;
the PSRAM must be on the module or board.

The firmware tells you at boot on UART0 and also through the serial bridge as a
framed `LOG` message:

```
[doom] PSRAM: present (8388608 bytes)
[doom] DOOM zone memory: 6 MiB
```

or, if it is missing or too small:

```
[doom] ERROR: No PSRAM detected. This build needs a PSRAM-equipped ESP32-S3 ...
```

The build uses `CONFIG_SPIRAM_IGNORE_NOTFOUND=y`, so a board without PSRAM still
boots far enough to print that clear error instead of hanging in startup.

The default configuration uses **octal-mode PSRAM** (`CONFIG_SPIRAM_MODE_OCT=y`),
which matches ESP32-S3R8 / `N8R8` modules with 8 MiB embedded PSRAM. If your
module is an `N8R2` / ESP32-S3R2 with 2 MiB PSRAM, change both `sdkconfig.defaults`
and `sdkconfig` to `CONFIG_SPIRAM_MODE_QUAD=y` and rebuild — quad and octal
PSRAM are not interchangeable.

`esptool.py chip_id` reports the chip model/revision, and may report an
`Embedded PSRAM` feature for chips with in-package PSRAM. `esptool.py flash_id`
only reports the **flash** chip. Neither reliably detects external PSRAM soldered
onto a module such as `N8R8`/`N8R2`; use the module marking or the bridge log
above.

This firmware needs at least **2 MiB PSRAM**. With 8 MiB it allocates a 6 MiB
DOOM zone; with smaller PSRAM it reduces the zone automatically. The renderer's
40 KiB plane-openings scratch buffer is also allocated from PSRAM at startup, so
keep `CONFIG_SPIRAM_USE_MALLOC=y` if you change the PSRAM configuration.

---

## 2. Build

All commands below assume this folder as the working directory:

```sh
cd /mnt/data/supervisor/media/backup_hdd/doom-s3-serial

export PATH="$PWD/host-bin:$PATH"
export IDF_PATH="$PWD/esp-idf"
export IDF_PATH_FORCE=1
export IDF_TOOLS_PATH="$PWD/esp-idf-tools"
export IDF_PYTHON_ENV_PATH="$PWD/idf-python-env"

. ./esp-idf/export.sh
idf.py set-target esp32s3   # only needed the first time / after removing sdkconfig
idf.py build
```

Use `/bin/sh` (or another POSIX shell) to source `export.sh`; some system
`bash` installations here do not handle its array syntax correctly.

`host-bin/` contains local wrappers for Python 3.11, CMake and Ninja, so no
system-wide installs are needed.

Useful menuconfig options:

```sh
idf.py menuconfig
```

- `DOOM serial port` → USB-Serial-JTAG (default) or UART1 (TX=17, RX=18).
- `DOOM serial video` → ANSI columns/rows, raw width/height, frame cap.

---

## 3. Flash

Flash both the firmware and the WAD:

```sh
tools/flash_all.sh /dev/ttyACM0
```

Or separately:

```sh
idf.py -p /dev/ttyACM0 flash
tools/flash_wad.sh /dev/ttyACM0
```

The WAD is written to flash offset `0x190000` (partition `doom_wad`, size
`0x500000`). The included `doom1.wad` is the 4,196,020-byte shareware IWAD from
libsdl.org.

If your port is different, set `ESPPORT`:

```sh
ESPPORT=/dev/ttyUSB0 tools/flash_all.sh
```

For UART1 mode, use the serial port of your USB-UART adapter and make sure its
baud rate matches `CONFIG_DOOM_UART1_BAUD` (default 3,000,000). The baud setting
is ignored by USB-Serial-JTAG.

### Windows

A USB-C to USB-C cable is fine if the board has a USB-C socket. Plug it in,
open Device Manager, and find the port under **Ports (COM & LPT)**, for example
`COM3`.

The Linux shell scripts and Linux-local toolchain folders are not usable from
native Windows. To flash the already-built binaries, copy this project folder to
Windows and run:

```bat
tools\flash_all.bat COM3
```

The first run creates `win-venv\` inside the project and installs `esptool` and
`pyserial` there. It flashes the bootloader, partition table, firmware, and WAD
in one step.

To inspect the board from Windows:

```bat
tools\board_info.bat COM3
```

This runs `esptool chip_id`, `esptool flash_id`, and `espefuse summary`. The chip
model/revision appears in the `chip_id` output, and flash information appears in
`flash_id`. The `PSRAM_CAP` eFuse field only covers **in-package/embedded** PSRAM;
it will not show external PSRAM soldered onto an `N8R8`/`N8R2` module. Use the
bridge boot log described in section 1 for the actual PSRAM detected by the
firmware.

If no COM port appears, try another data-capable USB-C cable or USB-C port.
Windows 10/11 normally loads the USB Serial/JTAG driver automatically; otherwise
install the ESP-IDF Windows tools.

To rebuild the firmware on Windows, install ESP-IDF for Windows and use
`idf.py`; do not reuse the Linux `host-bin/`, `esp-idf/`, or `esp-idf-tools/`
folders.

---

## 4. Run the host bridge

ANSI mode (device renders the image):

```sh
idf-python-env/bin/python tools/bridge.py --port /dev/ttyACM0 --mode ansi
```

Raw mode with local terminal display:

```sh
idf-python-env/bin/python tools/bridge.py --port /dev/ttyACM0 --mode raw
```

Raw mode writing each frame as a PPM image (useful for capture/ffmpeg):

```sh
idf-python-env/bin/python tools/bridge.py --port /dev/ttyACM0 --mode raw --raw-out frame.ppm
```

Raw mode streaming PPM frames to stdout:

```sh
idf-python-env/bin/python tools/bridge.py --port /dev/ttyACM0 --mode raw \
    --raw-out - --no-display > stream.ppm
```

ESP-IDF boot logs remain on UART0; DOOM's `printf()` output arrives as framed
`LOG` messages and is printed by the bridge with a `[doom]` prefix.

### Windows bridge

Use Windows Terminal for ANSI colour support:

```bat
tools\bridge.bat --port COM3 --mode ansi
tools\bridge.bat --port COM3 --mode raw
```

`bridge.py` supports Windows arrow keys through `msvcrt`; the Linux raw-terminal
path is unchanged.

### Keys

| Host key | DOOM action |
|---|---|
| W / Up | Forward |
| S / Down | Backward |
| A / Left | Turn left |
| D / Right | Turn right |
| Q / E | Strafe left / right |
| Space | Fire |
| F | Use |
| Enter | Menu select |
| Tab | Menu |
| Esc | Menu / pause |
| Backspace | Backspace |
| A-Z, 0-9 | Type into menus/cheats |
| Ctrl+C | Exit the bridge |

Because `doomgeneric/i_input.c` has a pass-through `TranslateKey()`, the bridge
sends DOOM key codes directly.

---

## 5. Wire protocol

All multi-byte integers are little-endian.

ESP → PC:

```
A5 5A <type:u8> <len:u16> <payload> <crc16:u16>
```

PC → ESP:

```
5A A5 <type:u8> <len:u16> <payload> <crc16:u16>
```

CRC is CCITT CRC-16 (`0x1021`, initial `0xffff`) over `type || len || payload`.

Frame types from ESP:

| Type | Name | Payload |
|---:|---|---|
| 1 | `ANSI` | UTF-8 ANSI escape sequences with cursor moves and dirty cells |
| 2 | `RAW` | `width:u16`, `height:u16`, then row-major RLE pairs `[run, palette_index]` |
| 3 | `LOG` | DOOM `printf()` text |
| 4 | `HELLO` | `DOOMS3`, ANSI cols/rows, raw width/height |
| 5 | `PALETTE` | 256 × RGB bytes, sent once when the palette changes |

Frame types to ESP:

| Type | Name | Payload |
|---:|---|---|
| 0x10 | `KEY` | `[pressed:u8] [doom_key:u8]` |
| 0x11 | `MODE` | `[0=ANSI, 1=RAW]` |

---

## 6. Layout

```
.
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── doom1.wad
├── main/                 # ESP-IDF app, serial I/O, VFS, platform callbacks
├── components/doom_engine/
├── vendor/doomgeneric/   # upstream doomgeneric engine
├── tools/                # host bridge and flash helpers
├── host-bin/             # local python3/cmake/ninja wrappers
├── host-tools/           # downloaded CMake and Ninja
├── python-dist/          # self-contained Python 3.11
├── esp-idf/              # ESP-IDF v5.5.6
├── esp-idf-tools/        # Xtensa toolchain, esptool, etc.
└── idf-python-env/       # ESP-IDF Python virtualenv
```

---

## 7. Troubleshooting

- **`cmake must be available on the PATH`** — put `$PWD/host-bin` first on `PATH`.
- **`Could not automatically detect IDF_PATH`** — export `IDF_PATH` and `IDF_PATH_FORCE=1` as shown above.
- **No picture in the bridge** — confirm the serial port and `--mode`; watch UART0 for boot logs.
- **Garbled ANSI** — use an xterm-256-compatible terminal and an 80-column, 50-row window.
- **Bad frame CRC messages** — wrong port, wrong baud (UART1 mode), or a noisy cable.
- **`ERROR: No PSRAM detected`** — the board has no PSRAM; use an `N8R8`/`N8R2`-style module.
