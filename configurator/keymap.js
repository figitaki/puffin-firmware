// Puffin base-layer keymap: matrix geometry, action names and the USB serial
// line protocol from src/KeymapConsole.h. See README "Persistent per-key overrides".

export const ROWS = 5;
export const COLS = 12;
export const FN = 256;
export const PRESETS = ['QWERTY', 'Colemak-DH', 'Dvorak'];
// The firmware protects the Fn switch from remapping.
const FN_ROW = 4, FN_COL = 8;

// Same bit masks as KeyInput::fitted: the 51 switches on the 5 x 12 matrix.
const FITTED = [0x1F8, 0xFFF, 0xFFF, 0xFFF, 0xF57];
export const fitted = (row, col) => row >= 0 && row < ROWS && col >= 0 && col < COLS &&
  (FITTED[row] & (1 << col)) !== 0;

const NAMED = {
  0: 'Off', 32: 'Space',
  0x80: 'Ctrl', 0x81: 'Shift', 0x82: 'Alt', 0x83: 'GUI',
  0x84: 'Right Ctrl', 0x85: 'Right Shift', 0x86: 'Right Alt', 0x87: 'Right GUI',
  0xB0: 'Return', 0xB1: 'Esc', 0xB2: 'Backspace', 0xB3: 'Tab', 0xC1: 'Caps Lock',
  0xCE: 'Print Screen', 0xD1: 'Insert', 0xD2: 'Home', 0xD3: 'Page Up', 0xD4: 'Delete',
  0xD5: 'End', 0xD6: 'Page Down', 0xD7: '→', 0xD8: '←', 0xD9: '↓', 0xDA: '↑',
  0xDC: 'Num /', 0xDD: 'Num *', 0xDE: 'Num -', 0xDF: 'Num +', 0xE0: 'Num Enter',
  0xEA: 'Num 0', 0xEB: 'Num .',
  // Puffin internal actions that can appear on the base layer but not be assigned.
  256: 'Fn', 281: 'Screenshot',
};
for (let n = 1; n <= 9; ++n) NAMED[0xE0 + n] = `Num ${n}`;
for (let n = 1; n <= 12; ++n) NAMED[0xC1 + n] = `F${n}`;
for (let n = 13; n <= 24; ++n) NAMED[0xF0 + n - 13] = `F${n}`;

export function actionLabel(action) {
  if (action in NAMED) return NAMED[action];
  if (action > 32 && action <= 126) return String.fromCharCode(action);
  if (action > 135 && action <= 255) return `HID ${action - 136}`;
  return `Action ${action}`;
}

// Every action the firmware accepts for `keymap set` (TypingLayout::validAction),
// grouped for the picker. Codes 136-255 without a name stay reachable by number.
const range = (from, to) => Array.from({ length: to - from + 1 }, (_, i) => from + i);
const chars = (text) => [...text].map((c) => c.charCodeAt(0));
export const ACTION_GROUPS = [
  ['Letters', chars('abcdefghijklmnopqrstuvwxyz')],
  ['Numbers', chars('1234567890')],
  ['Symbols', chars("`-=[]\\;',./~!@#$%^&*()_+{}|:\"<>?").concat(32)],
  ['Editing', [0xB0, 0xB1, 0xB2, 0xB3, 0xD4, 0xD1, 0xC1, 0xCE]],
  ['Navigation', [0xD8, 0xDA, 0xD9, 0xD7, 0xD2, 0xD5, 0xD3, 0xD6]],
  ['Modifiers', range(0x80, 0x87)],
  ['Function keys', range(0xC2, 0xCD).concat(range(0xF0, 0xFB))],
  ['Number pad', range(0xE1, 0xE9).concat([0xEA, 0xEB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0])],
  ['Other', [0]],
];
export const assignable = (action) => action === 0 || (action >= 32 && action <= 126) ||
  (action >= 128 && action <= 255);

// Browser KeyboardEvent.code values for non-printing keys, used by "press a key".
const CODES = {
  Enter: 0xB0, NumpadEnter: 0xE0, Escape: 0xB1, Backspace: 0xB2, Tab: 0xB3, Space: 32,
  CapsLock: 0xC1, PrintScreen: 0xCE, Insert: 0xD1, Home: 0xD2, PageUp: 0xD3, Delete: 0xD4,
  End: 0xD5, PageDown: 0xD6, ArrowRight: 0xD7, ArrowLeft: 0xD8, ArrowDown: 0xD9, ArrowUp: 0xDA,
  ControlLeft: 0x80, ShiftLeft: 0x81, AltLeft: 0x82, MetaLeft: 0x83,
  ControlRight: 0x84, ShiftRight: 0x85, AltRight: 0x86, MetaRight: 0x87,
  NumpadDivide: 0xDC, NumpadMultiply: 0xDD, NumpadSubtract: 0xDE, NumpadAdd: 0xDF,
  Numpad0: 0xEA, NumpadDecimal: 0xEB,
};
for (let n = 1; n <= 9; ++n) CODES[`Numpad${n}`] = 0xE0 + n;
for (let n = 1; n <= 12; ++n) CODES[`F${n}`] = 0xC1 + n;
for (let n = 13; n <= 24; ++n) CODES[`F${n}`] = 0xF0 + n - 13;

export function actionFromKeyEvent({ code, key }) {
  if (code in CODES) return CODES[code];
  if (key.length === 1 && key.charCodeAt(0) > 32 && key.charCodeAt(0) <= 126) return key.charCodeAt(0);
  return null;
}

// "keymap dump PRESET" followed by 60 row-major tokens: "-", "N" or "N*".
export function parseDump(line) {
  const tokens = line.trim().split(/\s+/);
  if (tokens.length !== 3 + ROWS * COLS || tokens[0] !== 'keymap' || tokens[1] !== 'dump') {
    throw new Error('Unexpected keymap dump from Puffin.');
  }
  const preset = Number(tokens[2]);
  if (!(preset in PRESETS)) throw new Error('Puffin reported an unknown preset.');
  const keys = [];
  tokens.slice(3).forEach((token, index) => {
    const row = Math.floor(index / COLS), col = index % COLS;
    if (token === '-') {
      if (fitted(row, col)) throw new Error(`Puffin reported no action for switch ${row},${col}.`);
      return;
    }
    const match = /^(\d+)(\*?)$/.exec(token);
    if (!match || !fitted(row, col)) throw new Error(`Bad keymap entry at ${row},${col}.`);
    const action = Number(match[1]);
    keys.push({ row, col, action, overridden: match[2] === '*', editable: action !== FN });
  });
  return { preset, keys };
}

// A shareable layout: the preset plus every user override, by matrix position.
export function exportLayout({ preset, keys }) {
  return {
    puffinKeymap: 1,
    preset: PRESETS[preset],
    overrides: keys.filter((k) => k.overridden).map(({ row, col, action }) => ({ row, col, action })),
  };
}

export function layoutCommands(file) {
  const preset = PRESETS.indexOf(file?.preset);
  if (file?.puffinKeymap !== 1 || preset < 0 || !Array.isArray(file.overrides) || file.overrides.length > ROWS * COLS) {
    throw new Error('This is not a Puffin keymap file.');
  }
  const commands = [`keymap preset ${preset}`, 'keymap clear'];
  for (const { row, col, action } of file.overrides) {
    // Validate everything before sending anything, so an import never half-applies.
    if (!fitted(row, col) || (row === FN_ROW && col === FN_COL) ||
        !Number.isInteger(action) || !assignable(action)) {
      throw new Error(`Invalid override at ${row},${col}.`);
    }
    commands.push(`keymap set ${row} ${col} ${action}`);
  }
  return commands;
}

// Line-oriented client for the USB console. Puffin may print debug logs between
// replies, so only lines starting with "keymap " answer a command.
export class KeymapClient {
  constructor(write, timeoutMs = 3000) {
    this.write = write;
    this.timeoutMs = timeoutMs;
    this.queue = Promise.resolve();
    this.waiting = null;
    this.buffer = '';
  }

  receive(text) {
    this.buffer += text;
    const lines = this.buffer.split(/\r?\n/);
    this.buffer = lines.pop();
    for (const line of lines) {
      if (!line.startsWith('keymap ') || !this.waiting) continue;
      const { resolve, reject, timer } = this.waiting;
      this.waiting = null;
      clearTimeout(timer);
      if (line.startsWith('keymap error')) reject(new Error('Puffin rejected the command.'));
      else if (line.startsWith('keymap session-only')) reject(new Error('Puffin applied the change but could not save it; it will be lost on restart.'));
      else resolve(line);
    }
  }

  // Commands run one at a time; each waits for its own reply.
  send(command) {
    const run = () => new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiting = null;
        reject(new Error('Puffin did not answer. Make sure it is in typing mode, not MIDI mode.'));
      }, this.timeoutMs);
      this.waiting = { resolve, reject, timer };
      Promise.resolve(this.write(`${command}\n`)).catch((error) => {
        clearTimeout(timer); this.waiting = null; reject(error);
      });
    });
    const result = this.queue.then(run, run);
    this.queue = result.catch(() => {});
    return result;
  }

  async dump() { return parseDump(await this.send('keymap dump')); }
}
