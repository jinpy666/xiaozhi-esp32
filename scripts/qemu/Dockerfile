# QEMU smoke test image: espressif/idf base + Espressif QEMU (esp32c3 via qemu-riscv32).
# Built once locally by scripts/qemu/qemu_smoke.sh, reused afterwards.
FROM espressif/idf:release-v6.1

# IDF >= 5.2 ships QEMU through idf_tools; respects IDF_TOOLS_PATH set by the image.
RUN python /opt/esp/idf/tools/idf_tools.py install qemu-riscv32
