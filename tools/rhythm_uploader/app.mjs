import {verifyFirmware,parseIdentity,parseFMStatus,Transfer,IDENTITY_INQUIRY} from './protocol.mjs';
const $=id=>document.getElementById(id);
let config, firmware=null, midi=null, identity=null, transfer=null, checking=false, simulating=false, loading=false;
let currentInput=null,currentOutput=null;
const entries=[];
function log(message) {
  const line=new Date().toISOString()+' '+message;
  entries.push(line); $('log').textContent=entries.slice(-250).join('\n')+'\n';
  $('log').scrollTop=$('log').scrollHeight;
}
function active() { return checking || transfer?.active || simulating || loading; }
function selected() { return {input:midi?.inputs.get($('input').value),output:midi?.outputs.get($('output').value)}; }
function update() {
  const locked=!!active(), ports=selected(), boot=identity?.mode==='bootloader';
  $('connect').disabled=locked || !navigator.requestMIDIAccess;
  $('input').disabled=$('output').disabled=locked || !midi;
  $('identify').disabled=locked || !ports.input || !ports.output;
  $('load').disabled=locked || !config; $('file').disabled=locked; $('preset').disabled=locked;
  $('ack').disabled=locked;
  $('send').disabled=locked || !firmware || !boot || !$('ack').checked;
  $('stop').disabled=!transfer?.active;
  $('simulate').disabled=locked || !firmware;
}
function showIdentity(value) {
  identity=value;
  const custom=config?.firmwares.find(e=>e.role==='candidate');
  const customIdentified=value?.mode==='application' && value.build===custom?.identity_build;
  $('identity').textContent=value ? `Circuit Rhythm · ${value.mode} mode · build ${value.build}`:'No verified Rhythm device.';
  if(value?.mode==='application') {
    $('identity').textContent+=customIdentified ? ' · Custom FM image identified; test audio separately.' : ' · Custom FM installation is not confirmed.';
    if(value.build===15713) $('identity').textContent+=' FM memory initialization failed; this is not a working FM installation.';
    log($('identity').textContent);
  }
  $('identity').className=value?.mode==='bootloader'||customIdentified?'good':'bad'; update();
}
function fillPorts() {
  for (const [id,ports] of [['input',midi.inputs],['output',midi.outputs]]) {
    const prior=$(id).value;
    const list=[...ports.values()].filter(p=>p.state==='connected').sort((a,b)=>Number(/Circuit Rhythm/i.test(b.name))-Number(/Circuit Rhythm/i.test(a.name)));
    $(id).replaceChildren(...list.map(p=>new Option(p.name||p.id,p.id)));
    if (list.some(p=>p.id===prior)) $(id).value=prior;
  }
  update();
}
async function boundedOpen(port) {
  if (port.state!=='connected') throw Error('MIDI port is disconnected');
  let timer;
  try { await Promise.race([port.open(),new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('MIDI port did not open')),3000);})]); }
  finally { clearTimeout(timer); }
}
function closePort(port) {
  if (port?.state==='connected') Promise.resolve(port.close()).catch(()=>{});
}
async function identify() {
  const {input,output}=selected();
  if (!input || !output) throw Error('Select a MIDI input and output');
  showIdentity(null);
  await boundedOpen(input); await boundedOpen(output);
  return await new Promise((resolve,reject)=>{
    const done=(error,value)=>{
      clearTimeout(timer);input.removeEventListener('midimessage',receive);
      if(error) reject(error); else resolve(value);
    };
    const receive=event=>{
      try {
        const status=parseFMStatus(event.data);
        if(status) {log('FM runtime status: '+JSON.stringify(status));return;}
      } catch(error) {log(error.message);return;}
      const value=parseIdentity(event.data);
      if(value) {log('Identity reply: '+value.raw.map(x=>x.toString(16).padStart(2,'0')).join(' '));done(null,value);}
      else if(event.data[0]===0xf0 && event.data[1]===0x7e && event.data[4]===2) done(Error('The selected device is not a supported Circuit Rhythm'));
    };
    const timer=setTimeout(()=>done(Error('No Rhythm identity reply on the selected input/output pair')),3000);
    input.addEventListener('midimessage',receive);
    try { output.send(IDENTITY_INQUIRY);log('Sent standard identity inquiry only.'); } catch(error) {done(error);}
  });
}
async function checkDevice() {
  checking=true;update();
  try { showIdentity(await identify()); }
  catch(error) { log(error.message);$('identity').textContent=error.message;$('identity').className='bad'; }
  finally { checking=false; const ports=selected();closePort(ports.input);closePort(ports.output);update(); }
}
$('connect').onclick=async()=>{
  checking=true;update();
  try {
    midi=await navigator.requestMIDIAccess({sysex:true});
    midi.onstatechange=event=>{
      if(event.port.state==='disconnected') {
        identity=null;
        if(transfer?.active && (event.port===currentInput || event.port===currentOutput)) transfer.disconnect();
        $('identity').textContent='MIDI port disconnected; check the device again after restart.';
      }
      fillPorts();
    };
    fillPorts(); checking=false; await checkDevice();
  } catch(error) { log('MIDI access: '+error.message);$('identity').textContent=error.message; }
  finally {checking=false;update();}
};
$('identify').onclick=checkDevice;
for(const id of ['input','output']) $(id).onchange=()=>showIdentity(null);
$('ack').onchange=update;
async function load(input) {
  loading=true; firmware=null;$('ack').checked=false;update();$('fileState').textContent='Checking SHA-256, product, block structure and CRC…';
  try {
    const bytes=await input;
    firmware=await verifyFirmware(bytes,config.firmwares);
    $('fileState').textContent=`Verified: ${firmware.entry.label} · ${firmware.size.toLocaleString()} application bytes`;
    $('fileState').className='good';$('hash').textContent='SHA-256 '+firmware.hash;
    $('progress').value=0;$('transfer').textContent='Nothing has been transferred.';
    log('Verified file '+firmware.entry.label+' sha256='+firmware.hash);
  } catch(error) { $('fileState').textContent=error.message;$('fileState').className='bad';$('hash').textContent='';log('File rejected: '+error.message); }
  loading=false;update();
}
$('load').onclick=()=>load((async()=>{
  const response=await fetch(config.firmwares[Number($('preset').value)].file);
  if(!response.ok)throw Error('Packaged file could not be read');
  return response.arrayBuffer();
})());
$('file').onchange=()=>{if($('file').files[0])load($('file').files[0].arrayBuffer());};
function onTransfer(snapshot) {
  $('progress').value=100*snapshot.sent/snapshot.total;
  $('transfer').textContent=snapshot.message;
  $('transfer').className=snapshot.state==='complete'?'good':['failed','unconfirmed'].includes(snapshot.state)?'bad':'';
  if(snapshot.state!=='transferring' || snapshot.sent%500===0)log(snapshot.state+': '+snapshot.message+` (${snapshot.sent}/${snapshot.total} messages)`);
  if(!transfer.active) {
    currentInput?.removeEventListener('midimessage',receiveUpdate);
    identity=null;closePort(currentInput);closePort(currentOutput);
    $('identity').textContent='Check the device again before another transfer.';
  }
  update();
}
function receiveUpdate(event) { transfer?.receive(event.data); }
$('send').onclick=async()=>{
  if(!firmware || !$('ack').checked || active())return;
  const chosen=firmware;
  if(!confirm(`Transfer ${chosen.entry.label} to the selected Circuit Rhythm?\n\nThis writes firmware. Keep USB and power connected.\nNo transfer starts until its bootloader identity is checked again.`))return;
  checking=true;update();
  try {
    const fresh=await identify();showIdentity(fresh);
    if(fresh.mode!=='bootloader')throw Error('Enter bootloader mode before transferring');
    ({input:currentInput,output:currentOutput}=selected());
    currentInput.addEventListener('midimessage',receiveUpdate);
    transfer=new Transfer({firmware:chosen,send:packet=>{
      if(currentOutput.state!=='connected')throw Error('MIDI output disconnected');
      currentOutput.send(packet);
    },onChange:onTransfer});
    log(`Transfer authorized: ${firmware.entry.label}, sha256=${firmware.hash}, bootloader=${fresh.build}`);
    transfer.start(fresh);
  } catch(error){log('Transfer not started: '+error.message);$('transfer').textContent=error.message;const ports=selected();closePort(ports.input);closePort(ports.output);}
  finally {checking=false;update();}
};
$('stop').onclick=()=>transfer?.stop();
$('simulate').onclick=async()=>{
  simulating=true;update();let sent=0,latest;
  $('simulation').textContent='Running simulated device requests; MIDI is not opened…';
  const engine=new Transfer({firmware,send:()=>{sent++;},onChange:s=>{latest=s;}});
  try {
    engine.start({product:0x63,mode:'bootloader'});
    const request=n=>Uint8Array.of(0xf0,0,0x20,0x29,0,0x72,n&127,(n>>7)&127,(n>>14)&127,0xf7);
    for(let n=1;n<firmware.blocks;n++) {
      engine.receive(request(n));
      if(n%500===0){$('simulation').textContent=`Simulation: ${n.toLocaleString()} blocks checked…`;await new Promise(resolve=>setTimeout(resolve,0));}
    }
    engine.receive(request(0));engine.receive(Uint8Array.of(0xf0,0,0x20,0x29,0,0x73,127,0,0,0xf7));
    if(latest.state!=='complete' || sent!==firmware.packets.length)throw Error('Simulation failed');
    $('simulation').textContent=`Simulation passed: ${sent.toLocaleString()} messages. No firmware was transferred.`;
    $('simulation').className='good';log($('simulation').textContent);
  } catch(error){engine.stop();$('simulation').textContent=error.message;log(error.message);}
  finally{simulating=false;update();}
};
$('export').onclick=()=>{
  const text=['Circuit Rhythm firmware transfer session',`File: ${firmware?.entry.label||'none'}`,`SHA-256: ${firmware?.hash||'none'}`,
              `Transfer: ${JSON.stringify(transfer?.snapshot()||null)}`,'',...entries].join('\n');
  const url=URL.createObjectURL(new Blob([text],{type:'text/plain'})),a=document.createElement('a');
  a.href=url;a.download='rhythm-transfer-log.txt';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
};
window.addEventListener('beforeunload',event=>{if(transfer?.active){event.preventDefault();event.returnValue='';}});
try {
  const response=await fetch('approved.json');if(!response.ok)throw Error('Packaged firmware checks are missing');config=await response.json();
  $('preset').replaceChildren(...config.firmwares.map((item,i)=>new Option(item.label,String(i))));
  $('configuration').textContent=config.recovery_only ? config.notice+' Only official recovery firmware is available.' : 'Only the packaged FM candidate and official Rhythm recovery file are accepted.';
  if(!navigator.requestMIDIAccess)$('identity').textContent='Open this local page in Chrome or Edge for Web MIDI. Offline simulation remains available.';
} catch(error){$('configuration').textContent=error.message;log(error.message);}
update();
