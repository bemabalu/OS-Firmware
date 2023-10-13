#!/bin/python3

import esptool

# fmt: off
# Note: Bootloader for esp32-s3 starts at 0x0000, unlike several other ESP32 variants that start at 0x1000.
esptool.main([
    '--chip', 'esp32s3',
    'merge_bin', '-o', 'merged.bin',
    '--flash_size', '8MB',
    '0x0', './bootloader.bin',
    '0x8000', './partitions.bin',
    '0x10000', './firmware.bin',
    '0x280000', './littlefs.bin'
])
# fmt: on