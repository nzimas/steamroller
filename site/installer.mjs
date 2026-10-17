// SteamRoller installer: finds the Circuit Rhythm over Web MIDI, checks the
// packaged firmware, and sends it to the Rhythm's bootloader.
import {verifyFirmware, parseIdentity, Transfer, IDENTITY_INQUIRY} from './protocol.mjs';

const $ = id => document.getElementById(id);
const log = [];
let release = null, firmware = null, midi = null, device = null, transfer = null, busy = false;

function note(message) { log.push(new Date().toISOString() + ' ' + message); }
function say(message, tone = '') {
  $('status').textContent = message;
  $('status').className = tone;
  note(message);
}
function refresh() {
  const ready = device?.identity.mode === 'bootloader' && firmware && !busy;
  $('connect').hidden = !!ready || !!transfer?.active || (busy && !!device);
  $('connect').disabled = busy || !navigator.requestMIDIAccess;
  $('install').hidden = !ready;
}

// The packaged firmware, checked against its published hashes before use.
async function loadFirmware() {
  const response = await fetch('firmware.json', {cache: 'no-store'});
  if (!response.ok) throw Error('The firmware description could not be loaded.');
  release = await response.json();
  $('version').textContent = 'Version ' + release.version;
  $('install').textContent = 'Install SteamRoller ' + release.version;
  const file = await fetch(release.file, {cache: 'no-store'});
  if (!file.ok) throw Error('The firmware could not be loaded.');
  firmware = await verifyFirmware(await file.arrayBuffer(), [{...release, role: 'candidate'}]);
  note(`Firmware ${release.version} verified, sha256=${firmware.hash}`);
}

function wait(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }
async function open(port) {
  await Promise.race([port.open(), wait(3000).then(() => { throw Error('MIDI port did not open'); })]);
}
// Ask one output for its identity; any input may answer.
async function ask(output, inputs) {
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
    if (!device) say('No Circuit Rhythm found. Check the USB cable, close other music apps, and press Connect again.', 'bad');
    else if (device.identity.mode === 'bootloader') say('Circuit Rhythm ready for update.', 'good');
    else if (device.identity.build === release?.identity_build)
      say(`SteamRoller ${release.version} is already installed. To reinstall, restart the Rhythm in update mode (steps above).`, 'good');
    else say('Your Circuit Rhythm is running normally. Restart it in update mode (steps above), then press Connect again.');
  } catch (error) {
    say(error.name === 'SecurityError' || error.name === 'NotAllowedError'
      ? 'MIDI access was not allowed. Allow it in the browser and press Connect again.'
      : 'Could not connect: ' + error.message, 'bad');
  } finally { busy = false; refresh(); }
}

const friendly = {
  starting: 'Starting…',
  transferring: 'Installing… Keep the USB cable connected.',
  verifying: 'Finishing… The Rhythm is checking the firmware.',
};
async function install() {
  if (!device || !firmware) return;
  if (!confirm(`Install SteamRoller ${release.version} on this Circuit Rhythm?\n\nKeep the USB cable and power connected until it is done.`)) return;
  busy = true; refresh();
  try {
    const fresh = await ask(device.output, [device.input]);   // still the same Rhythm, still in update mode
    if (fresh?.identity.mode !== 'bootloader') throw Error('The Circuit Rhythm is no longer in update mode. Press Connect again.');
    const {input, output} = device;
    $('progress').hidden = false; $('progress').value = 0;
    const receive = event => transfer?.receive(event.data);
    input.addEventListener('midimessage', receive);
    transfer = new Transfer({
      firmware,
      send: packet => output.send(packet),
      onChange: snapshot => {
        $('progress').value = 100 * snapshot.sent / snapshot.total;
        if (snapshot.state === 'complete') {
          say('Done. Your Circuit Rhythm restarts with SteamRoller.', 'good');
        } else if (['failed', 'stopped', 'unconfirmed'].includes(snapshot.state)) {
          say('The install did not finish: ' + snapshot.message + '. Restart the Rhythm in update mode and try again.', 'bad');
        } else if (/press Play/i.test(snapshot.message)) {
          say('Press Play on the Circuit Rhythm to confirm.');
        } else if (friendly[snapshot.state] && $('status').textContent !== friendly[snapshot.state]) {
          say(friendly[snapshot.state]);
        }
        if (!transfer.active) {
          input.removeEventListener('midimessage', receive);
          device = null; busy = false; refresh();
          if (snapshot.state !== 'complete') $('connect').hidden = false;
        }
      },
    });
    midi.onstatechange = event => {
      if (event.port.state === 'disconnected' && (event.port === input || event.port === output)) transfer?.disconnect();
    };
    $('install').hidden = true;
    note(`Install started: SteamRoller ${release.version}, bootloader build ${fresh.identity.build}`);
    transfer.start(fresh.identity);
    refresh();
  } catch (error) {
    say(error.message, 'bad'); busy = false; device = null; refresh();
  }
}

$('connect').onclick = connect;
$('install').onclick = install;
$('savelog').onclick = () => {
  const text = [`SteamRoller ${release?.version || '?'} installer log`, `Browser: ${navigator.userAgent}`, '', ...log].join('\n');
  const url = URL.createObjectURL(new Blob([text], {type: 'text/plain'}));
  const link = Object.assign(document.createElement('a'), {href: url, download: 'steamroller-install-log.txt'});
  link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
};
window.addEventListener('beforeunload', event => { if (transfer?.active) { event.preventDefault(); event.returnValue = ''; } });

try {
  await loadFirmware();
  if (!navigator.requestMIDIAccess) say('This browser cannot talk to MIDI devices. Open this page in Chrome or Edge on a computer.', 'bad');
  else say('');
} catch (error) {
  say(error.message, 'bad');
}
refresh();
