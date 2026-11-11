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

## Key input and layers

Each fitted switch is debounced for 5 ms, independently of loop speed and the
other switches. Pressing a key captures its action; releasing it releases that
same action even if a layer or mapping changes while it is held. Changing layers
does not release unrelated held keys. Unpopulated matrix positions are ignored.

In typing mode, hold Fn for the function layer. While holding Fn, hold the GUI
key to enter the existing number-pad layer. The number-pad layer remains active
until GUI is released, even if Fn is released first. Ordinary GUI presses in the
base layer still behave as GUI. In MIDI mode, Fn selects the MIDI function map;
musical actions do not also send typing reports. Mode and device changes retain
their existing restart behavior.

The host-side regression tests need only a C++11 compiler, not a connected board:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic -Isrc test/KeyInput.cpp -o /tmp/puffin-input-tests
/tmp/puffin-input-tests
```

CI also runs these tests with address and undefined-behavior sanitizers. The
debounce interval and GPIO settling delay still need confirmation on hardware.
