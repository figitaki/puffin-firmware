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

## Development Bluetooth firmware updater

The native [iPhone/iPad updater](ios/PuffinUpdater/README.md) provides a consumer
flow for official HTTPS updates and transfers them over BLE. Its release-server
URL still needs configuring; local `firmware.bin` import is in the options menu. Install
updater-capable firmware **once by USB** first; the app cannot add this service
to older firmware or provision an OTA partition table. Keep USB recovery available.

Hold physical **Fn + Up during reset/power-on** to enter exclusive maintenance
mode. Its OLED shows a fresh six-digit PIN; keyboard and MIDI services do not
start. Reset without those keys to exit. New sessions are accepted for five
minutes, five incorrect PINs lock the session until reset, and an active transfer
aborts after 30 seconds without progress. Interrupted transfers restart from zero.

The inactive OTA slot is selected for boot only after SHA-256 and ESP image
validation. This is a **development prototype**, not a secure release system:
PIN + SHA-256 do not authenticate a publisher, and firmware signatures, secure
boot, and automatic post-boot rollback are not implemented. A valid but broken
application may still require USB recovery. Transfer completion does not confirm
that the new firmware booted. See the [wire protocol](docs/ble-update-protocol.md)
and app README for setup, limits, and the physical-device test checklist.

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

## Alternative typing layouts

QWERTY is the default. In typing mode, hold Fn, then GUI, and press the physical
Q, W, or E position to choose QWERTY, Colemak-DH, or Dvorak, respectively.
The Colemak-DH preset matches the base layer of the `figitaki` Corne keymap
(`keyboards/crkbd/keymaps/figitaki` in figitaki/qmk_firmware), without its
home-row mod-taps. These shortcuts are position-based even after changing layouts.
They replace the redundant left-side 1/2/3 bindings in the number-pad layer;
the right-side number pad and the normal Fn number row remain available.

The preset changes only the base layer's 30 letter/punctuation positions.
Modifiers, space, arrows, Tab/Return/Backspace, top-row shortcuts, and the Fn
symbol layer stay in place. The up-arrow position interrupts Puffin's bottom
typing row; Dvorak's Z uses the far-right slash position, not the arrow. Dvorak's
slash remains accessible at Fn+up-arrow. Held keys retain their original
press-time actions across a layout change; new presses use the new layout.
MIDI maps and controls are not changed.

Selection is stored with ESP32 Preferences in the separate `puffin-keys` NVS
namespace under `layout`, without modifying the existing mode/device EEPROM.
Only a changed selection is written. Missing, invalid, or unreadable stored
selection defaults to QWERTY. A failed save keeps the new layout for the current
session and reports the failure over serial. No restart or re-pair is required
to change the layout; physical reboot persistence still needs board validation.

These presets emit US-layout HID positions through the existing ASCII mapping.
Use a US/QWERTY input source on the host, not a second Colemak/Dvorak remap.
There is no desktop configuration tool yet.

## Persistent per-key overrides

On ProS3, connect the native USB port and open a serial terminal at 115200 baud
while in typing mode. Send the following ASCII commands with LF or CRLF endings;
coordinates are zero-based matrix row/column, not key labels. Only base-layer
keys are remapped. The Fn switch (row 4, column 8) is protected, so preset
selection and mode/device controls remain reachable through the unchanged Fn
layers even if the base GUI key is remapped.

| Command | Meaning |
| --- | --- |
| `keymap get ROW COL` | Read the current base-layer action |
| `keymap set ROW COL ACTION` | Set a position override |
| `keymap reset ROW COL` | Remove one override, inheriting the selected preset |
| `keymap clear` | Remove every override without changing the preset |

For example, `keymap set 1 1 97` makes the physical Q position send lowercase A.
`keymap reset 1 1` returns it to the selected preset. Actions are decimal:
0 disables a switch; 32–126 are printable US ASCII; 128–135 are BLE Keyboard
modifier actions; 136–255 use that library's non-printing HID encoding
(136 + HID usage). For example, 128 is left Ctrl, 129 is left Shift, 178 is
Backspace, and 179 is Tab. Internal commands, macros, and MIDI actions cannot be
assigned. Nonexistent switches and invalid actions/coordinates are rejected.

Responses start with `keymap`: `saved`, `unchanged`, `value N`,
`session-only: save failed`, or `error: ...`. Existing debug logs can appear
between replies. Duplicate sets and resets do not write flash. A save failure
keeps the change in RAM; `unchanged` does not retry a previously failed save.
Send at most 63 printable characters per command. Malformed, overlong, or
control-character-containing lines are discarded through the next line ending,
not executed as truncated commands.

Overrides follow physical positions across preset changes and take precedence
over the preset. Held keys retain their captured actions until release. Changed
overrides are stored as one versioned `remap` blob in `puffin-keys`, separately
from the preset ID and existing EEPROM. At boot, wrong-size, unknown-version,
or invalid records are ignored in full; no partial map is applied. Version 1 is
121 bytes: a version byte (1), then 60 row-major little-endian uint16 actions,
where 0xFFFF means inherit. Board/matrix-format changes require a format update.

The console reads at most 16 bytes per loop and only when native USB CDC is
enabled and typing mode is active. It never reads the Serial0 MIDI UART; MIDI
mode does not process configuration commands or write remap flash. Flash writes
and serial replies can still pause typing briefly, so configure while idle.
Physical USB interaction, power-cycle persistence and BLE delivery still need
board validation. No Fn-layer remapping, BLE configuration service, or MIDI-map
editing is implemented.

## Sequencer note safety (foundation)

The single-track model stores up to four musical note numbers 0–127 per step,
not key-function codes. Velocity zero is a rest. Each track has a four-page,
64-step capacity and releases the explicitly tracked sounding chord at the next
step, including at wraparound and after edits. Independent gate timing is not
implemented yet.

Start releases the old sequencer note and arms step zero for the first clock;
Stop releases it and retains the next position and pulse phase; Continue resumes
from there. Incoming transport is accepted only from the selected clock input.
Live playing and each sequence share note ownership on their track's channel of
the selected output. The first owner supplies velocity; overlapping owners do not retrigger,
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

There is no independent gate scheduler or project storage.
BLE pairing, UART wiring, actual note delivery, and the OLED route indicator need
validation on the board; host tests establish state/message behavior only.

## Four-track sequencing

Four tracks, each with up to four notes per step, run on MIDI channels 1–4, sharing the clock and
Start/Stop/Continue controls. Each has its own pattern, 1–64-step length (default
16: one page), next-step position, and mute state. A page contains 16 steps;
tracks can use one through four pages. Different lengths loop independently.
Only track 1 contains the prototype's initial C note; the others start empty.

In MIDI mode, hold Fn and use these typing-key positions:

| Key position | Action |
| --- | --- |
| Q / W / E / R | Select track 1 / 2 / 3 / 4 |
| T | Toggle selected track mute |
| Y / U | Remove / add one 16-step page on the selected track (1–4 pages) |

The OLED shows `T1` through `T4`, with `M` appended when the selected track is
muted. Live notes use the selected track's channel; releasing a key after a
track change still releases its original channel. Recording while running and
the existing clear action affect only the selected track's next step. Mute
immediately releases that track's sequence-owned note, not a shared live owner,
and keeps advancing its position. Unmute takes effect at the next step.

Changing length stops the shared transport and resets only the edited track's
next-step position to zero. Other tracks retain their positions; Continue
resumes from those positions. Attempts beyond 1–4 pages do nothing, including leaving
playback running. Shortening hides, rather than erases, steps beyond the new
length. Adding/removing a page sets a full-page length: 16, 32, 48, or 64 steps.
For example, a 19-step pattern has two pages; Fn+Y makes it 16 steps and Fn+U
makes it 48 steps. No settings or patterns survive restart yet. Typing-mode maps
are unchanged.

### Onboard step editor

In MIDI mode, Fn+I toggles between the performance view and the step editor.
These are physical key positions, independent of the typing preset or remaps.

| Key position | Editor action |
| --- | --- |
| Left / Right | Previous / next step, wrapping within the selected track |
| Fn+O / Fn+P | Previous / next 16-step page, wrapping between pages |
| Fn+Return | Make the selected step the inclusive end of the sequence |
| Note keys | Audition and write the selected step at velocity 95 |
| Return | Clear the selected step, including its chord, chance, ratchets, and locks |

Each track remembers its own edit position. The cursor does not advance when
entering notes or during playback; edits work while stopped or running without
moving the playback position. Page changes preserve the offset within the page,
clamping to the last step on a partial final page. For example, in a 17-step
pattern, paging forward from step 16 selects step 17; paging forward again
selects step 1. Shortening a pattern clamps its edit cursor if necessary.

To make a 19-step sequence, expand to two pages with Fn+U, enter the editor with
Fn+I, use Fn+P and the arrows to select step 19, then press Fn+Return. Step 19
will play before the sequence wraps to step 1. This stops playback just like
other length changes; pressing Fn+Return on the existing end is a no-op.
The shortcut does nothing outside the editor. To extend again, add a page.

The OLED shows track/mute, current page / total pages, and `>` for running or
`-` for stopped. The second line shows selected step / sequence length, the
selected note, chord size (such as `3N`), or `REST`, BPM, and clock source. Notes
are still entered at the fixed velocity of 95. The 16-step strip shows filled notes, rest
dots, an outlined edit cursor, and a separate tick for the **next playback
step** when it is on the current page. That tick is not a sounding-note marker.
Hollow small squares mark automation-only steps.
Battery, charging, and Bluetooth indicators remain visible.

Fn+Q/W/E/R, Fn+T, Fn+Y/U, transport, and Down (output route) retain their
existing functions. Outside the editor, note keys only record while running
and Return still clears the next playback step. Outside the editor, recording
still replaces that step with one note. Editing is volatile: there is no
velocity/gate editor, undo, or pattern persistence yet.
Host tests and software OLED renders do not replace checking the physical
controls, display, and received MIDI on a board.

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -Isrc test/MultiTrackSequencer.cpp -o /tmp/puffin-multitrack-tests
/tmp/puffin-multitrack-tests
```

### Chance, ratchets, chords, and parameter locks

Enter the editor with Fn+I and select a step with the arrows. These physical
key positions affect only the selected track/step, and do nothing outside the
editor (except MIDI Ctrl, which is also a chord-entry toggle inside the editor).

| Key position | Action |
| --- | --- |
| Fn+A / Fn+S | Chance −10% / +10%, clamped to 0–100% |
| Fn+D / Fn+F | Previous / next ratchet count: 1, 2, 3, 6 |
| Fn+G (or MIDI Ctrl) | Toggle chord append mode |
| Fn+H / Fn+J | Select MIDI CC number −1 / +1 (0–119; starts at 74) |
| Fn+K / Fn+L | Set/adjust that CC lock value −1 / +1 (0–127) |
| Fn+semicolon | Remove the selected CC lock from this step |
| Fn+Backspace | Cycle note, trigger, and CC detail views |

Chord append mode shows `+` before the transport indicator. Tap up to four
different note keys to build a chord on the selected step, without advancing
the cursor; they need not be held together. Duplicate pitches are ignored.
Disable append mode to replace a chord with a single note, retaining its chance,
ratchets, and locks. Return clears the entire step to defaults. A fifth note or
fifth distinct CC lock is rejected with `FULL`; existing data stays intact.
Note keys still audition even when the sequence is muted, chance is zero, or
the chord is full. Chord-entry mode and the CC selector are global editor tools,
not per-track musical settings.

Chance defaults to 100% and is rolled once per step visit, for the entire chord
and its locks. 0% skips all of them, without keeping the preceding chord held.
The hardware uses ESP32 randomness. Ratchets default to 1; 2, 3 and 6 mean that
many evenly spaced chord hits within a sixteenth-note step. They use the actual
24-PPQN pulses from internal, BLE, or TRS clock, not a guessed external tempo.
4× ratchets need finer timing and are not supported yet. Locks send once before
the first hit, not on each ratchet. Overlapping live notes retain ownership, so
their pitches do not retrigger or release when a ratchet fires.
Dense chords/ratchets across four tracks can exceed the 31.25-kbaud TRS MIDI
bandwidth at high tempos; maximum density and physical timing need board testing.

Up to four CC locks can be stored per step, including a step with no notes.
Selecting a CC does not create a lock. Adjusting an unset lock starts from 64
(K makes 63, L makes 65); further taps change it by one. Controls 120–127 are
excluded because they are MIDI channel-mode commands. There is no parameter
name discovery: map the CC to a plugin parameter in AUM/the receiving instrument.
Locks send on the track channel and selected BLE/TRS route. Removing a lock,
clearing a step, muting, or stopping does not restore the receiver's earlier CC
value; it holds until another message changes it. This includes pedal-like CCs
such as sustain, which may hold receiver voices even after note-offs.

Trigger details show `C100 R2 N3` (chance, ratchets, note count). CC details show
`74=65 L1` (selected CC, value, number of locks); `---` means that CC is unlocked.
Editing these settings automatically selects their detail view. Running edits
apply on the next visit: the sounding chord and its pending ratchets are a
snapshot. Stop, mute, resize, route changes, and lost-clock handling release the
sequence's notes and cancel remaining ratchets. Continue waits until the next
step to sound a new chord. None of these patterns/settings survive reboot yet.

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
