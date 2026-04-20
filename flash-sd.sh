#!/bin/bash
# Flash Yocto image to SD card for Raspberry Pi 4
# For Linux — uses lsblk and dd
# Usage: ./flash-sd.sh [/dev/sdX]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ "${EUID}" -ne 0 ]]; then
    exec sudo "$SCRIPT_DIR/$(basename "$0")" "$@"
fi

cd "${SCRIPT_DIR}"

if [[ "$(uname)" != "Linux" ]]; then
    echo "ERROR: This script is for Linux only. Use flash-sd-mac.sh on macOS."
    exit 1
fi

IMAGE_DIR="./build/tmp/deploy/images/raspberrypi4-64"
IMAGE=$(readlink -f "${IMAGE_DIR}/core-image-base-raspberrypi4-64.rpi-sdimg" 2>/dev/null)

if [ -z "${IMAGE}" ] || [ ! -f "${IMAGE}" ]; then
    echo "ERROR: No .rpi-sdimg found in ${IMAGE_DIR}/"
    echo "Run ./build.sh first."
    exit 1
fi

echo "Image found: ${IMAGE}"
echo "Size: $(du -h "${IMAGE}" | cut -f1)"
echo ""

# List removable block devices
echo "=== Available removable disks ==="
lsblk -d -o NAME,SIZE,MODEL,TRAN,RM | grep -E "NAME|1$" || echo "(none found)"
echo ""

if [ -n "$1" ]; then
    DISK="$1"
else
    read -p "Enter SD card device (e.g. /dev/sdb): " DISK
fi

if [ -z "${DISK}" ]; then
    echo "ERROR: No device specified."
    exit 1
fi

# Resolve to base device (strip partition numbers)
DISK=$(echo "${DISK}" | sed 's/[0-9]*$//')

# Safety checks — refuse to write to common system disks
BASENAME=$(basename "${DISK}")
if [[ "${BASENAME}" == "sda" || "${BASENAME}" == "nvme0n1" || "${BASENAME}" == "vda" || "${BASENAME}" == "mmcblk0" ]]; then
    echo "ERROR: ${DISK} looks like a system drive. Refusing to flash."
    exit 1
fi

# Verify it's a removable device
REMOVABLE=$(cat "/sys/block/${BASENAME}/removable" 2>/dev/null || echo "unknown")
if [[ "${REMOVABLE}" != "1" ]]; then
    echo "WARNING: ${DISK} is not marked as removable (removable=${REMOVABLE})."
    read -p "Continue anyway? Type YES to proceed: " FORCE
    if [ "${FORCE}" != "YES" ]; then
        echo "Aborted."
        exit 1
    fi
fi

# Confirm
echo ""
echo "WARNING: This will ERASE ALL DATA on ${DISK}"
lsblk "${DISK}" 2>/dev/null || true
echo ""
read -p "Are you sure? Type YES to continue: " CONFIRM

if [ "${CONFIRM}" != "YES" ]; then
    echo "Aborted."
    exit 1
fi

# Unmount all partitions on the device
echo ""
echo "Unmounting any mounted partitions on ${DISK}..."
for part in "${DISK}"*; do
    if mountpoint -q "$(findmnt -n -o TARGET "${part}" 2>/dev/null)" 2>/dev/null; then
        umount "${part}" 2>/dev/null || true
    fi
done
# Also try umount by device directly
umount "${DISK}"* 2>/dev/null || true

echo "Flashing image to ${DISK}..."
dd if="${IMAGE}" of="${DISK}" bs=4M status=progress conv=fsync

echo ""
echo "Syncing..."
sync

# Re-read partition table and wait for udev to re-enumerate the device
blockdev --rereadpt "${DISK}" 2>/dev/null || true
udevadm settle --timeout=10

echo ""
echo "=== Done! ==="
echo "Insert the SD card into your Raspberry Pi 4 and power on."
echo "SSH in with: ssh root@<pi-ip>"

# Verification step: check partitions and try mounting rootfs
echo ""
echo "Verifying SD card..."
lsblk -o NAME,SIZE,TYPE,MOUNTPOINT,LABEL,MODEL "${DISK}"

# Find rootfs partition (second partition, usually)
# mmcblk devices use pN suffix (e.g. mmcblk1p2), others use plain N (e.g. sdb2)
if [[ "${BASENAME}" == mmcblk* ]]; then
    ROOTFS_PART="${DISK}p2"
else
    ROOTFS_PART="${DISK}2"
fi
MNT_DIR="/tmp/sdroot-verify-$$"
mkdir -p "$MNT_DIR"
if mount -o ro "$ROOTFS_PART" "$MNT_DIR" 2>/dev/null; then
    if [ -d "$MNT_DIR/etc/ssh" ]; then
        echo "Rootfs mount OK. SSH config found."
        ls -l "$MNT_DIR/etc/ssh"
    else
        echo "Rootfs mount OK, but /etc/ssh not found!"
    fi
    umount "$MNT_DIR"
else
    echo "WARNING: Could not mount rootfs partition ($ROOTFS_PART). The image may be corrupted or the card is faulty."
fi
rmdir "$MNT_DIR"
