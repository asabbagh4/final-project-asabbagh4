MV Synchronome by Laye Tenumah and Abdul Rahman Alsabbagh.
Final project for Linux course at CU Boulder

[Project Overview](https://github.com/cu-ecen-aeld/final-project-asabbagh4/wiki/Project-Overview)


[Project Schedule](https://github.com/users/asabbagh4/projects/2)
## Build & Flash Instructions

### Prerequisites

- **Docker** (via [OrbStack](https://orbstack.dev/) on macOS or Docker Engine on Linux)
- **Git** with submodule support
- An SD card (8 GB+) and a card reader

### Clone the repository

```bash
git clone --recursive https://github.com/cu-ecen-aeld/final-project-asabbagh4.git
cd final-project-asabbagh4
```

### Build the Yocto image

#### macOS (Apple Silicon)

```bash
./build-mac.sh
```

This uses a Docker volume to avoid macOS filesystem limitations with Yocto. The output image is copied to `./deploy-images/` when the build completes.

#### Linux (x86 or ARM64)

```bash
./build.sh
```

The output image will be at `build/tmp/deploy/images/raspberrypi4-64/core-image-base-raspberrypi4-64.rpi-sdimg`.

### Flash to SD card

#### macOS

```bash
./flash-sd.sh
```

The script will list available disks, ask you to select the SD card, and flash the image.

#### Linux

```bash
# Find your SD card device
lsblk

# Flash (replace /dev/sdX with your SD card)
sudo dd if=build/tmp/deploy/images/raspberrypi4-64/core-image-base-raspberrypi4-64.rpi-sdimg of=/dev/sdX bs=4M status=progress
sync
```

### Boot and connect

1. Insert the SD card into the Raspberry Pi 4
2. Connect a USB UVC camera (e.g., Logitech C920)
3. Power on the Pi
4. SSH in: `ssh root@<pi-ip>` (password: `root`)

### Run the application

```bash
mkdir -p /mnt/ramdisk/frames
mount -t tmpfs -o size=512M tmpfs /mnt/ramdisk
./sequencer 1800
```