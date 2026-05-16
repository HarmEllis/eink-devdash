#!/usr/bin/env bash
# Starts the HTTP server and watches for new builds.
# When firmware/build/eink-devdash.bin changes, bins/ is updated automatically.
# Run once inside the devcontainer; kill with Ctrl+C.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../firmware/build"
BINS_DIR="${SCRIPT_DIR}/bins"
BIN="${BUILD_DIR}/eink-devdash.bin"

copy_bins() {
    mkdir -p "$BINS_DIR"
    cp "${BUILD_DIR}/bootloader/bootloader.bin"       "${BINS_DIR}/"
    cp "${BUILD_DIR}/partition_table/partition-table.bin" "${BINS_DIR}/"
    cp "${BIN}"                                        "${BINS_DIR}/"
    echo "[watch] bins updated — $(date '+%H:%M:%S')"
}

if [[ ! -f "$BIN" ]]; then
    echo "ERROR: firmware not built yet. Run: cd firmware && idf.py build"
    exit 1
fi

copy_bins

echo "[watch] HTTP server on http://localhost:8080 — watching for new builds..."
python3 -m http.server 8080 --directory "$SCRIPT_DIR" &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null" EXIT

LAST_MOD=$(stat -c '%Y' "$BIN")
while true; do
    sleep 2
    CURRENT_MOD=$(stat -c '%Y' "$BIN" 2>/dev/null || echo "$LAST_MOD")
    if [[ "$CURRENT_MOD" != "$LAST_MOD" ]]; then
        LAST_MOD="$CURRENT_MOD"
        copy_bins
    fi
done
