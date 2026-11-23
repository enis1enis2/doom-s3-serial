#!/usr/bin/env python3
"""
Host bridge for doom-s3-serial.

Receives framed video/logs from the ESP32-S3 and sends key frames back.

Modes:
  ansi  - device renders 80x50 half-block ANSI; bridge forwards the bytes
  raw   - device sends 160x100 indexed RLE frames; bridge can display them as
          ANSI locally and/or write PPM frames for capture

Examples:
  python3 tools/bridge.py --port /dev/ttyACM0 --mode ansi
  python3 tools/bridge.py --port /dev/ttyACM0 --mode raw --raw-out frame.ppm
  python3 tools/bridge.py --port /dev/ttyUSB0 --mode raw --no-display
  tools\\bridge.bat --port COM7 --mode ansi
"""

import argparse
import binascii
import os
import struct
import sys
import threading
import time

IS_WINDOWS = os.name == "nt"

if IS_WINDOWS:
    import ctypes
    import msvcrt
else:
    import select
    import termios
    import tty

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial is required; install it in the local Python environment\n")
    raise

ESP_SYNC0 = 0xA5
ESP_SYNC1 = 0x5A
HOST_SYNC0 = 0x5A
HOST_SYNC1 = 0xA5

HOST_KEY = 0x10
HOST_MODE = 0x11

FRAME_ANSI = 1
FRAME_RAW = 2
FRAME_LOG = 3
FRAME_HELLO = 4
FRAME_PALETTE = 5

KEY_UP = 0xAD
KEY_DOWN = 0xAF
KEY_LEFT = 0xAC
KEY_RIGHT = 0xAE
KEY_STRAFE_L = 0xA0
KEY_STRAFE_R = 0xA1
KEY_USE = 0xA2
KEY_FIRE = 0xA3
KEY_TAB = 0x09
KEY_ENTER = 0x0D
KEY_ESCAPE = 0x1B
KEY_BACKSPACE = 0x7F
KEY_RSHIFT = 0x80 + 0x36
KEY_RCTRL = 0x80 + 0x1D


def crc16_ccitt(data, crc=0xFFFF):
    """CCITT CRC-16, polynomial 0x1021, initial value 0xffff.

    binascii.crc_hqx is the C implementation of exactly the same CRC used by
    the firmware; using it avoids a Python bit loop for every payload byte.
    """
    return binascii.crc_hqx(data, crc)


class FrameParser:
    """Parser for A5 5A <type> <len:u16le> <payload> <crc16:u16le>."""

    S_SYNC0, S_SYNC1, S_TYPE, S_LEN0, S_LEN1, S_PAYLOAD, S_CRC0, S_CRC1 = range(8)

    def __init__(self):
        self.state = self.S_SYNC0
        self.frame_type = 0
        self.length = 0
        self.pos = 0
        self.payload = bytearray()
        self.header_crc = 0
        self.crc = 0
        self.rx_crc0 = 0

    def feed(self, data: bytes):
        frames = []
        for byte in data:
            if self.state == self.S_SYNC0:
                if byte == ESP_SYNC0:
                    self.state = self.S_SYNC1

            elif self.state == self.S_SYNC1:
                if byte == ESP_SYNC1:
                    self.state = self.S_TYPE
                else:
                    self.state = self.S_SYNC1 if byte == ESP_SYNC0 else self.S_SYNC0

            elif self.state == self.S_TYPE:
                self.frame_type = byte
                self.header_crc = crc16_ccitt(bytes((byte,)))
                self.state = self.S_LEN0

            elif self.state == self.S_LEN0:
                self.length = byte
                self.header_crc = crc16_ccitt(bytes((byte,)), self.header_crc)
                self.state = self.S_LEN1

            elif self.state == self.S_LEN1:
                self.length |= byte << 8
                self.crc = crc16_ccitt(bytes((byte,)), self.header_crc)
                self.pos = 0
                if self.length <= 65535:
                    self.payload = bytearray(self.length)
                    self.state = self.S_PAYLOAD if self.length else self.S_CRC0
                else:
                    self.state = self.S_SYNC0

            elif self.state == self.S_PAYLOAD:
                self.payload[self.pos] = byte
                self.pos += 1
                if self.pos == self.length:
                    self.state = self.S_CRC0

            elif self.state == self.S_CRC0:
                self.rx_crc0 = byte
                self.state = self.S_CRC1

            elif self.state == self.S_CRC1:
                expected = crc16_ccitt(self.payload, self.crc)
                if self.rx_crc0 != (expected & 0xFF) or byte != ((expected >> 8) & 0xFF):
                    self._log_bad_crc()
                else:
                    frames.append((self.frame_type, bytes(self.payload)))
                self.state = self.S_SYNC0
        return frames

    @staticmethod
    def _log_bad_crc():
        global _BAD_CRC_LAST
        now = time.monotonic()
        if now - _BAD_CRC_LAST >= 1.0:
            sys.stderr.write("[bridge] bad frame CRC\r\n")
            _BAD_CRC_LAST = now


_BAD_CRC_LAST = 0.0


class Sender:
    def __init__(self, ser):
        self.ser = ser
        self.lock = threading.Lock()

    def send(self, frame_type: int, payload: bytes):
        header = bytes((HOST_SYNC0, HOST_SYNC1, frame_type,
                        len(payload) & 0xFF, (len(payload) >> 8) & 0xFF))
        crc = crc16_ccitt(header[2:5])
        crc = crc16_ccitt(payload, crc)
        with self.lock:
            self.ser.write(header + payload + struct.pack("<H", crc))

    def key(self, pressed: bool, key: int):
        self.send(HOST_KEY, bytes((1 if pressed else 0, key & 0xFF)))

    def mode(self, raw: bool):
        self.send(HOST_MODE, bytes((1 if raw else 0,)))


XTERM_LEVELS = (0, 95, 135, 175, 215, 255)
XTERM_BASE = (
    (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
    (0, 0, 128), (128, 0, 128), (0, 128, 128), (192, 192, 192),
    (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
)


def xterm_rgb(idx: int):
    if idx < 16:
        return XTERM_BASE[idx]
    if idx < 232:
        n = idx - 16
        return XTERM_LEVELS[n // 36], XTERM_LEVELS[(n // 6) % 6], XTERM_LEVELS[n % 6]
    v = 8 + (idx - 232) * 10
    return v, v, v


XTERM_RGB = [xterm_rgb(i) for i in range(256)]


def nearest_xterm(r: int, g: int, b: int) -> int:
    best, best_dist = 0, 1 << 60
    for idx, (tr, tg, tb) in enumerate(XTERM_RGB):
        dr, dg, db = r - tr, g - tg, b - tb
        dist = dr * dr * 2 + dg * dg * 3 + db * db
        if dist < best_dist:
            best_dist, best = dist, idx
            if dist == 0:
                break
    return best


def nearest_level(v: int) -> int:
    best, best_dist = 0, 1 << 60
    for idx, level in enumerate(XTERM_LEVELS):
        d = abs(v - level)
        if d < best_dist:
            best_dist, best = d, idx
    return best


def xterm_from_rgb(r: int, g: int, b: int) -> int:
    """O(1) approximation of nearest_xterm, used for averaged colours."""
    high = max(r, g, b)
    low = min(r, g, b)
    if high <= 8:
        return 0
    if high - low <= 12 and 8 <= high <= 238:
        v = (r + g + b) // 3
        idx = max(0, min(23, (v - 8 + 5) // 10))
        return 232 + idx
    if low >= 240:
        return 231
    return (16 + 36 * nearest_level(r) + 6 * nearest_level(g) +
            nearest_level(b))


class RawTerminal:
    """Convert raw indexed frames into dirty-cell half-block ANSI."""

    def __init__(self):
        self.set_size(80, 50)
        self.map = list(range(256))
        self.blend = [0] * 65536
        self.idx_bytes = [bytes((i,)) for i in range(256)]
        self.fg_esc = [f"\x1b[38;5;{i}m".encode() for i in range(256)]
        self.bg_esc = [f"\x1b[48;5;{i}m".encode() for i in range(256)]
        self.block = "\u2580".encode()
        self.have_prev = False

    def set_size(self, cols: int, rows: int):
        self.cols = cols
        self.rows = rows
        self.prev = [-1] * (cols * rows)
        self.cursor = [
            f"\x1b[{cy + 1};{cx + 1}H".encode()
            for cy in range(rows) for cx in range(cols)
        ]
        self.have_prev = False

    def set_palette(self, palette: bytes):
        if len(palette) < 768:
            return
        self.map = [
            nearest_xterm(palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2])
            for i in range(256)
        ]
        self.blend = [0] * 65536
        for a in range(256):
            ar, ag, ab = palette[a * 3], palette[a * 3 + 1], palette[a * 3 + 2]
            for b in range(a, 256):
                br = palette[b * 3]
                bg = palette[b * 3 + 1]
                bb = palette[b * 3 + 2]
                idx = xterm_from_rgb((ar + br + 1) >> 1,
                                     (ag + bg + 1) >> 1,
                                     (ab + bb + 1) >> 1)
                self.blend[a * 256 + b] = idx
                self.blend[b * 256 + a] = idx
        self.have_prev = False
        sys.stdout.write("\x1b[2J\x1b[H")
        sys.stdout.flush()

    def draw(self, payload: bytes):
        if len(payload) < 4:
            return
        width, height = struct.unpack_from("<HH", payload, 0)
        if width == 0 or height == 0:
            return

        fb = bytearray(width * height)
        pos = 4
        for y in range(height):
            x = 0
            base = y * width
            while x < width and pos + 1 < len(payload):
                run = payload[pos]
                idx = payload[pos + 1]
                pos += 2
                if run == 0:
                    break
                end = min(x + run, width)
                fb[base + x:base + end] = self.idx_bytes[idx] * (end - x)
                x = end

        cols, rows = self.cols, self.rows
        cells = [0] * (cols * rows)
        blend = self.blend
        for cy in range(rows):
            y0 = cy * height // rows
            y1 = min((cy + 1) * height // rows - 1, height - 1)
            row0 = y0 * width
            row1 = y1 * width
            for cx in range(cols):
                xa = cx * width // cols
                xb = min((cx + 1) * width // cols - 1, width - 1)
                top = blend[fb[row0 + xa] * 256 + fb[row0 + xb]]
                bot = blend[fb[row1 + xa] * 256 + fb[row1 + xb]]
                cells[cy * cols + cx] = (top << 8) | bot

        out = bytearray()
        last_fg = last_bg = -1
        full = not self.have_prev
        for cy in range(rows):
            cx = 0
            while cx < cols:
                i = cy * cols + cx
                cell = cells[i]
                if not full and self.prev[i] == cell:
                    cx += 1
                    continue

                out += self.cursor[i]
                while cx < cols:
                    i = cy * cols + cx
                    cell = cells[i]
                    if not full and self.prev[i] == cell:
                        break
                    fg, bg = cell >> 8, cell & 0xFF
                    if fg != last_fg:
                        out += self.fg_esc[fg]
                        last_fg = fg
                    if bg != last_bg:
                        out += self.bg_esc[bg]
                        last_bg = bg
                    out += self.block
                    cx += 1

        self.prev = cells
        self.have_prev = True
        if out:
            sys.stdout.buffer.write(out)
            sys.stdout.buffer.flush()


def decode_raw(payload: bytes):
    width, height = struct.unpack_from("<HH", payload, 0)
    fb = bytearray(width * height)
    pos = 4
    for y in range(height):
        x = 0
        base = y * width
        while x < width and pos + 1 < len(payload):
            run, idx = payload[pos], payload[pos + 1]
            pos += 2
            if run == 0:
                break
            end = min(x + run, width)
            fb[base + x:base + end] = bytes((idx,)) * (end - x)
            x = end
    return width, height, fb


def write_ppm(path: str, payload: bytes, palette: bytes):
    width, height, fb = decode_raw(payload)
    rgb = bytearray(width * height * 3)
    if len(palette) >= 768:
        for i, idx in enumerate(fb):
            rgb[i * 3:i * 3 + 3] = palette[idx * 3:idx * 3 + 3]
    data = f"P6\n{width} {height}\n255\n".encode() + bytes(rgb)
    if path == "-":
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
    else:
        with open(path, "wb") as f:
            f.write(data)


def map_key(data: bytes):
    if len(data) == 1:
        ch = data
        table = {
            b"w": KEY_UP, b"W": KEY_UP,
            b"s": KEY_DOWN, b"S": KEY_DOWN,
            b"a": KEY_LEFT, b"A": KEY_LEFT,
            b"d": KEY_RIGHT, b"D": KEY_RIGHT,
            b"q": KEY_STRAFE_L, b"Q": KEY_STRAFE_L,
            b"e": KEY_STRAFE_R, b"E": KEY_STRAFE_R,
            b" ": KEY_FIRE,
            b"f": KEY_USE, b"F": KEY_USE,
            b"\t": KEY_TAB,
            b"\r": KEY_ENTER, b"\n": KEY_ENTER,
            b"\x7f": KEY_BACKSPACE, b"\x08": KEY_BACKSPACE,
            b"\x1b": KEY_ESCAPE,
        }
        if ch in table:
            return table[ch]
        if b"a" <= ch <= b"z":
            return ch[0] - 32
        if b"A" <= ch <= b"Z" or b"0" <= ch <= b"9":
            return ch[0]
    else:
        table = {
            b"\x1b[A": KEY_UP,
            b"\x1b[B": KEY_DOWN,
            b"\x1b[C": KEY_RIGHT,
            b"\x1b[D": KEY_LEFT,
            b"\x1b[Z": KEY_RSHIFT,
        }
        return table.get(data)
    return None


if IS_WINDOWS:
    WINDOWS_SPECIAL_KEYS = {
        "H": b"\x1b[A",  # Up
        "P": b"\x1b[B",  # Down
        "M": b"\x1b[C",  # Right
        "K": b"\x1b[D",  # Left
        "\t": b"\x1b[Z",  # Shift+Tab
        "S": b"\x7f",    # Delete
    }

    def enable_windows_ansi() -> None:
        """Enable xterm-style escape processing in Windows Terminal / console."""
        try:
            kernel32 = ctypes.windll.kernel32
            handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
            mode = ctypes.c_uint32()
            if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
                kernel32.SetConsoleMode(handle, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        except Exception:
            pass

    def fit_windows_console(cols: int, rows: int) -> None:
        """Best-effort resize of a real Windows console to the ANSI grid."""
        class COORD(ctypes.Structure):
            _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]

        class SMALL_RECT(ctypes.Structure):
            _fields_ = [("Left", ctypes.c_short), ("Top", ctypes.c_short),
                        ("Right", ctypes.c_short), ("Bottom", ctypes.c_short)]

        class CONSOLE_SCREEN_BUFFER_INFO(ctypes.Structure):
            _fields_ = [("dwSize", COORD), ("dwCursorPosition", COORD),
                        ("wAttributes", ctypes.c_ushort), ("srWindow", SMALL_RECT),
                        ("dwMaximumWindowSize", COORD)]

        try:
            kernel32 = ctypes.windll.kernel32
            handle = kernel32.GetStdHandle(-11)
            info = CONSOLE_SCREEN_BUFFER_INFO()
            if not kernel32.GetConsoleScreenBufferInfo(handle, ctypes.byref(info)):
                return

            cols = max(1, min(cols, info.dwMaximumWindowSize.X))
            rows = max(1, min(rows, info.dwMaximumWindowSize.Y))

            # Shrink the visible window before shrinking the buffer.
            small = SMALL_RECT(0, 0,
                               min(info.srWindow.Right, cols - 1),
                               min(info.srWindow.Bottom, rows - 1))
            kernel32.SetConsoleWindowInfo(handle, True, ctypes.byref(small))
            kernel32.SetConsoleScreenBufferSize(handle, COORD(cols, rows))
            full = SMALL_RECT(0, 0, cols - 1, rows - 1)
            kernel32.SetConsoleWindowInfo(handle, True, ctypes.byref(full))
            kernel32.SetConsoleCursorPosition(handle, COORD(0, 0))
        except Exception:
            pass

    def read_key() -> bytes:
        ch = msvcrt.getwch()
        if ch in ("\x00", "\xe0"):
            code = msvcrt.getwch()
            return WINDOWS_SPECIAL_KEYS.get(code, code.encode("utf-8", "replace"))
        return ch.encode("utf-8", "replace")

else:
    def enable_windows_ansi() -> None:
        pass

    def fit_windows_console(cols: int, rows: int) -> None:
        pass

    def read_escape() -> bytes:
        data = b"\x1b"
        if select.select([sys.stdin], [], [], 0.03)[0]:
            data += os.read(sys.stdin.fileno(), 8)
        return data

    def read_key() -> bytes:
        data = os.read(sys.stdin.fileno(), 1)
        if data == b"\x1b":
            data = read_escape()
        return data


def main(argv=None):
    parser = argparse.ArgumentParser(description="DOOM ESP32-S3 serial bridge")
    default_port = "COM3" if IS_WINDOWS else "/dev/ttyACM0"
    parser.add_argument("--port", default=os.environ.get("DOOM_PORT", default_port))
    parser.add_argument("--baud", type=int, default=int(os.environ.get("DOOM_BAUD", "3000000")))
    parser.add_argument("--mode", choices=("ansi", "raw"), default="ansi")
    parser.add_argument("--raw-out", help="Write each RAW frame as PPM to this path ('-' for stdout)")
    parser.add_argument("--no-display", action="store_true", help="Do not locally display RAW frames")
    args = parser.parse_args(argv)

    enable_windows_ansi()
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    sender = Sender(ser)
    parser_frames = FrameParser()
    palette = bytearray(768)
    terminal = RawTerminal()
    raw_mode = args.mode == "raw"

    sender.mode(raw_mode)
    sys.stdout.write("\x1b[2J\x1b[H\x1b[?25l")
    sys.stdout.flush()
    sys.stderr.write(f"[bridge] {args.port} @ {args.baud}, mode={args.mode}\r\n")
    sys.stderr.write("[bridge] WASD/arrows move, Q/E strafe, Space fire, F use, "
                     "Esc menu, Ctrl+C exit\r\n")

    stop = threading.Event()

    def reader():
        while not stop.is_set():
            data = ser.read(4096)
            if not data:
                continue
            for frame_type, payload in parser_frames.feed(data):
                if frame_type == FRAME_HELLO:
                    label = payload[:6].decode("ascii", "replace") if len(payload) >= 6 else "?"
                    if len(payload) >= 10:
                        cols, rows = struct.unpack_from("<HH", payload, 6)
                        if 20 <= cols <= 200 and 10 <= rows <= 120:
                            terminal.set_size(cols, rows)
                            fit_windows_console(cols, rows)
                            sys.stderr.write(f"[bridge] firmware: {label}, ANSI {cols}x{rows}\r\n")
                        else:
                            sys.stderr.write(f"[bridge] firmware: {label}\r\n")
                    else:
                        sys.stderr.write(f"[bridge] firmware: {label}\r\n")
                elif frame_type == FRAME_LOG:
                    sys.stderr.write("[doom] " + payload.decode("utf-8", "replace") + "\r\n")
                elif frame_type == FRAME_PALETTE:
                    palette[:] = payload[:768]
                    terminal.set_palette(bytes(palette))
                elif frame_type == FRAME_ANSI:
                    if not raw_mode:
                        sys.stdout.buffer.write(payload)
                        sys.stdout.buffer.flush()
                elif frame_type == FRAME_RAW:
                    if raw_mode:
                        if args.raw_out:
                            write_ppm(args.raw_out, payload, bytes(palette))
                        if not args.no_display:
                            terminal.draw(payload)

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    old_termios = None
    try:
        if not IS_WINDOWS and sys.stdin.isatty():
            old_termios = termios.tcgetattr(sys.stdin.fileno())
            tty.setraw(sys.stdin.fileno())

        while True:
            data = read_key()
            if not data:
                break
            if data == b"\x03":  # Ctrl+C
                break
            key = map_key(data)
            if key is None:
                continue
            sender.key(True, key)
            time.sleep(0.05)
            sender.key(False, key)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        if old_termios is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, old_termios)
        enable_windows_ansi()
        sys.stdout.write("\x1b[?25h\x1b[0m\x1b[2J\x1b[H")
        sys.stdout.flush()
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
