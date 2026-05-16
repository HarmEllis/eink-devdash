#!/usr/bin/env bash
# Run this on your LOCAL machine to share the ESP32 USB device over the network.
# The remote host (server) can then attach it with usbip-attach.sh.
#
# Requires: sudo apt install usbip  (or usbutils on some distros)

set -euo pipefail

echo "==> Loading usbip_host kernel module..."
sudo modprobe usbip_host

echo "==> Locally connected USB devices:"
usbip list -l

echo ""
echo "==> Looking for ESP32/serial device..."
BUS_ID=$(usbip list -l 2>/dev/null \
  | grep -i "ESP32\|CP210\|CH340\|USB Serial\|Communications\|ACM" \
  | head -1 \
  | grep -oP '(?<=- )[0-9-]+\.[0-9.]+')

if [[ -z "$BUS_ID" ]]; then
  echo "ERROR: No ESP32/serial device found. Is it plugged in?"
  echo "       Run 'usbip list -l' to see all devices, then:"
  echo "       sudo usbip bind -b <bus-id>"
  exit 1
fi

echo "==> Binding bus ${BUS_ID}..."
sudo usbip bind -b "$BUS_ID"

echo ""
echo "==> Starting usbipd (listening on port 3240)..."
echo "    Press Ctrl+C to stop sharing."
sudo usbipd
