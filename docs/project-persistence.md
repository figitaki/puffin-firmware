# Project persistence v4 (v1/v2/v3-compatible reader)

One local project contains one four-track pattern. Saves include every track's
active length/mute and all 64 steps, including steps hidden by shorter lengths.
Each step retains ordered notes, shared velocity, chance, ratchets, gate, tie, and
ordered MIDI CC locks. A tie is a continuation holding the preceding note/chord;
the step's own musical fields remain stored and validated even while dormant.
Internal tempo and global sixteenth swing belong to the project.
Runtime transport, editor cursors/tools, clock source/output route, receiver CC
state, typing configuration, and Bluetooth identities do not.

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
| 4 | 2 | Schema version 4 (reader also accepts 1, 2 and 3) |
| 6 | 2 | Header size 32 |
| 8 | 4 | Payload size 4,687 (v3: 4,655; v2: 4,654; v1: 4,398) |
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
- Each 18-byte step: four note slots, note count, velocity, chance, ratchet,
  four interleaved CC/value pairs, lock count, gate. Each field is uint8.
  Gate is byte offset 17 within a step; offsets 0–16 are unchanged from v1.
- Gate is 1–6 clock pulses at 24 PPQN, truncated by the next ratchet hit or
  step boundary. Gate 6 preserves the old sustain behavior with straight swing;
  on a long swung step it can end before the next boundary. It is musical data,
  including on silent, automation-only and hidden steps; no milliseconds,
  timestamps, countdowns, elapsed time or other runtime fields are persisted.
- After all four tracks: one uint8 global swing byte at payload offset 4,654
  (record offset 4,686). Values are 0 straight (default), 1 for 58 1/3%, and 2
  for 66 2/3% sixteenth swing. All v2 track/step offsets remain unchanged.
  Swing participates in validation, musical equality and dirty detection.
  `capture(seq, tempo, swing = 0)` stores it; `apply` still replaces tracks only.
  The caller restores tempo and clock swing separately after validation.
- After swing: a 32-byte tie bitmap at payload offset 4,655 (record offsets
  4,687–4,718 inclusive). Tracks are ordered 1–4, eight bytes per track;
  within each track steps 0–63 are LSB-first. For zero-based track `t` and step
  `s`, bit `s % 8` of record byte `4687 + t * 8 + s / 8` is its tie bool.
  Every bit is meaningful; every possible bitmap is valid, including all-zero
  (default) and all-one. No raw C++ bool representation is serialized or tested
  as an invalid value. All 256 ties participate in equality and dirty detection,
  even on silent/automation-only steps or beyond the active length. All v3
  payload offsets are unchanged; only the schema/size/CRC header fields differ.

Unused note/lock slots are zero on write and ignored on read/comparison. Order of
used notes/locks is preserved because it affects serial MIDI output order.
Note 0, velocity/CC value 0, silent notes, automation-only steps, 0% chance, and
hidden steps are valid; none are treated as missing data.

One v4 record is exactly 4,719 bytes; two v4 files occupy 9,438 bytes before
filesystem overhead. The decoder rejects bad counts, duplicate pitches/CCs,
unsupported ratchets, invalid IDs, nonzero flags/reserved, wrong lengths, checksum
errors, truncation, and trailing bytes. Musical limits match the current engine:
length 1–64; BPM 20–300; notes/velocity/values 0–127; chance 0–100; ratchets
1/2/3/6; gate 1–6; at most four notes and four unique CC0–119 locks per step.
All 256 steps' dormant fields are validated, including gates, even when tied or
beyond active track lengths, as is swing 0–2. Ties themselves are bools and
cannot carry out-of-range values.
Validation finishes before publishing either the snapshot or its generation. The decoder
uses two passes and only a small Step temporary; large buffers belong to the
store, not the task stack.

Version 1 has exactly 4,430 bytes, a 4,398-byte payload and 17-byte steps.
Its reader supplies gate 6 for every step, preserving all original identities,
music and note/lock order. Version 2 has exactly 4,686 bytes, a 4,654-byte payload
and 18-byte steps including gate. These two readers supply swing 0, even when
decoding over an existing nonzero-swing snapshot. Version 3 has exactly 4,687
bytes and a 4,655-byte payload, adding swing after the tracks. All three legacy
readers supply tie=false for every step, even when decoding over an all-tied
snapshot. All four versions require their
exact header/payload sizes and CRC; truncation and trailing bytes are rejected,
not guessed at.

## Storage and recovery

`ProjectStore` uses `/project-a.pfn` and `/project-b.pfn`, independently validated
with the newest valid generation winning. No separate active-file pointer is
needed. Save writes only the older or invalid slot, checks all writes, `fsync`
and `close`, then rereads and validates it before clearing the dirty state.
Unchanged clean v4 saves skip writes. Repairing a damaged second slot does write a
new generation even if the recovered music itself is unchanged. The previous
valid generation remains untouched.

`LittleFSProjectFiles` mounts the current `spiffs`-labelled data partition as
LittleFS at `/littlefs`. No partition-table changes are introduced. The generated
ProS3 table has a 3,456-KiB data partition, alongside two OTA application slots;
NVS remains separate. Ordinary application updates must not upload a filesystem
image or erase flash. An installed device's partition map and prior filesystem
contents still need checking before first initialization.

Mount never formats automatically. On mount failure, the adapter checks every
byte of the `spiffs` partition. Only an entirely erased (0xFF), successfully read
partition is treated as empty/uninitialized: boot shows the normal blank pattern,
and explicit Save shows `STORAGE NOT SETUP` with Fn+C setup instructions. A mounted
filesystem with no saves and a healthy restored project also boot silently.
Nonblank mount failures (including old filesystems), partition/read failures,
corrupt records, unsupported schemas and conflicting generations retain their
errors; OTA does not excuse or erase them. Fn+C opens a separate destructive
initialization prompt; Fn+V erases that data partition and mounts it, while Fn+B
cancels. This erases all its files, not just these two snapshots, but not bonds,
typing settings, or application slots. Initialization retains working music in
RAM and marks it unsaved. The next Save persists it.

A corrupt newest record recovers the previous complete record and reports
`SAVE RECOVERED`. If neither record is usable, the working music is left intact;
ordinary Save asks before replacing damaged files. A recognizable unsupported
version blocks ordinary/confirmed Save and automatic restore, even if the other
slot contains a valid old version. Equal-generation records with differing
content also block Load/Save. With identical content at equal generations,
the newest schema is preferred (v4 over v3 over v2 over v1); otherwise generation takes
precedence over schema.
Explicit destructive initialization remains possible only after its own warning.

Load and boot restore never rewrite files. Loading v1/v2/v3 migrates only in memory
and establishes a clean musical baseline. The first deliberate Save still writes
v4 at generation + 1 into the other slot, even without musical edits. The legacy
source remains untouched through durable write and verified readback (and until
a later save needs its slot). Write/sync/readback failure does not advance the
baseline. On reboot, a complete valid newer v4 wins; an incomplete/corrupt v4
falls back to the retained v1/v2/v3. Subsequent unchanged v4 saves skip writes, unless
the other slot needs repair. Recognizable future schemas still block all normal
load/save, including confirmed replacement, regardless of the other slot.

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
interruption at every byte boundary, including v1/v2/v3-to-v4 migration with unchanged
content, hidden gate edits, swing-only edits and hidden tie-only edits. Independent
v1/v2/v3 legacy fixtures cover asymmetric hidden music and ordered content; v4 byte
layout and CRC have fixed Python struct/zlib references with both zero and
nonuniform tie bitmaps. Tests cover all 256 tie positions, every bitmap byte
value, dormant tied fields, and hidden tie dirty/revert behavior. Mixed-version tests
cover generation selection, equal-generation ties, read-only loading and the
first unchanged migration Save. These tests model storage faults; they do
not prove the physical flash/filesystem power-loss behavior.

Before performance use, check on a real ProS3:

- Save distinctive four-track music, power-cycle in MIDI mode, and verify notes,
  lengths/mutes, tempo, swing, chords, ratchets, gates, ties and CC ordering at a receiver.
- Save with hidden notes/ties beyond step 19, reboot, expand and confirm they return.
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
cloud dependency, or multiplayer journal in v4.
