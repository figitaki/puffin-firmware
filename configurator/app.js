import { PRESETS, ACTION_GROUPS, KeymapClient, actionLabel, actionFromKeyEvent,
  exportLayout, layoutCommands } from './keymap.js';
import { BleUpdater, loadImage } from './updater.js';

const $ = (id) => document.getElementById(id);

// Tabs
for (const tab of document.querySelectorAll('[data-tab]')) {
  tab.addEventListener('click', () => {
    for (const other of document.querySelectorAll('[data-tab]')) {
      other.setAttribute('aria-selected', String(other === tab));
      $(other.dataset.tab).hidden = other !== tab;
    }
  });
}

function say(element, text, error = false) {
  element.textContent = text;
  element.classList.toggle('error', error);
}

// ---- Keymap over USB serial ----

let client = null;
let keymap = null;
let selected = null;
let capturing = false;
const keymapMessage = $('keymap-message');

$('no-serial').hidden = 'serial' in navigator;
$('connect').disabled = !('serial' in navigator);
$('preset').append(...PRESETS.map((name, index) => new Option(name, index)));

for (const [title, actions] of ACTION_GROUPS) {
  const group = document.createElement('div');
  group.innerHTML = `<h3>${title}</h3><div class="choices"></div>`;
  for (const action of actions) {
    const button = document.createElement('button');
    button.textContent = actionLabel(action);
    button.dataset.action = action;
    button.addEventListener('click', () => assign(action));
    group.lastChild.append(button);
  }
  $('groups').append(group);
}

function setConnected(connected) {
  for (const id of ['preset', 'export', 'clear', 'import']) $(id).disabled = !connected;
  $('import-label').setAttribute('aria-disabled', String(!connected));
  $('board').classList.toggle('disabled', !connected);
  $('connect').textContent = connected ? 'Connected' : 'Connect Puffin over USB';
  $('connect').disabled = connected || !('serial' in navigator);
  if (!connected) { selected = null; $('editor').hidden = true; }
}

function render() {
  const board = $('board');
  board.replaceChildren();
  $('preset').value = keymap.preset;
  for (const key of keymap.keys) {
    const button = document.createElement('button');
    button.className = 'key';
    button.style.gridRow = key.row + 1;
    button.style.gridColumn = key.col + 1;
    button.textContent = actionLabel(key.action);
    button.classList.toggle('long', button.textContent.length > 3);
    button.classList.toggle('overridden', key.overridden);
    button.classList.toggle('fixed', !key.editable);
    button.classList.toggle('selected', selected?.row === key.row && selected?.col === key.col);
    button.title = key.editable ? `Row ${key.row}, column ${key.col}` : 'Fn stays fixed so presets and recovery are always reachable';
    button.addEventListener('click', () => select(key));
    board.append(button);
  }
  const current = selected && keymap.keys.find((k) => k.row === selected.row && k.col === selected.col);
  $('editor').hidden = !current;
  if (!current) return;
  $('editor-title').textContent = `${actionLabel(current.action)} key`;
  $('editor-detail').textContent = `Row ${current.row}, column ${current.col}. ` +
    (current.overridden ? `Changed from the ${PRESETS[keymap.preset]} layout.` : `From the ${PRESETS[keymap.preset]} layout.`);
  $('reset-key').disabled = !current.overridden;
  for (const button of $('groups').querySelectorAll('button')) {
    button.classList.toggle('current', Number(button.dataset.action) === current.action);
  }
}

function select(key) {
  if (!key.editable) { say(keymapMessage, 'Fn can’t be remapped, so you can always reach the Fn layers.'); return; }
  selected = { row: key.row, col: key.col };
  stopCapture();
  render();
}

// Run commands, then re-read the whole layer so the page always shows Puffin's state.
async function run(commands, done) {
  try {
    for (const command of commands) await client.send(command);
    keymap = await client.dump();
    render();
    if (done) say(keymapMessage, done);
  } catch (error) {
    say(keymapMessage, error.message, true);
    try { keymap = await client.dump(); render(); } catch { /* reported above */ }
  }
}

function assign(action) {
  stopCapture();
  if (selected) run([`keymap set ${selected.row} ${selected.col} ${action}`], `Saved: ${actionLabel(action)}.`);
}

$('connect').addEventListener('click', async () => {
  let port;
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
  } catch (error) {
    if (error.name !== 'NotFoundError') say(keymapMessage, `Couldn’t open Puffin: ${error.message}`, true);
    return;
  }
  const writer = port.writable.getWriter();
  const encoder = new TextEncoder();
  client = new KeymapClient((text) => writer.write(encoder.encode(text)));
  const reader = port.readable.pipeThrough(new TextDecoderStream()).getReader();
  (async () => {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        client.receive(value);
      }
    } catch { /* unplugged */ }
    client = null; keymap = null;
    setConnected(false);
    say(keymapMessage, 'Puffin disconnected.', true);
  })();
  try {
    keymap = await client.dump();
    setConnected(true);
    render();
    say(keymapMessage, 'Connected. Choose a key to change it.');
  } catch (error) {
    say(keymapMessage, error.message, true);
  }
});

$('preset').addEventListener('change', (event) => {
  const preset = Number(event.target.value);
  run([`keymap preset ${preset}`], `Base layout: ${PRESETS[preset]}. Your changed keys stay where they are.`);
});

$('clear').addEventListener('click', () => {
  if (confirm(`Reset every key to the ${PRESETS[keymap.preset]} layout?`)) {
    run(['keymap clear'], 'All keys use the base layout again.');
  }
});

$('reset-key').addEventListener('click', () => {
  if (selected) run([`keymap reset ${selected.row} ${selected.col}`], 'Key uses the base layout again.');
});

// "Press a key to assign": the next key pressed on any keyboard is assigned.
function onCapture(event) {
  event.preventDefault();
  const action = actionFromKeyEvent(event);
  if (action === null) { say(keymapMessage, `${event.key} can’t be assigned.`, true); return; }
  assign(action);
}
function stopCapture() {
  if (!capturing) return;
  capturing = false;
  window.removeEventListener('keydown', onCapture, true);
  $('capture').textContent = 'Press a key to assign…';
}
$('capture').addEventListener('click', () => {
  if (capturing) { stopCapture(); return; }
  capturing = true;
  window.addEventListener('keydown', onCapture, true);
  $('capture').textContent = 'Waiting for a key… (click to cancel)';
});

$('export').addEventListener('click', () => {
  const blob = new Blob([JSON.stringify(exportLayout(keymap), null, 2)], { type: 'application/json' });
  const link = Object.assign(document.createElement('a'), {
    href: URL.createObjectURL(blob), download: `puffin-keymap-${PRESETS[keymap.preset].toLowerCase()}.json`,
  });
  link.click();
  URL.revokeObjectURL(link.href);
});

$('import').addEventListener('change', async (event) => {
  const file = event.target.files[0];
  event.target.value = '';
  if (!file) return;
  let commands;
  try {
    commands = layoutCommands(JSON.parse(await file.text()));
  } catch (error) {
    say(keymapMessage, error instanceof SyntaxError ? 'This is not a Puffin keymap file.' : error.message, true);
    return;
  }
  run(commands, `Loaded ${file.name}.`);
});

// ---- Firmware update over Bluetooth ----

let image = null;
let updater = null;
const firmwareMessage = $('firmware-message');
const bluetooth = 'bluetooth' in navigator;
$('no-bluetooth').hidden = bluetooth;

function setUpdateControls({ connected = false, installing = false } = {}) {
  $('ble-connect').disabled = !bluetooth || !image || connected || installing;
  $('pin').disabled = !connected || installing;
  $('install').disabled = !connected || installing;
  $('ble-cancel').disabled = !connected && !installing;
  $('image').disabled = installing;
}

$('image').addEventListener('change', async (event) => {
  const file = event.target.files[0];
  image = null;
  $('image-name').textContent = file ? file.name : 'No file chosen';
  setUpdateControls();
  if (!file) return;
  try {
    image = await loadImage(file);
    say(firmwareMessage, `Firmware checked (${(image.bytes.length / 1024).toFixed(0)} KB). Put Puffin in update mode, then connect.`);
  } catch (error) {
    say(firmwareMessage, error.message, true);
  }
  setUpdateControls();
});

$('ble-connect').addEventListener('click', async () => {
  updater = new BleUpdater();
  try {
    say(firmwareMessage, 'Choose “Puffin Update” in the list…');
    await updater.connect();
    updater.device.addEventListener('gattserverdisconnected', () => {
      if (!updater?.complete) setUpdateControls();
    });
    setUpdateControls({ connected: true });
    say(firmwareMessage, 'Connected. Enter the PIN shown on Puffin’s display.');
    $('pin').focus();
  } catch (error) {
    if (error.name !== 'NotFoundError') say(firmwareMessage, error.message, true);
    else say(firmwareMessage, '');
    setUpdateControls();
  }
});

$('install').addEventListener('click', async () => {
  const progress = $('progress');
  setUpdateControls({ connected: true, installing: true });
  progress.hidden = false; progress.value = 0;
  say(firmwareMessage, 'Sending firmware. Keep this page open and stay near Puffin.');
  try {
    await updater.install(image, $('pin').value.trim(), (written, total) => {
      progress.value = written / total;
      if (written === total) say(firmwareMessage, 'Puffin is verifying the firmware…');
    });
    say(firmwareMessage, 'Transferred and verified. Puffin is restarting into the new firmware.');
    setUpdateControls();
  } catch (error) {
    say(firmwareMessage, error.message, true);
    progress.hidden = true;
    setUpdateControls();
  }
});

$('ble-cancel').addEventListener('click', () => {
  updater?.disconnect();
  $('progress').hidden = true;
  say(firmwareMessage, 'Canceled. Nothing was installed; start again from the beginning.');
  setUpdateControls();
});

setConnected(false);
setUpdateControls();
