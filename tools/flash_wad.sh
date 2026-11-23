#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-${ESPPORT:-/dev/ttyACM0}}"

cd "$ROOT"
export PATH="$ROOT/host-bin:$PATH"
export IDF_PATH="$ROOT/esp-idf"
export IDF_PATH_FORCE=1
export IDF_TOOLS_PATH="$ROOT/esp-idf-tools"
export IDF_PYTHON_ENV_PATH="$ROOT/idf-python-env"
. "$ROOT/esp-idf/export.sh"

echo "Flashing $ROOT/doom1.wad to 0x190000 on $PORT"
"$ROOT/idf-python-env/bin/python" -m esptool --chip esp32s3 --port "$PORT" write_flash 0x190000 "$ROOT/doom1.wad"
