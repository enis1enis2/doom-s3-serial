#!/usr/bin/env python3
"""Read one burst from the DOOM serial port and compare frame CRCs."""

import sys
import serial


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
    with serial.Serial(port, 3000000, timeout=2) as ser:
        data = ser.read(8192)

    print(f"bytes={len(data)}")
    if not data:
        return 0

    offset = 0
    shown = 0
    while shown < 12:
        start = data.find(b"\xa5\x5a", offset)
        if start < 0 or start + 7 > len(data):
            break
        frame_type = data[start + 2]
        length = data[start + 3] | (data[start + 4] << 8)
        end = start + 7 + length
        if end > len(data):
            print(f"type={frame_type} len={length} truncated")
            break
        payload = data[start + 5 : start + 5 + length]
        got = data[start + 5 + length] | (data[start + 6 + length] << 8)
        expected = crc16_ccitt(bytes([frame_type, length & 0xFF, length >> 8]) + payload)
        print(
            f"type={frame_type} len={length} "
            f"expected=0x{expected:04x} got=0x{got:04x} "
            f"ok={expected == got} payload={payload[:8].hex(' ')}"
        )
        offset = end
        shown += 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
