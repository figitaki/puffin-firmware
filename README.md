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

## Sequencer note safety (foundation)

The single-track model stores musical note numbers 0–127 and per-step velocities,
not key-function codes. Velocity zero is a rest. It retains the prototype's
96-step capacity and releases the explicitly tracked sounding note at the next
step, including at wraparound and after edits. Independent gate timing is not
implemented yet.

Start releases the old sequencer note and arms step zero for the first clock;
Stop releases it and retains the next position and pulse phase; Continue resumes
from there. Incoming transport is accepted only from the selected clock input.
Live playing and the sequence share note ownership on channel 1 of the selected
output. The first owner supplies velocity; overlapping owners do not retrigger,
and only the last release sends a physical note-off.

Switching BLE/TRS output stops playback and releases all locally owned notes on
the old route before selecting the new one. The first output toggle now selects
TRS, matching its indicator. Still-held performance keys must be released and
pressed again on the new route. Mode/device restarts attempt the same cleanup
and flush buffered output first. Delivery to a disconnected receiver cannot be
guaranteed. Forwarded MIDI-thru notes are not locally owned.

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -Isrc test/Sequencer.cpp -o /tmp/puffin-sequencer-tests
/tmp/puffin-sequencer-tests
```

This is a single-track transport foundation, not the finished sequencer editor.
The chord action remains a prototype. There is no independent gate scheduler,
multi-track engine, or project storage.
BLE pairing, UART wiring, actual note delivery, and the OLED route indicator need
validation on the board; host tests establish state/message behavior only.

## Standalone transport and clock

In MIDI mode, the six fitted top-row switches, left to right, now control:

| Matrix position (zero-based) | Typing-mode key | MIDI action |
| --- | --- | --- |
| Row 0, column 3 | Esc | Start from step zero |
| Row 0, column 4 | Screenshot | Stop |
| Row 0, column 5 | 3 | Continue |
| Row 0, column 6 | 4 | Internal tempo −1 BPM |
| Row 0, column 7 | 5 | Internal tempo +1 BPM |
| Row 0, column 8 | 6 | Cycle Internal → BLE → TRS clock input |

Typing-mode bindings are unchanged. Fn+Esc retains the mode-switch/restart
binding. The down-arrow position (row 4, column 10) still selects note output
BLE/TRS independently of clock input. Source changes stop playback and reset
pulse phase; Continue can resume the retained next step, or Start resets to zero.
These settings/patterns are not persisted yet.

Internal clock defaults to 120 BPM, supports 20–300 BPM, and emits 24 pulses per
quarter note. Each sixteenth note advances every six clocks. Start/Continue send
their MIDI transport message before the first internal clock; external transport
waits for the selected input's next clock. Start resets step/pulse position;
Continue preserves pulse phase and the next-step index, with the next clock due
immediately in internal mode. Stop releases the sequence without cutting off a
shared live-note owner.

The clock accumulates integer microsecond fractions, preserving long-run tempo
and tempo-change phase without millisecond rounding or integer division by zero.
Unsigned timestamp differences handle normal `micros()` wrap. Two pending
internal clocks are treated as a stall: stop/release instead of replaying a
backlog. A playing external sequence also stops after 500 ms without a selected
clock, including a late arrival after that gap. Restart/Continue is then required.
These are fail-safe policies, not guarantees that BLE or the main loop can meet
all deadlines on physical hardware.

The upper-right clock indicator shows `INT`, `BLE`, or `TRS`, separately from the
lowercase output-route label. External BPM is the last valid 24-interval arrival
measurement (20–300 BPM), or `--` before one is available; BLE packet batching can
affect it. Rendering no longer changes clock state. Channel/SysEx bridge traffic
remains bidirectional, but only the selected external input forwards TimingClock,
Start, Continue, and Stop to the other interface. Internal mode filters both
incoming transport streams, avoiding an additional forwarded clock master.
External transport is not echoed back to its input. Local controls send transport
to the selected output even in external-clock mode; they do not generate clocks
there. Avoid external software/hardware MIDI feedback loops.

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -Isrc test/Clock.cpp -o /tmp/puffin-clock-tests
/tmp/puffin-clock-tests
```

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
