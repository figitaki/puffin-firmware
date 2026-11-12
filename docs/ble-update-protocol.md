# Puffin BLE updater v1 (development)

This is an explicitly entered maintenance mode, not a service running beside MIDI/HID. Install the updater-capable firmware once by USB. Hold physical Fn+Up during reset/power-on to enter it. The OLED shows a fresh six-digit PIN. No keyboard or MIDI service runs during updating. Reset without those keys to return to normal operation.

The iOS app checks a publisher-controlled HTTPS release manifest, downloads and validates a ProS3 application `firmware.bin`, scans/connects, asks for the PIN and transfers while foregrounded. The production release URL still needs configuring. Local file import remains in the options menu for development/recovery. A merged whole-flash image is not an application image. Do not erase NVS or overwrite the partition table. No App Store upload or signing identity is included.

This development protocol verifies transfer integrity and ESP image validity, not publisher authenticity. It does not implement firmware signatures, secure boot or automatic post-boot rollback. Do not use it to distribute untrusted firmware. The inactive OTA slot is written; the boot slot changes only after successful digest/image validation. Never claim that a disconnect proves installation or that a completed transfer proves the new app boots. A bad application can still require USB recovery.

## Discovery

- Name: `Puffin Update`.
- Service: `B9B75000-892A-4B9E-9E34-9F1B8DC7B56A`.
- Control: `B9B75001-892A-4B9E-9E34-9F1B8DC7B56A`, write with response.
- Data: `B9B75002-892A-4B9E-9E34-9F1B8DC7B56A`, write with response.
- Status: `B9B75003-892A-4B9E-9E34-9F1B8DC7B56A`, read and notify.

All integers are little-endian. Subscribe to Status and read its current value before authenticating. These commands fit a 20-byte ATT payload. The data payload is at most 180 bytes including its offset; the app must also respect `maximumWriteValueLength(for: .withResponse)`. One transfer is active on one connection. A disconnect aborts it; reconnect and restart from zero, not a guessed resume offset.

## Control commands

1. Authenticate: `01` then six ASCII PIN digits (7 bytes). Success: Authorized status. Five bad PIN attempts lock the maintenance session until reset.
2. Digest: `03`, half index `00` or `01`, then 16 SHA-256 bytes (18 bytes). Send both halves after authorization, before Begin. Each is acknowledged by the ATT write response, with unchanged Authorized status.
3. Begin: `02` then uint32 image size (5 bytes). Requires both digest halves and size within the inactive slot. Success: Receiving status with written=0 and total=size.
4. Data characteristic: uint32 byte offset then 1–176 image bytes. Offset must equal current written, and bytes cannot exceed total. Every accepted packet produces Receiving status with the newly committed byte count. Send the next packet only after both its ATT write response and matching status progress; use no write-without-response queue.
5. Finish: `04` (1 byte), only after written=total. The board checks SHA-256 and the ESP image, sets the next boot slot and publishes Complete status, then reboots after approximately one second.
6. Abort: `05` (1 byte). Aborts the OTA writer and returns to Ready with zero progress. Authenticate and send both digest halves again to retry.

Malformed commands, bad offsets, flash failures, invalid images, timeouts and hash mismatches fail closed: discard the in-progress transfer, leave the boot slot unchanged, publish Error. Re-authenticate to retry unless locked. Abort must not undo a completed installation. After disconnect, reset transfer state (unless already Complete), but retain the PIN attempt count. Maintenance accepts new sessions for five minutes from entering mode; inactivity during a transfer times out after 30 seconds. Reset the board to reopen an expired window.

## Status (12 bytes)

`version:uint8=1, state:uint8, error:uint8, reserved:uint8=0, written:uint32, total:uint32`.

- States: Ready=0, Authorized=1, Receiving=2, Complete=3, Error=4.
- Errors: None=0, Authentication=1, Locked=2, Protocol=3, Size=4, Offset=5, Flash=6, Verification=7, Timeout=8.

The iOS app must surface errors, handle Bluetooth denied/off, cancel/disconnect/background entry, and time out missing progress. Do not automatically restart or interpret disconnection as success. For Complete, show “Transferred and verified; restarting” rather than claiming a successful boot. The PIN gate and physical maintenance window are development safeguards, not a replacement for signed updates.
