// SPDX-License-Identifier: GPL-3.0-only
// SteamRoller installer: finds the Circuit Rhythm over Web MIDI, checks the
// packaged firmware against firmware.json, and sends it to the Rhythm's
// bootloader (protocol.mjs: Novation's update protocol, block by block).
import {verifyFirmware, parseIdentity, Transfer, IDENTITY_INQUIRY} from './protocol.mjs';

const $ = id => document.getElementById(id);
const log = [];
let release = null, firmware = null, midi = null, device = null, transfer = null, busy = false;

function note(message) { if (message) log.push(new Date().toISOString() + ' ' + message); }
function say(status, detail = '', tone = '') {
  $('status').textContent = status;
  $('status').className = 'status' + (tone ? ' ' + tone : '');
  $('detail').textContent = detail;
  note(status + (detail ? ' — ' + detail : ''));
}
function refresh() {
  const ready = device?.identity.mode === 'bootloader' && firmware && !busy && !transfer?.active;
  $('connect').disabled = busy || !firmware || !navigator.requestMIDIAccess || !!transfer?.active;
  $('install').disabled = !ready || !$('agree').checked;
  $('agree').disabled = !!transfer?.active;
}
function showDevice(identity) {
  $('facts').hidden = false;
  $('f-dev').textContent = !identity ? 'not found'
    : identity.mode === 'bootloader' ? `in update mode (bootloader ${identity.build})`
    : identity.build === release.identity_build ? `SteamRoller ${release.version}`
    : `running normally (firmware build ${identity.build})`;
}

// The packaged firmware, checked against its published hashes before use.
async function loadFirmware() {
  const response = await fetch('firmware/firmware.json', {cache: 'no-store'});
  if (!response.ok) throw Error('The firmware description could not be loaded.');
  release = await response.json();
  const file = await fetch(release.file, {cache: 'no-store'});
  if (!file.ok) throw Error('The firmware could not be loaded.');
  firmware = await verifyFirmware(await file.arrayBuffer(), [{...release, role: 'candidate'}]);
  $('facts').hidden = false;
  $('f-fw').textContent = `SteamRoller ${release.version} (build ${release.identity_build})`;
  $('install').textContent = `Install SteamRoller ${release.version}`;
  note(`Firmware ${release.version} verified, sha256=${firmware.hash}`);
}

function wait(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }
async function open(port) {
  await Promise.race([port.open(), wait(3000).then(() => { throw Error('MIDI port did not open'); })]);
}
// Ask one output for its identity; any input may answer.
function ask(output, inputs) {
  return new Promise(resolve => {
    const done = value => {
      clearTimeout(timer);
      for (const input of inputs) input.removeEventListener('midimessage', receive);
      resolve(value);
    };
    const receive = event => {
      const identity = parseIdentity(event.data);
      if (identity) done({input: event.target, output, identity});
    };
    for (const input of inputs) input.addEventListener('midimessage', receive);
    const timer = setTimeout(() => done(null), 1500);
    try { output.send(IDENTITY_INQUIRY); } catch { done(null); }
  });
}
// Find a Circuit Rhythm among the connected MIDI ports (likely names first).
async function findRhythm() {
  const likely = port => /circuit|rhythm|novation/i.test(port.name || '') ? 0 : 1;
  const inputs = [...midi.inputs.values()].filter(p => p.state === 'connected');
  const outputs = [...midi.outputs.values()].filter(p => p.state === 'connected').sort((a, b) => likely(a) - likely(b));
  for (const input of inputs) await open(input).catch(() => {});
  for (const output of outputs) {
    try { await open(output); } catch { continue; }
    const found = await ask(output, inputs);
    if (found) { note(`Found ${found.identity.mode} build ${found.identity.build} on ${output.name}`); return found; }
  }
  return null;
}

async function connect() {
  busy = true; device = null; refresh();
  try {
    say('Looking for your Circuit Rhythm…');
    midi ??= await navigator.requestMIDIAccess({sysex: true});
    device = await findRhythm();
    showDevice(device?.identity);
    if (!device)
      say('No Circuit Rhythm found.', 'Check the USB cable, close other music programs, and press Find my Circuit Rhythm again.', 'bad');
    else if (device.identity.mode === 'bootloader')
      say('Ready to install.', $('agree').checked ? '' : 'Tick the box above to enable the install.', 'ok');
    else if (device.identity.build === release.identity_build)
      say(`SteamRoller ${release.version} is already installed.`, 'To install it again, start the Rhythm in update mode (step 1 below) and press Find my Circuit Rhythm.', 'ok');
    else
      say('Your Circuit Rhythm is running normally.', 'Switch it off and start it in update mode (step 1 below), then press Find my Circuit Rhythm again.');
  } catch (error) {
    if (error.name === 'SecurityError' || error.name === 'NotAllowedError')
      say('MIDI access was not allowed.', 'Allow this page to control MIDI devices and press Find my Circuit Rhythm again.', 'bad');
    else say('Could not connect.', error.message, 'bad');
  } finally { busy = false; refresh(); }
}

const progress = {
  starting: 'Starting the update…',
  transferring: 'Installing… Keep the cable and power connected.',
  verifying: 'Finishing… The Rhythm is checking the firmware.',
};
async function install() {
  if (!device || !firmware || !$('agree').checked) return;
  busy = true; refresh();
  try {
    const fresh = await ask(device.output, [device.input]);   // still the same Rhythm, still in update mode
    if (fresh?.identity.mode !== 'bootloader') throw Error('The Rhythm is no longer in update mode. Press Find my Circuit Rhythm again.');
    const {input, output} = device;
    const bar = $('bar').firstElementChild;
    $('bar').hidden = false; bar.style.width = '0';
    const receive = event => transfer?.receive(event.data);
    input.addEventListener('midimessage', receive);
    transfer = new Transfer({
      firmware,
      send: packet => output.send(packet),
      onChange: snapshot => {
        bar.style.width = (100 * snapshot.sent / snapshot.total).toFixed(1) + '%';
        if (snapshot.state === 'complete')
          say('SteamRoller is installed.', 'Your Circuit Rhythm restarts by itself. You can disconnect it once it has started.', 'ok');
        else if (['failed', 'stopped', 'unconfirmed'].includes(snapshot.state))
          say('The install did not finish.', snapshot.message + '. Start the Rhythm in update mode again and press Find my Circuit Rhythm.', 'bad');
        else if (/press Play/i.test(snapshot.message))
          say('Press Play on the Circuit Rhythm to confirm.');
        else if (progress[snapshot.state] && $('status').textContent !== progress[snapshot.state])
          say(progress[snapshot.state]);
        if (!transfer.active) {
          input.removeEventListener('midimessage', receive);
          device = null; busy = false; showDevice(null); $('f-dev').textContent = 'restarting';
        }
        refresh();
      },
    });
    midi.onstatechange = event => {
      if (event.port.state === 'disconnected' && (event.port === input || event.port === output)) transfer?.disconnect();
    };
    note(`Install started: SteamRoller ${release.version}, bootloader build ${fresh.identity.build}`);
    transfer.start(fresh.identity);
  } catch (error) {
    say('The install did not start.', error.message, 'bad'); busy = false; device = null;
  }
  refresh();
}

$('connect').onclick = connect;
$('install').onclick = install;
$('agree').onchange = () => {
  refresh();
  if ($('agree').checked && device?.identity.mode === 'bootloader') $('detail').textContent = '';
};
$('savelog').onclick = () => {
  const text = [`SteamRoller ${release?.version || '?'} installer log`, `Browser: ${navigator.userAgent}`, '', ...log].join('\n');
  const url = URL.createObjectURL(new Blob([text], {type: 'text/plain'}));
  const link = Object.assign(document.createElement('a'), {href: url, download: 'steamroller-install-log.txt'});
  link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
};
window.addEventListener('beforeunload', event => { if (transfer?.active) { event.preventDefault(); event.returnValue = ''; } });

try {
  await loadFirmware();
  if (!navigator.requestMIDIAccess)
    say('This browser cannot talk to MIDI devices.', 'Open this page in a desktop Chrome or Edge.', 'bad');
  else say('Start your Circuit Rhythm in update mode.', 'See the steps below, then press Find my Circuit Rhythm.');
} catch (error) {
  say('The firmware could not be loaded.', error.message, 'bad');
}
refresh();
