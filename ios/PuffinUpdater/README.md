# Puffin Updater

A consumer-oriented SwiftUI iPhone/iPad interface for official wireless updates,
using the [development BLE protocol](../../docs/ble-update-protocol.md). Requires
iOS/iPadOS 16+, Xcode 15+ and a **physical** Bluetooth LE-capable iPhone or iPad
to transfer. The primary flow checks the official service, shows release notes,
downloads and validates the application, connects to Puffin, and asks for its
six-digit code before updating. Local file installation is in the options menu.
No server is deployed or distribution signing identity included.

## Official update service

Set the build's `INFOPLIST_KEY_PuffinReleaseManifestURL` to the publisher-controlled
HTTPS manifest URL in Xcode settings or your private release configuration, then
regenerate the project if configuring it through XcodeGen. No production endpoint
has been supplied, so the unconfigured app fails closed instead of downloading
from a guessed host. Users do not enter a server URL, token, certificate or hash.

The manifest is JSON with `version` (display label), `notes` (array of release-note
strings), `target` (`ProS3`), `url` (HTTPS application binary), `size` (exact byte
count), and `sha256` (64 lowercase hexadecimal digits). The download must match
both size and digest before Bluetooth discovery begins. iOS URLSession/ATS validates
server certificates through the system trust store; there are no certificate
bypasses, custom trust delegates or HTTP exceptions.

This assumes a trusted publisher service and its certificate chain. **HTTPS
certificates are not firmware signatures.** Board-enforced signed-image checking,
secure boot and rollback remain unimplemented. The consumer interface does not
claim those protections. The current board protocol also does not report the
installed firmware version or confirm its next boot, so the app offers the
published release without claiming “already up to date” or confirmed installation.

## Prerequisite: install the updater by USB

The board must already run updater-capable Puffin firmware with an OTA partition layout. Install that once by USB using the firmware repository’s instructions. This app cannot bootstrap BLE updating onto old firmware or provision a partition table. Keep USB recovery available.

Hold **Fn + Up during reset/power-on** to enter the exclusive maintenance mode. The OLED displays a fresh six-digit PIN. Keyboard/MIDI services do not run in this mode. The window expires after five minutes; reset into maintenance again if needed. Reset without the keys to leave maintenance mode.

## Generate, sign and run locally

On your Mac, install Xcode and [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`). From this directory:

```sh
xcodegen generate
open PuffinUpdater.xcodeproj
```

Select the **PuffinUpdater** target → Signing & Capabilities. Choose your own development Team and, if needed, a unique bundle identifier. No Team or private credentials are stored here. Connect and trust your physical iPhone/iPad, enable Developer Mode when prompted, select it as the run destination and Run. Allow Bluetooth permission. Generated projects and local signing changes need not be committed.

For local development/recovery only, copy a trusted **ProS3 application
`firmware.bin`** to Files and use the options menu → Install local firmware.
Do not choose a merged whole-flash image, bootloader or partition table. Local
files bypass the official manifest, not the board's digest/image checks. Connect
Puffin, enter its code, and tap Update Puffin. Remain foregrounded, nearby and
powered throughout. There is no automatic PIN retry; five bad attempts lock the
board until reset.

## Safety and scope

- **Development security only.** The PIN, physical maintenance window and SHA-256 are safeguards and integrity checks, not publisher authentication. There are no firmware signatures, secure boot or automatic rollback. Use only firmware from a source you trust. A valid application can still fail to boot and require USB recovery.
- Import reads at most 8 MiB + 1 byte with security-scoped access. It checks `.bin`, size, ESP magic, segment count, SPI mode, ESP32-S3 chip ID, segment bounds and presence of checksum/hash bytes, then hashes the bytes with CryptoKit. These are plausibility checks, not full ESP image validation or cryptographic authenticity. The board performs final digest/image validation. The **8 MiB client bound is not the OTA slot capacity**; Begin can reject a smaller board-side limit.
- Scans use the exact updater service UUID, not a trusted device name. The app subscribes to Status and requests its current value before enabling PIN authentication. It accepts only Ready as the initial state; other states/errors require a fresh session/reset rather than guessing recovery.
- Control writes always use ATT response, one pending control at a time. After authentication's Authorized status **and** ATT acknowledgment, a Data characteristic advertising `.writeWithoutResponse` selects streaming: send control `[6]`, wait for its ATT acknowledgment only, then send digest halves and Begin normally. Advertising WRITE_NR is assumed to mean this negotiation is supported; rejection fails closed, with no in-session fallback. The 12-byte status format is unchanged, including reserved byte 3 = 0.
- Streaming packets are `offsetLE32 + bytes`, at most `min(512, maximumWriteValueLength(.withoutResponse))` bytes total (4 bytes are offset). The production `TransferWindow` cursor queues at most eight packets, checks `canSendWriteWithoutResponse` before each write, and resumes via `peripheralIsReady(toSendWriteWithoutResponse:)`. It cannot open the next window until an exact cumulative Receiving acknowledgment at that window's end; a shortened final window also requires its acknowledgment **before Finish**. Displayed progress is board-acknowledged, not merely queued. Duplicate/stale committed offsets are ignored; new intermediate/future offsets, mismatched totals and unexpected states fail closed. The board must notify after eight accepted packets, the final packet, errors and control-state transitions. No retransmission or resume is attempted.
- Boards without WRITE_NR retain the legacy protocol: no `[6]`, Data uses ATT response and packets at most `min(180, maximumWriteValueLength(.withResponse))` bytes including offset. Each advances only after **both** ATT acknowledgment and exact matching Receiving status. Stale notifications do not enqueue duplicate packets. Digest halves in either mode need ATT acknowledgments, not changed status.
- Cancel, background entry, errors and missing progress disconnect, which aborts the board’s unfinished OTA writer under this protocol. The app clears the streaming cursor and deliberately uses disconnect rather than racing an Abort write with queued/outstanding packets; it never sends Abort after Complete. There is no resume, restoration or automatic restart. Already queued Bluetooth writes cannot be recalled, and are not considered committed without board progress. After cancellation/error, scan again explicitly and start at zero. Closing the app can only interrupt, never confirm installation.
- Timeouts: 30 seconds to select a scanning device, 15 seconds for connection/discovery, each write/status pair, or a streaming window (including backpressure), 120 seconds for entering a PIN. Only a matching window acknowledgment renews the streaming deadline; stale notifications/readiness callbacks do not. No missing progress or disconnect is success. Only matching Complete status **and** the Finish ATT acknowledgment produce the “Over to Puffin” handoff screen. This does **not** confirm the new application booted. A timeout/disconnect around Finish may leave the outcome unknown: inspect the board before trying again.

## Tests and isolated UI previews

On the Mac, after generation, select a locally available simulator name:

```sh
xcodebuild -project PuffinUpdater.xcodeproj -scheme PuffinUpdater \
  -destination 'platform=iOS Simulator,name=iPhone 16' CODE_SIGNING_ALLOWED=NO test
```

Tests cover strict status decoding and errors, wire format/PIN boundaries, plausible-image/size/segment boundaries, SHA-256, acknowledgment-before-status and status-before-acknowledgment, duplicate/stale status rejection and the Complete/Finish gate. Streaming tests cover negotiated payload caps/small MTUs, the eight-packet bound, backpressure/resume, stale/invalid/out-of-range acknowledgments, short/exact-eight/ninth-packet final windows and final-ack-before-Finish. They exercise production model code, not a second implementation. CoreBluetooth delegate integration and actual flashing still need a real board and physical iPhone/iPad; simulator screenshots do not prove BLE works.

For screenshots only, add these Scheme → Run → Arguments (or equivalent `simctl launch` arguments):

```text
--preview-updater idle
--preview-updater connect
--preview-updater pin
--preview-updater transfer
--preview-updater success
--preview-updater error
```

Use one pair per launch. Every preview visibly says **DESIGN PREVIEW · No device
activity**. The sample version/notes are illustrative, not a published release.
Buttons advance an isolated demo from release → device → code → progress →
handoff. Preview mode never creates a Bluetooth central manager, imports firmware,
downloads files or enables flashing actions. Remove the flag for real use.
Idle is also the fallback for an unknown preview state.

Prior validation (before streaming): Xcode 27.3 built the app and passed all 12 protocol/release
tests plus the guided UI regression on both iPhone 17 Pro and iPhone SE simulators.
The UI test exercises full-width button hit areas, device selection, six-digit
code validation with the keyboard open, progress, verified-transfer handoff and
Done. A separate temporary check exercised connection help, local Files import/
cancel, and an unconfigured update service. The consumer screens and a recorded
preview interaction were inspected. The production image loader also accepted
the actual ProS3 application binary during earlier validation. These checks do
not establish real-server delivery, Bluetooth delivery or successful installation
on a board.

Before real distribution, test discovery/permissions, valid and invalid PINs/lockout, wrong/oversized images, transfer at varying MTUs, stale notifications, disconnect/cancel/background at each phase, hash rejection, board timeouts, verified completion and manual post-reboot observation. No connected iPhone or board was available for this validation; physical-device testing remains required.
