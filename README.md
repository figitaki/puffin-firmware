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
When several switches hold the same action, only the first press and last release
are dispatched. A full scan is collected before resolving transitions: releases
first, then layer-key presses, then HID modifier presses, then ordinary presses.
Fn+GUI therefore selects the number-pad layer even when both keys become
debounced in the same scan, and same-scan Shift/Ctrl/GUI chords send the modifier
before the letter.

In typing mode, hold Fn for the function layer. While holding Fn, hold the GUI
key to enter the existing number-pad layer. The number-pad layer remains active
until GUI is released, even if Fn is released first. Ordinary GUI presses in the
base layer still behave as GUI. In MIDI mode, Fn selects the MIDI function map;
musical actions do not also send typing reports. Mode and device changes retain
their existing restart behavior.
The screenshot shortcut temporarily releases the `5` usage if `5` or `%` is
already held, so the host sees a fresh shortcut key-down. It then restores the
physical report without changing any held-key ownership. Unrelated keys and
modifiers stay held throughout. If all six report slots are occupied by other
keys (or the keyboard is in rollover), the shortcut is skipped rather than
evicting another key.
Typing reports track owners of translated HID usages and modifier bits, so
distinct actions such as `9` and `(` can share a key, and releasing a shifted
character does not clear a still-held physical Shift. Translation reuses the
exported ASCII table from the pinned BLE Keyboard library. The existing BLE
report has six ordinary key slots; more than six distinct usages send standard
ErrorRollOver until enough keys release, without discarding held state.

The host-side regression tests need only a C++11 compiler, not a connected board:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic -Isrc test/KeyInput.cpp -o /tmp/puffin-input-tests
/tmp/puffin-input-tests
g++ -std=c++11 -Wall -Wextra -Werror -pedantic -Isrc test/KeyboardOutput.cpp -o /tmp/puffin-output-tests
/tmp/puffin-output-tests
```

CI also runs these tests with address and undefined-behavior sanitizers. The
debounce interval and GPIO settling delay still need confirmation on hardware.

## Display scheduling

The loop sends the 128 x 32 OLED framebuffer incrementally: one address command,
then sixteen 32-byte data chunks, with input/MIDI polling between transactions.
Each transaction uses 400 kHz I2C and restores the library's default 100 kHz bus
clock afterward. Rendering waits until the preceding frame finishes, so bytes
from two frames cannot be mixed. A failed transaction abandons the frame; the
next refresh starts again with the address command. Drawing and pixel layout
are unchanged. Boot-only display helpers still use the library's blocking API.

This removes the loop's full-frame I2C stall, not every possible scan delay:
bus timeouts, BLE calls, and other loop work can still block. Measure worst-case
scan intervals and short-press capture on ProS3 hardware before claiming a
maximum input latency or reliable capture of every press longer than 5 ms.

CI checks complete framebuffer byte order, one-transaction updates, and bus-error
recovery with a simulated bus. Run its native test with:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic -Isrc test/DisplayTransfer.cpp -o /tmp/puffin-display-tests
/tmp/puffin-display-tests
```
