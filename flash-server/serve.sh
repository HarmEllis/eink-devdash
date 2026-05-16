#!/usr/bin/env bash
# Run inside the devcontainer after `idf.py build`.
# Copies the built binaries to flash-server/bins/ and starts a local HTTP server on port 8080.
# VS Code forwards port 8080 → open http://localhost:8080 in Chrome on your local machine.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../firmware/build"
BINS_DIR="${SCRIPT_DIR}/bins"

if [[ ! -f "${BUILD_DIR}/eink-devdash.bin" ]]; then
  echo "ERROR: firmware not built yet. Run: cd firmware && idf.py build"
  exit 1
fi

echo "==> Copying binaries..."
mkdir -p "$BINS_DIR"
cp "${BUILD_DIR}/bootloader/bootloader.bin"  "${BINS_DIR}/"
cp "${BUILD_DIR}/partition_table/partition-table.bin" "${BINS_DIR}/"
cp "${BUILD_DIR}/eink-devdash.bin"           "${BINS_DIR}/"

echo "==> Serving flash-server/ on http://localhost:8080"
echo "    Open in Chrome on your local machine and click Install."
echo ""
cd "$SCRIPT_DIR"
python3 -m http.server 8080
