#!/usr/bin/env bash
# In-container steps for the QEMU boot smoke test. Invoked by qemu_smoke.sh with
# the repo mounted at /project. Kept as a separate file to avoid nested quoting
# inside `docker run ... bash -c`.
# Usage: container_steps.sh <board-dir> <variant-name> <qemu-seconds>
set -euo pipefail

BOARD="$1"
VARIANT="$2"
QEMU_SECONDS="$3"

source /opt/esp/idf/export.sh >/dev/null 2>&1

echo "[qemu] building $BOARD / $VARIANT ..."
python3 scripts/build.py "$BOARD" --name "$VARIANT" 2>&1 | tail -5
test -f build/merged-binary.bin

# QEMU models the flash chip from the drive size; a smaller chip than
# CONFIG_ESPTOOLPY_FLASHSIZE trips an init assert. The textual sdkconfig lives
# at the project root (not build/); try the value line, then the selected
# choice line, then fall back to 4.
FLASH_MB=$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE="\([0-9]*\)MB"$/\1/p' sdkconfig 2>/dev/null | head -1)
[ -z "$FLASH_MB" ] && FLASH_MB=$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE_\([0-9]*\)MB=y$/\1/p' sdkconfig 2>/dev/null | head -1)
FLASH_MB="${FLASH_MB:-4}"
echo "[qemu] padding flash image to ${FLASH_MB}M (CONFIG_ESPTOOLPY_FLASHSIZE)"
cp build/merged-binary.bin build/qemu-flash.bin
truncate -s "${FLASH_MB}M" build/qemu-flash.bin

echo "[qemu] booting headless for ${QEMU_SECONDS}s -> build/qemu.log"
timeout "$QEMU_SECONDS" qemu-system-riscv32 \
    -M esp32c3 \
    -drive file=build/qemu-flash.bin,if=mtd,format=raw \
    -serial file:/project/build/qemu.log \
    -display none -monitor none -no-reboot || true

test -s build/qemu.log || { echo "[qemu] empty serial log"; exit 2; }

echo "[qemu] asserting milestones ..."
python3 scripts/hil/hil_smoke.py --from-file build/qemu.log --json
