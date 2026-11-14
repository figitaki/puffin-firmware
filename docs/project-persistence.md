# Project persistence v1

One local project contains one four-track pattern. Saves include every track's
active length/mute and all 64 steps, including steps hidden by shorter lengths.
Each step retains ordered notes, shared velocity, chance, ratchets, and ordered
MIDI CC locks. Internal tempo belongs to the project. Runtime transport, editor
cursors/tools, clock source/output route, receiver CC state, typing configuration,
and Bluetooth identities do not.

## Identity and format

A new working project receives a UUIDv4; normal saves and loads preserve it.
Pattern ID is 1, and track IDs are 1–4, currently mapping to MIDI channels 1–4.
The portable model/codec in `src/ProjectSnapshot.h` has no Arduino/filesystem
or transport dependencies. `Sequencer::State` exposes all stored steps without
changing the active length. Whole-project replacement validates everything
before changing any track and resets selection, playheads and edit cursors.

Fields are explicitly encoded, never dumped as C++ object memory. All integers
are little-endian; the 16 UUID bytes retain their standard byte order.

| Header offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `PFNP` |
| 4 | 2 | Schema version 1 |
| 6 | 2 | Header size 32 |
| 8 | 4 | Payload size 4,398 |
| 12 | 8 | Nonzero local save generation |
| 20 | 4 | Flags, zero |
| 24 | 4 | CRC-32/ISO-HDLC |
| 28 | 4 | Reserved, zero |

CRC covers the entire record with bytes 24–27 treated as zero, including the
generation. Reflected polynomial is `0xEDB88320`, initial/final XOR values
`0xFFFFFFFF`. CRC is a corruption check, not a signature or security boundary.

Payload order:

- Project ID: 16 bytes; pattern ID: uint32; internal BPM: uint16.
- Four track records, ordered by ID: uint32 ID, uint8 length, uint8 mute,
  then 64 step records.
- Each 17-byte step: four note slots, note count, velocity, chance, ratchet,
  four interleaved CC/value pairs, lock count. Each field is uint8.

Unused note/lock slots are zero on write and ignored on read/comparison. Order of
used notes/locks is preserved because it affects serial MIDI output order.
Note 0, velocity/CC value 0, silent notes, automation-only steps, 0% chance, and
hidden steps are valid; none are treated as missing data.

One record is exactly 4,430 bytes; the two files occupy 8,860 bytes before
filesystem overhead. The decoder rejects bad counts, duplicate pitches/CCs,
unsupported ratchets, invalid IDs, nonzero flags/reserved, wrong lengths, checksum
errors, truncation, and trailing bytes. Musical limits match the current engine:
length 1–64; BPM 20–300; notes/velocity/values 0–127; chance 0–100; ratchets
1/2/3/6; at most four notes and four unique CC0–119 locks per step.

## Storage and recovery

`ProjectStore` uses `/project-a.pfn` and `/project-b.pfn`, independently validated
with the newest valid generation winning. No separate active-file pointer is
needed. Save writes only the older or invalid slot, checks all writes, `fsync`
and `close`, then rereads and validates it before clearing the dirty state.
Unchanged clean saves skip writes. Repairing a damaged second slot does write a
new generation even if the recovered music itself is unchanged. The previous
valid generation remains untouched.

`LittleFSProjectFiles` mounts the current `spiffs`-labelled data partition as
LittleFS at `/littlefs`. No partition-table changes are introduced. The generated
ProS3 table has a 3,456-KiB data partition, alongside two OTA application slots;
NVS remains separate. Ordinary application updates must not upload a filesystem
image or erase flash. An installed device's partition map and prior filesystem
contents still need checking before first initialization.

Mount never formats automatically. A failed mount may mean corruption or a
previous filesystem, not an empty partition. Fn+C opens a separate destructive
initialization prompt; Fn+V erases that data partition and mounts it, while Fn+B
cancels. This erases all its files, not just these two snapshots, but not bonds,
typing settings, or application slots. Initialization retains working music in
RAM and marks it unsaved. The next Save persists it.

A corrupt newest record recovers the previous complete record and reports
`SAVE RECOVERED`. If neither record is usable, the working music is left intact;
ordinary Save asks before replacing damaged files. A recognizable unsupported
version blocks ordinary/confirmed Save and automatic restore, even if the other
slot contains a valid old version. Equal-generation records with differing
content also block Load/Save. No migration from other versions is implemented.
Explicit destructive initialization remains possible only after its own warning.

Local generations are not multiplayer revisions. Newer firmware must retain old
files during any future schema migration until an explicit successful Save.
A companion's full audio/plugin document must remain separate from this bounded
MIDI snapshot; unsupported content must not be silently discarded.

## Controls and execution

Physical MIDI Fn bindings are Z Save, X Load, C Initialize, V Confirm, B Cancel.
These occupy previously unused MIDI function-layer positions and do not change
typing maps. Save, Load, and initialization refuse while either clock/sequencer
is running. Dirty Load asks before discarding edits; unrelated MIDI actions
cancel pending confirmations. A `*` in either normal MIDI view denotes musical
content differing from the saved baseline. Navigation/playback does not dirty it.
Status cards replace the small OLED's normal content for five seconds; questions
remain visible until answered/cancelled. Confirmations use the full OLED without
battery/Bluetooth overlays.

Operations execute after the key-event batch, release all local MIDI ownership,
and require a fresh key press for held performance notes. A storage guard drops
incoming transport during I/O and keeps external transport paused afterward
until the board's local Start/Continue is pressed. Control Surface polls only a
bounded input batch, so a single drain cannot exclude old buffered Start messages;
the explicit local re-arm prevents later batches from restarting playback. No uninterrupted MIDI-thru guarantee is made during flash I/O.
Load replaces only fully validated content, retains route/source selection,
resets clock phase/selection/cursors, and leaves playback stopped. It does not
restore external receiver CC state, including sustain.

MIDI-mode boot restores the latest compatible snapshot read-only, stopped. A
new project is empty, four tracks of 16 steps at 120 BPM, with no prototype demo
note. Storage errors preserve files and leave an unsaved blank working project.
Typing and maintenance boots do not initialize or format project storage.

## Validation and remaining hardware gates

Native snapshot/store tests exercise asymmetric track lengths, hidden content,
expression/automation playback after restore, canonical comparison, malformed
records, unknown versions, ambiguous generations, failed writes/readback, and
interruption at every byte boundary. These tests model storage faults; they do
not prove the physical flash/filesystem power-loss behavior.

Before performance use, check on a real ProS3:

- Save distinctive four-track music, power-cycle in MIDI mode, and verify notes,
  lengths/mutes, tempo, chords, ratchets and CC ordering at a receiver.
- Save with hidden notes beyond step 19, reboot, expand and confirm they return.
- Confirm Save/Load refuse during internal and external playback. Check held-note
  cleanup, discard/cancel prompts, and that completion never resumes the clock.
- Cut power repeatedly during actual writes and verify recovery produces a whole
  old or new project, never a mixture. Check full-storage and mount failures.
- Verify an application-only BLE update preserves both snapshots; retain USB
  recovery and do not flash filesystem images. Check safe newer-schema rejection.
- Verify typing presets/remaps and Bluetooth pairing survive initialization and
  normal save/load. Check OLED legibility and key positions on the physical board.

Two generations cannot protect against arbitrary flash-device failure or complete
filesystem loss. There is no export UI, multi-project bank, autosave, song mode,
cloud dependency, or multiplayer journal in v1.
