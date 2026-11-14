# Puffin Firmware

This is the firmware for the Puffin Keys Mechanical Keyboard and MIDI Controller written in C++ along side Arduino library.

## Build

Use PlatformIO Core 6.1.18 with Python 3.11. From this directory:

```sh
python -m pip install platformio==6.1.18
pio run --environment ProS3
```

`ProS3` is the target for the Puffin PCB. The `esp32dev` environment is retained
for the earlier development board; it is not a substitute for ProS3 validation.

The platform is pinned to Espressif32 6.10.0 (Arduino-ESP32 2.0.17), and direct
library dependencies are pinned in `platformio.ini`. Control Surface uses an
immutable commit rather than its moving `main` branch. Upgrade these versions
deliberately and verify the complete build before changing the pins.

GitHub Actions builds the ProS3 target on pushes and pull requests. A successful
build checks compilation and linking, not electrical behavior, BLE pairing,
matrix scanning, or MIDI timing; those still need testing on the actual board.
