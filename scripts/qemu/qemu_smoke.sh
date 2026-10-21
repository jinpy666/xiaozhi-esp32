#!/usr/bin/env bash
# QEMU boot smoke test in Docker: build a lite-engine (ESP32-C3) variant with the
# canonical build.py entry point inside the espressif/idf container, boot it in
# headless Espressif QEMU, then assert serial-log milestones via hil_smoke.py.
# See docs/architecture/13-firmware-testing-guide.md §4.
#
# Usage:  bash scripts/qemu/qemu_smoke.sh
# Env:    BOARD (default kevin/c3), VARIANT (default kevin-c3), IMAGE, QEMU_SECONDS
set -euo pipefail

BOARD="${BOARD:-kevin/c3}"
VARIANT="${VARIANT:-kevin-c3}"
IMAGE="${IMAGE:-xiaozhi-qemu:release-v6.1}"
QEMU_SECONDS="${QEMU_SECONDS:-25}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    docker build -q -t "$IMAGE" -f "$REPO_ROOT/scripts/qemu/Dockerfile" "$REPO_ROOT/scripts/qemu"

exec docker run --rm \
    -v "$REPO_ROOT":/project -w /project \
    "$IMAGE" bash -c '
        set -e
        source /opt/esp/idf/export.sh >/dev/null 2>&1
        echo "[qemu] building '"$BOARD"' / '"$VARIANT"' ..."
        python3 scripts/build.py "'"$BOARD"'" --name "'"$VARIANT"'" 2>&1 | tail -5
        test -f build/merged-binary.bin
        # QEMU models the flash chip from the drive size; a smaller chip than
        # CONFIG_ESPTOOLPY_FLASHSIZE trips an init assert. The textual sdkconfig
        # lives at the project root (not build/); try the value line, then the
        # selected choice line, then fall back to 4.
        FLASH_MB=$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE="\([0-9]*\)MB"$/\1/p' sdkconfig 2>/dev/null | head -1)
        [ -z "$FLASH_MB" ] && FLASH_MB=$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE_\([0-9]*\)MB=y$/\1/p' sdkconfig 2>/dev/null | head -1)
        FLASH_MB="${FLASH_MB:-4}"
        echo "[qemu] padding flash image to ${FLASH_MB}M (CONFIG_ESPTOOLPY_FLASHSIZE)"
        cp build/merged-binary.bin build/qemu-flash.bin
        truncate -s "${FLASH_MB}M" build/qemu-flash.bin
        echo "[qemu] booting headless for '"$QEMU_SECONDS"'s -> build/qemu.log"
        timeout '"$QEMU_SECONDS"' qemu-system-riscv32 \
            -M esp32c3 \
            -drive file=build/qemu-flash.bin,if=mtd,format=raw \
            -serial file:/project/build/qemu.log \
            -display none -monitor none -no-reboot || true
        test -s build/qemu.log || { echo "[qemu] empty serial log"; exit 2; }
        echo "[qemu] asserting milestones ..."
        python3 scripts/hil/hil_smoke.py --from-file build/qemu.log --json
    '
