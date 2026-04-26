#!/bin/bash
# Build the Yocto image and validate the resulting artifacts.
# Runnable locally and from the CI workflow.
set -e

cd "$(dirname "$0")"
test_dir="$(pwd)"

logfile=test.sh.log
exec > >(tee -i -a "$logfile") 2> >(tee -i -a "$logfile" >&2)

echo "Running test as user $(whoami)"

# --- Build ---
echo "=== Building image ==="
./build.sh

# --- Validate image artifacts ---
echo "=== Validating image artifacts ==="
ARTIFACTS_DIR="${test_dir}/build/tmp/deploy/images/raspberrypi4-64"

if [ ! -d "${ARTIFACTS_DIR}" ]; then
    echo "ERROR: artifacts dir ${ARTIFACTS_DIR} missing"
    exit 1
fi
ls -lh "${ARTIFACTS_DIR}"

# 1. Image exists and is non-trivial in size (>100M)
IMAGE=$(ls "${ARTIFACTS_DIR}"/*.rpi-sdimg 2>/dev/null | head -n1)
if [ -z "${IMAGE}" ]; then
    echo "ERROR: no .rpi-sdimg produced"
    exit 1
fi
IMAGE_SIZE=$(stat -c '%s' "${IMAGE}")
if [ "${IMAGE_SIZE}" -lt 104857600 ]; then
    echo "ERROR: image ${IMAGE} is ${IMAGE_SIZE} bytes, expected >100MB"
    exit 1
fi
echo "OK: image ${IMAGE} is ${IMAGE_SIZE} bytes"

# 2. & 3. Manifest lists synchronome and openssh-sshd
MANIFEST=$(ls "${ARTIFACTS_DIR}"/*.manifest 2>/dev/null | head -n1)
if [ -z "${MANIFEST}" ]; then
    echo "ERROR: no image manifest produced"
    exit 1
fi
for pkg in synchronome openssh-sshd; do
    if ! grep -q "^${pkg} " "${MANIFEST}"; then
        echo "ERROR: package '${pkg}' not in image manifest"
        echo "--- manifest contents ---"
        cat "${MANIFEST}"
        exit 1
    fi
    echo "OK: ${pkg} present in manifest"
done

echo "=== All validations passed ==="
exit 0
