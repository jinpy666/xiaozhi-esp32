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
    "$IMAGE" bash /project/scripts/qemu/container_steps.sh \
    "$BOARD" "$VARIANT" "$QEMU_SECONDS"
