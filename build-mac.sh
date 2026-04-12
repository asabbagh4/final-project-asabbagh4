#!/bin/bash
# Build script for macOS Apple Silicon (M-series) using Docker volumes
# Avoids macOS bind mount issues with hard links that break Yocto builds
set -e

VOLUME_NAME="yocto-rpi4-build"
CONTAINER_NAME="yocto-builder"
PROJECT_DIR="/home/builder/project"
IMAGE_OUTPUT_DIR="./deploy-images"

echo "=== Yocto Build for macOS Apple Silicon ==="
echo ""

# Check Docker is running
if ! docker info >/dev/null 2>&1; then
    echo "ERROR: Docker is not running. Start OrbStack or Docker Desktop first."
    exit 1
fi

# Create persistent volume (survives container removal, keeps sstate-cache)
docker volume create "${VOLUME_NAME}" >/dev/null 2>&1 || true
echo "[1/5] Docker volume '${VOLUME_NAME}' ready"

# Remove old container if it exists
docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true

# Sync source code into the volume
# Uses a temp container to copy files, excludes build artifacts
echo "[2/5] Syncing source code into Docker volume..."
docker run --rm \
    -v "$(pwd)":/src:ro \
    -v "${VOLUME_NAME}":/dest \
    ubuntu:22.04 bash -c "
        # Install rsync for efficient syncing
        apt-get update -qq && apt-get install -y -qq rsync > /dev/null 2>&1
        # Sync source, exclude build artifacts and .git objects to save time
        rsync -a --delete \
            --exclude='build/tmp' \
            --exclude='build/sstate-cache' \
            --exclude='build/downloads' \
            --exclude='build/cache' \
            /src/ /dest/project/
        echo 'Source sync complete'
    "

echo "[3/5] Starting build container..."

# Run the build in a persistent container
docker run -it \
    --name "${CONTAINER_NAME}" \
    -v "${VOLUME_NAME}":/home/builder \
    ubuntu:22.04 bash -c "
        set -e
        echo '[4/5] Installing build dependencies...'
        export DEBIAN_FRONTEND=noninteractive
        ln -fs /usr/share/zoneinfo/UTC /etc/localtime
        apt-get update -qq
        apt-get install -y -o Dpkg::Options::='--force-confdef' -o Dpkg::Options::='--force-confold' \
            gawk wget git diffstat unzip texinfo gcc build-essential \
            chrpath socat cpio python3 python3-pip python3-pexpect \
            xz-utils debianutils iputils-ping python3-git python3-jinja2 \
            python3-subunit zstd liblz4-tool file locales libacl1-dev lz4 \
            rsync

        locale-gen en_US.UTF-8
        export LC_ALL=en_US.UTF-8

        # Create non-root build user (Yocto refuses to build as root)
        useradd -m -d /home/builder builder 2>/dev/null || true
        chown -R builder:builder /home/builder/project

        echo '[5/5] Starting Yocto build (this will take a while)...'
        echo ''
        su - builder -c 'cd ${PROJECT_DIR} && ./build.sh'
    "

# Copy the output image back to macOS
echo ""
echo "=== Build complete! Extracting image... ==="
mkdir -p "${IMAGE_OUTPUT_DIR}"
docker run --rm \
    -v "${VOLUME_NAME}":/src:ro \
    -v "$(pwd)/${IMAGE_OUTPUT_DIR}":/out \
    ubuntu:22.04 bash -c "
        if ls /src/project/build/tmp/deploy/images/raspberrypi4-64/*.rpi-sdimg 1>/dev/null 2>&1; then
            cp /src/project/build/tmp/deploy/images/raspberrypi4-64/*.rpi-sdimg /out/
            echo 'Image copied to ${IMAGE_OUTPUT_DIR}/'
            ls -lh /out/*.rpi-sdimg
        else
            echo 'ERROR: No .rpi-sdimg found. Check build logs above.'
            exit 1
        fi
    "

echo ""
echo "=== Done! ==="
echo "Flash to SD card with:"
echo "  diskutil list                  # find your SD card"
echo "  diskutil unmountDisk /dev/diskN"
echo "  sudo dd if=${IMAGE_OUTPUT_DIR}/core-image-base-raspberrypi4-64.rpi-sdimg of=/dev/rdiskN bs=4m status=progress"
echo "  diskutil eject /dev/diskN"
