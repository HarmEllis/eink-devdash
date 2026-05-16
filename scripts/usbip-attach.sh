#!/usr/bin/env bash
# Run this on the REMOTE host (server) to attach the ESP32 USB device forwarded from local machine.
# Usage: ./scripts/usbip-attach.sh <local-machine-ip>
#
# On your LOCAL machine first run:
#   sudo modprobe usbip_host
#   sudo usbipd  (or: sudo usbip bind -b $(usbip list -l | grep ESP | awk '{print $1}'))

set -euo pipefail

LOCAL_IP="${1:?Usage: $0 <local-machine-ip>}"
DEVICE_KEYWORD="ESP32\|CP210\|CH340\|USB Serial"

echo "==> Loading kernel modules on remote host..."
sudo modprobe vhci-hcd

echo "==> Available USB devices on ${LOCAL_IP}:"
usbip list -r "$LOCAL_IP"

echo ""
echo "==> Looking for ESP32/serial device..."
BUS_ID=$(usbip list -r "$LOCAL_IP" 2>/dev/null \
  | grep -i "ESP32\|CP210\|CH340\|USB Serial\|ACM\|ttyACM" \
  | head -1 \
  | awk '{print $1}' \
  | tr -d ':')

if [[ -z "$BUS_ID" ]]; then
  echo "ERROR: No ESP32/serial device found on ${LOCAL_IP}."
  echo "       Check that the ESP32 is plugged in and usbipd is running on the local machine."
  echo "       Manual: usbip attach -r ${LOCAL_IP} -b <bus-id>"
  exit 1
fi

echo "==> Attaching bus ${BUS_ID} from ${LOCAL_IP}..."
sudo usbip attach -r "$LOCAL_IP" -b "$BUS_ID"

sleep 1
echo "==> Result:"
ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "No ttyACM/ttyUSB found yet — check dmesg"
