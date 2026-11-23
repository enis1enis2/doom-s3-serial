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

echo "Flashing firmware to $PORT"
idf.py -p "$PORT" flash
echo "Flashing DOOM WAD to $PORT"
"$ROOT/tools/flash_wad.sh" "$PORT"
