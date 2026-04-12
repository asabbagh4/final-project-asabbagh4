#!/bin/bash
# Flash Yocto image to SD card for Raspberry Pi 4
# For macOS only — uses diskutil and rdisk for fast writes
# Usage: ./flash-sd.sh [/dev/diskN]
set -e

if [[ "$(uname)" != "Darwin" ]]; then
    echo "ERROR: This script is for macOS only."
    exit 1
fi

IMAGE_DIR="./deploy-images"
IMAGE=$(ls "${IMAGE_DIR}"/*.rpi-sdimg 2>/dev/null | head -1)

if [ -z "${IMAGE}" ]; then
    echo "ERROR: No .rpi-sdimg found in ${IMAGE_DIR}/"
    echo "Run ./build-mac.sh first."
    exit 1
fi

echo "Image found: ${IMAGE}"
echo "Size: $(du -h "${IMAGE}" | cut -f1)"
echo ""

# List disks
echo "=== Available disks ==="
diskutil list external physical
echo ""

if [ -n "$1" ]; then
    DISK="$1"
else
    read -p "Enter SD card disk (e.g. /dev/disk4): " DISK
fi

if [ -z "${DISK}" ]; then
    echo "ERROR: No disk specified."
    exit 1
fi

# Safety checks
if [ "${DISK}" = "/dev/disk0" ] || [ "${DISK}" = "/dev/disk1" ]; then
    echo "ERROR: ${DISK} is likely your boot drive. Refusing to flash."
    exit 1
fi

RDISK=$(echo "${DISK}" | sed 's|/dev/disk|/dev/rdisk|')

# Confirm
echo ""
echo "WARNING: This will ERASE ALL DATA on ${DISK}"
diskutil info "${DISK}" 2>/dev/null | grep -E "Device / Media Name|Disk Size" || true
echo ""
read -p "Are you sure? Type YES to continue: " CONFIRM

if [ "${CONFIRM}" != "YES" ]; then
    echo "Aborted."
    exit 1
fi

echo ""
echo "Unmounting ${DISK}..."
diskutil unmountDisk "${DISK}"

echo "Flashing image to ${RDISK} (using raw disk for speed)..."
sudo dd if="${IMAGE}" of="${RDISK}" bs=4m status=progress

echo ""
echo "Syncing..."
sync

echo "Ejecting ${DISK}..."
diskutil eject "${DISK}"

echo ""
echo "=== Done! ==="
echo "Insert the SD card into your Raspberry Pi 4 and power on."
echo "SSH in with: ssh root@<pi-ip>  (password: root)"
