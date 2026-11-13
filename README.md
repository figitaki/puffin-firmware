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
Q, W, or E position to choose QWERTY, standard Colemak (not Colemak-DH), or Dvorak,
respectively. These shortcuts are position-based even after changing layouts.
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

There is no independent gate scheduler. Project storage is described below.
BLE pairing, UART wiring, actual note delivery, and the OLED route indicator need
validation on the board; host tests establish state/message behavior only.

## Four-track sequencing

Four tracks, each with up to four notes per step, run on MIDI channels 1–4, sharing the clock and
Start/Stop/Continue controls. Each has its own pattern, 1–64-step length (default
16: one page), next-step position, and mute state. A page contains 16 steps;
tracks can use one through four pages. Different lengths loop independently.
A new project starts with all four tracks empty. MIDI-mode boot restores the
last saved project when storage is available, always with playback stopped.

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
makes it 48 steps. Explicit project saves include lengths and hidden steps.
Typing-mode maps are unchanged.

### Onboard step editor

In MIDI mode, Fn+I toggles between the performance view and the step editor.
These are physical key positions, independent of the typing preset or remaps.

| Key position | Editor action |
| --- | --- |
| Left / Right | Previous / next step, wrapping within the selected track |
| Fn+O / Fn+P | Previous / next 16-step page, wrapping between pages |
| Fn+Return | Make the selected step the inclusive end of the sequence |
| Note keys | Audition and write using the selected step's velocity (95 if unset) |
| Fn+N / Fn+M | Velocity −1 / +1, clamped to 1–127 |
| Fn+comma / Fn+period | Gate −1 / +1 MIDI clock pulses, clamped to 1–6 |
| Fn+slash | Cycle global sixteenth swing: straight / 58⅓% / 66⅔% (internal clock only) |
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
retain their step's shared velocity when replaced or appended. The 16-step strip shows filled notes, rest
dots, an outlined edit cursor, and a separate tick for the **next playback
step** when it is on the current page. That tick is not a sounding-note marker.
Hollow small squares mark automation-only steps.
Battery, charging, and Bluetooth indicators remain visible.

Fn+Q/W/E/R, Fn+T, Fn+Y/U, transport, and Down (output route) retain their
existing functions. Outside the editor, note keys only record while running
and Return still clears the next playback step. Outside the editor, recording
still replaces that step with one note. Edits remain unsaved until an explicit
project save. There is no undo yet.

Velocity editing selects the `V95` detail view. Empty steps also show `REST`:
changing velocity does not add notes or send MIDI. An unset/silent velocity of
zero starts from 95 (N makes 94; M makes 96); further taps change by one. All
notes in a chord share this velocity. Replacement, chord entry and audition use
it; performance-mode recording stays at 95. Clearing the step resets it to zero.
Running edits affect the next visit, not the sounding chord or pending ratchets.
Velocity survives Save/Load, including hidden pages.

Gate editing selects `G2/6`: two of the six 24-PPQN MIDI clock pulses in a
sixteenth-note step. Gates default to 6 (full sustain on a straight step) and
range from 1 to 6. Fn+comma shortens, Fn+period lengthens. Empty steps also show
`REST`; adjusting their gate does not add notes. Replacement/chord entry retain
the gate; Return clears it back to 6. All chord notes share a gate, and running
edits apply only on the next visit. Live audition still follows key release,
not the sequenced gate.

A gate ends after its pulse count or at the next ratchet/step hit, whichever
comes first. For example, gate 1 with 2x ratchets sends on/off at pulse 0/1 and
3/4; gate 5 with 2x ratchets is cut at pulse 3 and at the next step. There are no
ties or notes held across step boundaries. Gate expiry, Stop, mute, resize and
lost-clock handling release only sequence ownership and cancel pending sequence
hits as appropriate. Route/mode changes panic all local owners on the old route.
Receiver sustain/CC state can still hold sound.

This is intentionally pulse-resolution gating, not percentage or millisecond
gating. One pulse is 125 ms at 20 BPM, about 18.25 ms at 137 BPM, and about
8.33 ms at 300 BPM. Fractional internal periods and timer wrap use the existing
clock. External gates count the actual BLE/TRS pulses and follow their jitter;
there is no tempo prediction between pulses. A lost external master releases
remaining sequence notes after the existing 500-ms timeout. Dense serial MIDI,
BLE scheduling and the main loop add latency; native tests do not measure it.

### Sixteenth swing

In the editor, Fn+slash cycles straight (`SW50`), 58⅓% (`SW58`) and 66⅔%
(`SW67`). Swing is global across the four tracks. It alternates long/short
sixteenth-note spans of 12/12, 14/10 or 16/8 internal ticks at 48 PPQN. Each pair
always totals 24 ticks (one eighth note), so tempo and eighth-note boundaries
stay unchanged. Global pair phase does not reset when an odd-length track wraps.
Changes during playback latch at the next pair boundary; Start resets the pair,
while Stop/Continue retain its phase. The OLED displays the selected setting.

Outgoing MIDI TimingClock remains straight at 24 PPQN, including on the swung
step boundary. Only the onboard sequence's notes/locks are swung, not external
instruments' own sequencers or the MIDI-thru stream. Gates retain their absolute
24-PPQN pulse lengths (two internal ticks per pulse) and truncate at the next
hit/step; gate 6 can therefore end before a long swung step finishes. Ratchet
hits divide the current swung span, rounding down to the previous 48-PPQN tick.
All 1/2/3/6 hits are retained, even in the shortest step; unequal integer spacing
is bounded to one internal tick. CC locks still send once at the first hit.
Live audition is not delayed by swing.

When following BLE/TRS clock, the saved swing setting is bypassed and displayed
as `SW67 OFF` (or the selected amount). External sequencing stays on actual
24-PPQN pulses, without guessing between packets; switch back to internal clock
to activate swing. This avoids making interpolated external timing promises.
Internal 48-PPQN ticks are about 62.5 ms at 20 BPM, 9.12 ms at 137 BPM and
4.17 ms at 300 BPM. These are grid intervals, not measured output precision.
Two pending internal ticks cause a safe transport stop rather than a catch-up
burst, so the main-loop stall budget is tighter than the old 24-PPQN scheduler.

New saves use schema v3 with global swing plus all 256 gates. V1/v2 projects
load read-only with straight swing (v1 also supplies gate 6). The first explicit
Save writes v3 to the other generation slot, retaining the legacy recovery copy;
older firmware cannot load v3 and refuses ordinary overwrites. Back up before
downgrading.
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
| Fn+Backspace | Cycle note, trigger, CC, velocity, gate, and swing detail views |

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
many chord hits within a sixteenth-note step. Internal hits use the 48-PPQN
swing grid described above; external hits use actual 24-PPQN BLE/TRS pulses,
not a guessed external tempo. 4× ratchets remain unsupported on external clock
and are not exposed by the editor. Locks send once before
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
step to sound a new chord. Explicit project saves retain these step settings.

## Project Save/Load

One project holds all four tracks, their lengths/mutes, all 64 steps per track
(including hidden pages), chords, shared velocity/gate, chance, ratchets, CC locks,
internal tempo, and global swing. Clock source/output route, held notes, transport position,
editor tools, typing maps, and Bluetooth identities are not project content.
A `*` in either MIDI view marks unsaved musical changes; returning to the saved
content clears it. No autosave or per-note flash writes occur.

In MIDI mode, hold Fn and use these physical positions, in either MIDI view:

| Position | Action |
| --- | --- |
| Z | Save the current project |
| X | Load the last saved project; asks before discarding unsaved changes |
| C | Open project-storage initialization (erases **all files in the data partition**) |
| V | Confirm the displayed question |
| B | Cancel the question/notice |

Save, Load, and initialization require stopped playback. They release local
live notes, retain output/clock-source selection, and never restart playback.
After an operation, press the board's Start or Continue to re-arm transport,
including when following an external clock. Buffered external transport is
ignored until then; incoming MIDI cannot unexpectedly resume after storage.
Unrelated MIDI actions cancel a pending confirmation. Release and press held
performance keys again after an operation. MIDI-thru continuity during flash
writes is not guaranteed. Load resets track selection, edit cursors and
playheads to track 1/step 1 and restores tempo; it does not reset external CC
receiver state (including sustain). Boot restores music only when starting MIDI
mode, always stopped; there is no demo note in a blank project.

**First use:** a fully erased data partition, or a mounted filesystem with no
saved project, boots directly into an empty pattern without an error notice.
The whole partition must read as erased (0xFF); nonblank mount failures, read
errors, corrupt saves and unsupported schemas still report their real errors.
A healthy saved project also restores silently. Application-only OTA updates do
not erase project storage. If storage is unformatted, explicit Save shows
`STORAGE NOT SETUP / FN+C STORAGE SETUP`; no data is formatted automatically.
Only if you intend to initialize/erase that partition,
stop playback, press Fn+C, read `ERASE STORAGE? / ALL PROJECTS LOST`, then Fn+V.
After `STORAGE READY`, press Fn+Z to save your current music. Fn+B cancels. If a
previous firmware used the partition, back up its contents first. This operation
does not erase typing settings, Bluetooth bonds, or firmware OTA slots.

Two independently checked files retain the latest and previous changed saves.
A damaged latest snapshot falls back to a valid previous save with `SAVE RECOVERED`.
If neither is valid, Save asks before replacing damaged files. A recognized
unsupported schema shows `NEW FW REQUIRED` and blocks both ordinary and confirmed
Save; it is never silently overwritten. Initialization is a separate destructive
action, not a recovery shortcut. Failed writes show `SAVE FAILED`, retain the
unsaved marker, and leave the previous committed snapshot untouched.

The v3 format, v1/v2 migration, storage policy, limitations, and physical power-loss
acceptance checklist are in [Project persistence](docs/project-persistence.md).
Software checks do **not** establish real-board flash durability or uninterrupted
MIDI delivery. Physical save/reboot/power-cut and updater-preservation tests remain
required before relying on this for performances.

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
Project saves retain internal tempo and patterns, but not clock source or output route.

Internal clock defaults to 120 BPM, supports 20–300 BPM, and emits 24 pulses per
quarter note. Straight steps advance every six wire clocks; internal swing
changes sequence step spans without changing wire clock spacing. Start/Continue
send transport before resuming internal sequence ticks; external transport waits
for the selected input's next clock. Start resets step/pair position. Continue
preserves pair/half-clock phase and the next-step index, with an immediate
internal sequence tick; a wire clock is emitted only on an even internal tick. Stop releases the sequence without cutting off a
shared live-note owner.

The clock accumulates integer microsecond fractions, preserving long-run tempo
and tempo-change phase without millisecond rounding or integer division by zero.
Unsigned timestamp differences handle normal `micros()` wrap. Two pending
48-PPQN internal ticks are treated as a stall: stop/release instead of replaying a
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

## Pattern synchronization (experimental)

BLE/TRS MIDI SysEx now supports a revision-checked first-note/velocity pattern
projection: four tracks, all 64 stored slots, 16-step pages and arbitrary set-end
lengths. Staged writes commit only while stopped and preserve existing chords,
chance, ratchets, gates and CC locks. They edit RAM, not flash. This is a
single-companion adapter, not full-pattern backup or network peer sync.
See [the wire contract and interoperability harness](docs/pattern-sysex.md)
for capabilities, retry limits, projection semantics and hardware-test gaps.

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
