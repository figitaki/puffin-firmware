# Pattern synchronization: experimental PF SysEx v1

This adapter exposes a **first-note/velocity projection**, not a full-pattern backup. It retains the current paged model: four tracks, 64 stored slots per track, 16-step pages, active length 1–64. Shortening hides slots; it never deletes them. Firmware remains authoritative for chords, chance, ratchets, gates, CC locks, tempo and swing. This is a single-companion revision/CAS bridge, not a network CRDT protocol.

## Wire format

Complete frame: `F0 7D 50 46 01 device command requestLo requestHi payload... checksum F7`. Every interior byte is 7-bit. `7D` is experimental/noncommercial; obtain an appropriate manufacturer ID before commercial distribution. Checksum is the negative sum, modulo 128, from `7D` through the final payload byte. Request IDs and other integers are little-endian base-128; revisions are uint32 carried in five bytes (fifth byte ≤15). MIDI stack removes interleaved realtime messages before delivery.

Address is fixed at **0**. Address127 accepts HELLO discovery only and replies from0. Other addresses are ignored. Complete malformed addressed frames return ERROR when enough header exists to correlate the request. Unrelated SysEx retains the BLE/TRS bridge; PF frames and their continuations are consumed locally. Incomplete/oversized streamed messages are not applied. Requests fit within the existing 128-byte parser buffer; largest valid request47 bytes. Largest response147 bytes is sent via the MIDI sender (BLE packet fragmentation is handled by Control Surface). Physical OS/BLE reassembly still requires acceptance testing.

| Command | Payload | Response command / payload |
| --- | --- | --- |
| 01 HELLO | none | 41: profile1, tracks4, maxPages4, stepsPerPage16, caps11, nonce[8], revision[5] — 18 bytes |
| 02 READ_TRACK | track | 42: track, revision[5], length, muted, 64 note/velocity pairs — 136 bytes |
| 03 BEGIN_WRITE | nonce[8], expectedRevision[5], track, length, muted | 43: token[2] |
| 04 WRITE_CHUNK | token[2], offset0–63, count1–16, note/velocity pairs | 44: token[2], offset, count |
| 05 COMMIT_WRITE | token[2] | 45: newRevision[5] |
| 06 ABORT_WRITE | token[2] | 46: token[2] |
| 07 READ_STATUS | none | 47: revision[5], source, running, positions[4], tempo[2] — **13 bytes** |
| 7F ERROR | response only | originalCommand, code, currentRevision[5] |

Boolean fields are0/1. Capabilities: staged writes1, conflict checks2, optional notifications4 (**not implemented**), stopped-only commits8. Source: internal0/BLE1/TRS2. Positions are each track's **next** step0–63 while running and127 when stopped, not last-sounded steps. Tempo is internal preferred integer BPM20–300, not estimated external tempo. READ_STATUS does not contain swing. No remote tempo, swing, route or transport writes.

Errors: malformed1, unsupported2, busy3, conflict4, range5, expired6, bootChanged7. Unknown version/command is unsupported. Unsupported fields never silently select another profile.

## Projection preserves richer steps

READ_TRACK returns `count ? notes[0] : 0` and `count ? velocity : 0` for every slot, including hidden slots. Empty slots are canonically projected as (0,0), regardless of unused backing pitches. A zero-note rest may retain a silent entry-velocity preset. The projection intentionally does not expose that preset, ordered tail, expression or locks.

BEGIN copies the **full** existing track into staging. Each incoming pair that is identical to the staged projection leaves that entire Step unchanged, including a rest's velocity preset. A changed pair replaces only `notes[0]` and velocity, retaining all tail notes, chance, ratchet, gate, continuation ties and CC locks. A continuation's stored note/velocity remains dormant while the tie is on; profile1 cannot represent, create or clear ties. A changed pair on a previously empty step creates one note even at velocity0, retaining an explicitly edited silent pitch through revision tracking and Save/Load. An unchanged rest pair still preserves its entry-velocity preset without creating a note. On ordinary note steps, velocity0 silences but retains any existing chord/count. A pitch duplicating a retained tail pitch returns Range; the complete chunk is rejected atomically. Clients must explain these projection limits rather than claim full chord editing or backup.

COMMIT validates the revision again and is **stopped-only**, checked against both clock and sequencer transport. Busy does not stop playback. Success applies one track atomically in the main loop, without audition, seek or touching another track, live-note ownership, selection, route or clock phase. Its next position resets only if length changed; an out-of-range edit cursor clamps to the new last step. Writes affect RAM and mark the project dirty; they do **not** save to flash. Physical Save remains explicit.

## Revisions, retries and ownership

Every boot gets a random56-bit nonce. Revision starts0 after boot restoration and advances on observed musical content changes (all tracks/hidden expression, identities, internal tempo, swing), not navigation or playback. Successful COMMIT always advances, even for unchanged content. Revision never wraps; reaching uint32 max blocks writes until reboot. Full semantic snapshots, not persistence generations, determine changes. Revisions are boot-scoped, not durable actor counters.

One bounded stage is shared by the device and owned by the ingress BLE or TRS interface. Another ingress cannot edit/commit/abort it. A fresh BEGIN while staged returns Busy. Stage expires after10 seconds idle, even across millis wrap. Exact retries refresh activity only for successful BEGIN/chunk replies whose token matches the live stage; cached errors and replies from older stages do not extend its deadline. Retries never resurrect expired staging. Tokens are14-bit, never reused within a boot; exhausting16383 tokens blocks new stages pending reboot.

There are32 cached replies shared across inputs, keyed by ingress and request ID. Identical requests return the original response for30 seconds from execution; no mutation or COMMIT reapplication occurs. Same ID/different bytes returns malformed. Unexpired entries are **never evicted**: full cache returns uncached Busy before executing. Ordinary command errors, including playback Busy, are cached. After the condition changes use a new request ID for a deliberate new attempt. Retries do not extend cache lifetime. Cache IDs are not durable client identities: gateways should randomize their initial14-bit ID and avoid reuse within30 seconds, including after reconnect. Same-ingress apps must coordinate externally (e.g. Web Lock); BLE/TRS ownership is not authentication.

Send one request at a time. Default timeout1 second; at most three read/chunk retries, with backoff for cache pressure. Never retry uncertain COMMIT after30 seconds: reread state/revision and resolve before another transaction. Restart invalidates staging/cache. To obtain a coherent four-track snapshot, read HELLO, all tracks, then HELLO again, accepting only the same boot and revision throughout. A local edit between BEGIN/COMMIT produces Conflict; re-read and three-way merge against acknowledged baseline. Do not blindly overwrite.

For a future network journal, the companion imports resolved device differences as gateway-actor transactions and supplies revision CAS when exporting. Exactly one coordinating gateway is supported; snapshots plus boot revisions cannot infer causal physical operations across multiple gateways. Persistent actors/stamped deltas remain a future negotiated protocol.

## Host interoperability harness and checks

`src/PatternSync.h` is platform-independent. `test/PatternSync.cpp` contains protocol fixtures and assertions. Build with strict C++11, ASan/UBSan as in CI. Supplying any command-line argument runs a stdin/stdout hex harness: one full request per line, one full response per line. It starts a default empty project, nonce `01 02 03 04 05 06 07 08`, revision0, stopped internal120BPM, BLE ingress, time advances1ms per request. State persists across input lines.

Independent HELLO fixture:

- Request: `f0 7d 50 46 01 00 01 01 00 6a f7`
- Response: `f0 7d 50 46 01 00 41 01 00 01 04 04 10 0b 01 02 03 04 05 06 07 08 00 00 00 00 00 62 f7`

Tests cover hidden rich-state preservation, silent presets/chords, atomic malformed/duplicate-pitch rejection, revision conflicts from local musical edits, retry idempotency, source ownership, stage/cache expiration and rollover, capacity pressure, stopped-only commits, and default wire sizes/checksums. MultiTrackSequencer tests cover stopped replacement, length reset versus position retention, cursor clamping, hidden slots and live-note ownership. These software checks do not prove physical BLE/TRS timing, packet delivery/reassembly or journal integration. Read requests and SysEx transmission still consume main-loop time; measure playback timing under sync load before release.
