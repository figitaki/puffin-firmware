// Web Bluetooth client for Puffin's BLE maintenance updater, the same protocol
// the iOS app uses (docs/ble-update-protocol.md). Hold Fn+Up while resetting
// Puffin to enter maintenance mode; its OLED then shows the six-digit PIN.

export const SERVICE = 'b9b75000-892a-4b9e-9e34-9f1b8dc7b56a';
const CONTROL = 'b9b75001-892a-4b9e-9e34-9f1b8dc7b56a';
const DATA = 'b9b75002-892a-4b9e-9e34-9f1b8dc7b56a';
const STATUS = 'b9b75003-892a-4b9e-9e34-9f1b8dc7b56a';
// The protocol allows 180-byte data writes including the 4-byte offset.
const CHUNK = 176;
const MAX_IMAGE = 8 * 1024 * 1024;

export const State = { Ready: 0, Authorized: 1, Receiving: 2, Complete: 3, Error: 4 };
const ERRORS = [
  'No board error.',
  'Incorrect PIN. Check the code on Puffin’s display. Five wrong attempts lock the session until reset.',
  'Too many wrong PINs. Reset Puffin into maintenance mode to try again.',
  'Puffin rejected the update protocol. Check that its firmware supports Bluetooth updates.',
  'The image does not fit Puffin’s inactive firmware slot.',
  'Puffin rejected a packet offset. The transfer was discarded.',
  'Puffin could not write flash. USB recovery may be needed.',
  'Puffin rejected the digest or image. Use a trusted ProS3 application firmware.bin.',
  'Puffin’s maintenance window or transfer timed out. Reset into maintenance mode.',
];

export function parseStatus(view) {
  const b = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  const u32 = (i) => (b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24) >>> 0;
  if (b.length !== 12 || b[0] !== 1 || b[3] !== 0 || b[1] > 4 || b[2] >= ERRORS.length) {
    throw new Error('Unsupported Puffin status packet.');
  }
  const status = { state: b[1], error: b[2], written: u32(4), total: u32(8) };
  if (status.written > status.total || (status.state === State.Error) !== (status.error !== 0)) {
    throw new Error('Inconsistent Puffin status. Disconnect and reset the board.');
  }
  return status;
}

const u32le = (value) => [0, 8, 16, 24].map((shift) => (value >>> shift) & 0xFF);

// Mirrors the iOS FirmwareImage checks: an ESP32-S3 application image, not a
// merged whole-flash image, bootloader or partition table.
export async function loadImage(file) {
  if (!file.name.toLowerCase().endsWith('.bin')) throw new Error('Choose an application firmware.bin file.');
  if (file.size < 33 || file.size > MAX_IMAGE) throw new Error('Firmware must be between 33 bytes and 8 MiB.');
  const bytes = new Uint8Array(await file.arrayBuffer());
  const b = bytes.subarray(0, 24);
  if (b[0] !== 0xE9 || b[1] < 1 || b[1] > 16 || b[2] > 3 || b[12] !== 9 || b[13] !== 0 || b[23] > 1) {
    throw new Error('Not an ESP32-S3 application image. Don’t choose a merged image, bootloader or partition table.');
  }
  let cursor = 24;
  for (let segment = 0; segment < b[1]; ++segment) {
    if (cursor + 8 > bytes.length) throw new Error('Truncated firmware segment header.');
    const length = (bytes[cursor + 4] | bytes[cursor + 5] << 8 | bytes[cursor + 6] << 16 | bytes[cursor + 7] << 24) >>> 0;
    cursor += 8;
    if (!length || length > bytes.length - cursor) throw new Error('Truncated or empty firmware segment.');
    cursor += length;
  }
  if (cursor >= bytes.length || (b[23] === 1 && bytes.length - cursor < 33)) {
    throw new Error('Firmware checksum or appended hash is missing.');
  }
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return { name: file.name, bytes, digest };
}

export class BleUpdater {
  constructor({ timeoutMs = 15000 } = {}) {
    this.timeoutMs = timeoutMs;
    this.latest = null;
    this.waiter = null;
    this.complete = false;
  }

  async connect() {
    this.device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] });
    this.device.addEventListener('gattserverdisconnected', () => {
      this.reject(new Error(this.complete
        ? 'Puffin restarted.'
        : 'Puffin disconnected before the update was verified. Nothing was installed; reset into maintenance mode and start again.'));
    });
    const server = await this.device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);
    [this.control, this.data, this.status] = await Promise.all(
      [CONTROL, DATA, STATUS].map((uuid) => service.getCharacteristic(uuid)));
    this.status.addEventListener('characteristicvaluechanged', (event) => this.observe(event.target.value));
    await this.status.startNotifications();
    const initial = parseStatus(await this.status.readValue());
    if (initial.state !== State.Ready) {
      this.disconnect();
      throw new Error('Puffin is not ready. Reset it into maintenance mode (hold Fn+Up) and connect again.');
    }
  }

  disconnect() {
    if (this.device?.gatt.connected) this.device.gatt.disconnect();
  }

  // Authenticate, send the digest, transfer every chunk and ask Puffin to
  // verify. Each step waits for its write acknowledgment AND matching status.
  async install(image, pin, onProgress = () => {}) {
    if (!/^\d{6}$/.test(pin)) throw new Error('Enter the six digits shown on Puffin’s display.');
    const total = image.bytes.length;
    await this.write(this.control, [1, ...new TextEncoder().encode(pin)], { state: State.Authorized });
    await this.write(this.control, [3, 0, ...image.digest.subarray(0, 16)]);
    await this.write(this.control, [3, 1, ...image.digest.subarray(16)]);
    await this.write(this.control, [2, ...u32le(total)], { state: State.Receiving, written: 0, total });
    for (let written = 0; written < total;) {
      const end = Math.min(written + CHUNK, total);
      const packet = new Uint8Array(4 + end - written);
      packet.set(u32le(written));
      packet.set(image.bytes.subarray(written, end), 4);
      await this.write(this.data, packet, { state: State.Receiving, written: end, total });
      written = end;
      onProgress(written, total);
    }
    // Only Complete means transferred and verified; Puffin then reboots itself.
    await this.write(this.control, [4], { state: State.Complete, written: total, total });
    this.complete = true;
  }

  observe(view) {
    let status;
    try { status = parseStatus(view); } catch (error) { this.reject(error); return; }
    this.latest = status;
    if (status.state === State.Error) { this.reject(new Error(ERRORS[status.error])); return; }
    const waiter = this.waiter;
    if (waiter?.expected && Object.entries(waiter.expected).every(([key, value]) => status[key] === value)) {
      waiter.observed = true;
      if (waiter.acknowledged) this.resolve();
    }
  }

  // Notifications can arrive before the write acknowledgment, so both are tracked.
  write(characteristic, bytes, expected = null) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.reject(new Error(
        'Puffin stopped responding. The transfer was discarded; reset into maintenance mode and start again.')), this.timeoutMs);
      this.waiter = { resolve, reject, timer, expected, acknowledged: false, observed: false };
      characteristic.writeValueWithResponse(Uint8Array.from(bytes)).then(() => {
        if (!this.waiter) return;
        this.waiter.acknowledged = true;
        if (!expected || this.waiter.observed) this.resolve();
      }, (error) => this.reject(new Error(`Bluetooth write failed: ${error.message}`)));
    });
  }

  resolve() {
    const waiter = this.waiter;
    this.waiter = null;
    clearTimeout(waiter.timer);
    waiter.resolve();
  }

  reject(error) {
    const waiter = this.waiter;
    if (!waiter) return;
    this.waiter = null;
    clearTimeout(waiter.timer);
    waiter.reject(error);
    this.disconnect();
  }
}
