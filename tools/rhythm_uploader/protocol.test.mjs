/* Node's built-in test runner; no MIDI APIs or third-party packages. */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {parseFirmware,verifyFirmware,parseIdentity,parseFMStatus,parseReply,Transfer,FM_FAMILIES,V4_FIELDS,V8_FIELDS,V9_FIELDS,V11_FIELDS,V12_FIELDS,V13_FIELDS,V14_FIELDS,V15_FIELDS,V16_FIELDS,V17_FIELDS,V18_FIELDS,V19_FIELDS,V20_FIELDS,V21_FIELDS,V22_FIELDS,V23_FIELDS,GEN_FAMILIES} from './protocol.mjs';
const root=new URL('../../',import.meta.url);
const file=name=>new Uint8Array(fs.readFileSync(new URL(name,root)));
const bundle=process.env.RHYTHM_TEST_BUNDLE || 'build/rhythm-fm';
const approved=JSON.parse(fs.readFileSync(new URL(bundle+'/uploader/approved.json',root))).firmwares;
const candidate=approved.find(e=>e.role==='candidate');
const custom=new Uint8Array(fs.readFileSync(new URL(candidate.file,new URL(bundle+'/uploader/',root))));
const recovery=file('firmware/official/circuitrhythm-firmware-5706.syx');
const firmware=await verifyFirmware(custom,approved);
const boot=Uint8Array.from([0xf0,0x7e,0,6,2,0,0x20,0x29,0x11,0x63,0,0,0,3,0x25,0x3f,0xf7]);
const identity=parseIdentity(boot);
const request=n=>Uint8Array.of(0xf0,0,0x20,0x29,0,0x72,n&127,(n>>7)&127,(n>>14)&127,0xf7);
const status=n=>Uint8Array.of(0xf0,0,0x20,0x29,0,0x73,n,0,0,0xf7);
class Clock {
  next=1;callbacks=new Map();
  setTimeout(fn,ms) { const id=this.next++;this.callbacks.set(id,{fn,ms});return id; }
  clearTimeout(id) {this.callbacks.delete(id);}
  fire(){const {fn}=this.callbacks.values().next().value;this.callbacks.clear();fn();}
}
function session(f=firmware,send=null) {
  const packets=[],clock=new Clock(),changes=[];
  const engine=new Transfer({firmware:f,send:send|| (p=>packets.push(p)),clock,onChange:s=>changes.push(s)});
  return {engine,packets,clock,changes};
}
test('both packaged firmware files pass exact hashes, image CRC, product and structure',async()=>{
  assert.equal(firmware.size,candidate.size);
  assert.equal(String.fromCharCode(...firmware.image.subarray(0x1e8,0x1ee)),candidate.installation_marker);
  assert.equal(firmware.entry.identity_build,candidate.identity_build);
  assert.equal((await verifyFirmware(recovery,approved)).entry.role,'recovery');
});
test('non-word-aligned header size is rejected before a transfer can start',()=>{
  const data=custom.slice(),size=firmware.size-2;
  for(let i=0;i<8;i++)data[15+13+i]=(size>>>((7-i)*4))&15;
  assert.throws(()=>parseFirmware(data),/word-aligned/);
});
test('candidate approval rejects a missing installation marker or wrong observable identity',async()=>{
  for(const change of [{installation_marker:'WRONG!'},{identity_build:5706}]) {
    await assert.rejects(verifyFirmware(custom,approved.map(e=>e.role==='candidate'?{...e,...change}:e)),/marker|identity/);
  }
});
test('unknown or corrupted file cannot be approved',async()=>{
  const data=custom.slice();data[100]^=1;
  await assert.rejects(verifyFirmware(data,approved),/Unrecognized/);
});
test('container parser rejects a Tracks product and appended garbage',()=>{
  const tracks=custom.slice();tracks[7]=0x64;
  assert.throws(()=>parseFirmware(tracks),/not Circuit Rhythm/);
  const tail=new Uint8Array(custom.length+1);tail.set(custom);tail[custom.length]=0;
  assert.throws(()=>parseFirmware(tail),/outside SysEx/);
});
test('CRC and message-length validation are independent of allowlist hashing',()=>{
  const data=custom.slice();data[100]^=1;
  assert.throws(()=>parseFirmware(data),/CRC/);
  assert.throws(()=>parseFirmware(custom.subarray(0,custom.length-1)),/Unterminated/);
});
test('identity checks the Rhythm product and correct mode byte',()=>{
  assert.equal(identity.mode,'bootloader');assert.equal(identity.build,4799);
  const normal=boot.slice();normal[8]=1;assert.equal(parseIdentity(normal).mode,'application');
  const tracks=boot.slice();tracks[9]=0x64;assert.equal(parseIdentity(tracks),null);
  const foreign=boot.slice();foreign[6]=1;assert.equal(parseIdentity(foreign),null);
  assert.equal(parseIdentity(boot.subarray(0,16)),null);
});
test('no transfer commands before a bootloader identity is supplied',()=>{
  const {engine,packets}=session();
  assert.throws(()=>engine.start(null),/verified Rhythm/);
  assert.throws(()=>engine.start({...identity,mode:'application'}),/verified Rhythm/);
  assert.equal(packets.length,0);
});
for(const [label,bytes] of [['custom',custom],['official recovery',recovery]]) {
  test(`${label}: complete pull transfer sends every packet once, with block zero last`,()=>{
    const f=parseFirmware(bytes),{engine,packets,clock}=session(f);
    engine.start(identity);assert.equal(packets.length,2);
    engine.receive(status(0));assert.equal(packets.length,2);
    for(let n=1;n<f.blocks;n++)engine.receive(request(n));
    assert.equal(engine.state,'transferring');
    engine.receive(request(0));assert.equal(engine.state,'verifying');
    assert.equal(packets.length,f.packets.length);
    for(let i=0;i<packets.length;i++)assert.deepEqual(packets[i],f.packets[i]);
    engine.receive(status(125));assert.equal(engine.state,'verifying');
    engine.receive(status(127));assert.equal(engine.state,'complete');
    assert.equal(clock.callbacks.size,0);
  });
}
test('duplicate requests never advance or replay a block',()=>{
  const {engine,packets}=session();engine.start(identity);
  engine.receive(request(1));engine.receive(request(1));assert.equal(packets.length,3);
  engine.receive(request(2));assert.equal(packets.length,4);
});
test('out-of-order request stops before another data packet',()=>{
  const {engine,packets}=session();engine.start(identity);engine.receive(request(2));
  assert.equal(engine.state,'failed');assert.equal(packets.length,2);
  engine.receive(request(1));assert.equal(packets.length,2);
});
test('premature block zero and premature success are rejected',()=>{
  for(const reply of [request(0),status(127)]) {
    const {engine,packets}=session();engine.start(identity);engine.receive(reply);
    assert.equal(engine.state,'failed');assert.equal(packets.length,2);
  }
});
for(const code of [16,17,124,126,42])test(`device status ${code} stops sending`,()=>{
  const {engine,packets}=session();engine.start(identity);engine.receive(status(code));engine.receive(request(1));
  assert.equal(engine.state,'failed');assert.equal(packets.length,2);
});
test('timeout does not retry or send another packet',()=>{
  const {engine,packets,clock}=session();engine.start(identity);clock.fire();
  assert.equal(engine.state,'failed');assert.equal(packets.length,2);
});
test('stop and disconnect do not invent completion or send cancellation data',()=>{
  for(const operation of ['stop','disconnect']){
    const {engine,packets,clock}=session();engine.start(identity);engine[operation]();engine.receive(request(1));
    assert.equal(packets.length,2);assert.equal(clock.callbacks.size,0);assert.notEqual(engine.state,'complete');
  }
});
test('unrelated MIDI input does not advance the sender',()=>{
  const {engine,packets}=session();engine.start(identity);engine.receive(boot);engine.receive(Uint8Array.of(0x90,60,100));
  assert.equal(packets.length,2);engine.stop();
});
test('malformed and unsupported request formats stop safely',()=>{
  for(const reply of [Uint8Array.of(0xf0,0,0x20,0x29,0,0x72,1,0xf7),
                      Uint8Array.of(0xf0,0,0x20,0x29,0,0x72,0,0,0,0,0,0,0,1,0xf7)]){
    const {engine,packets}=session();engine.start(identity);engine.receive(reply);
    assert.equal(engine.state,'failed');assert.equal(packets.length,2);
  }
});
test('transport failure during initialization stops before header',()=>{
  let sends=0;const {engine}=session(firmware,()=>{sends++;throw Error('port closed');});engine.start(identity);
  assert.equal(engine.state,'failed');assert.equal(sends,1);
});
test('transport failure during data does not mark that block sent',()=>{
  let sends=0;const {engine}=session(firmware,()=>{if(++sends===3)throw Error('port closed');});engine.start(identity);
  engine.receive(request(1));assert.equal(engine.state,'failed');assert.equal(engine.sent,2);
});
test('request decoder matches captures from the actual Rhythm ARM encoder',()=>{
  const facts=JSON.parse(fs.readFileSync(new URL('build/rhythm-fm/request-encoder-validation.json',root)));
  for(const row of facts.cases)assert.deepEqual(parseReply(new Uint8Array(Buffer.from(row.sysex_hex,'hex'))),{type:'request',block:row.requested_block});
});
test('runtime FM report decodes unsigned counters and rejects truncated or non-MIDI data',()=>{
  const data=new Uint8Array(107);data.set([0xf0,0x7d,0x52,0x46,0x4d,1]);data[106]=0xf7;
  const values=[3,9,0xffffffff,100,200,75,0x7fffff,0x800000,0x464d31,0xffffff,1,32,1000,5,7,8,0x1003c64,0,42,0];
  for(let i=0;i<20;i++)for(let k=0;k<5;k++)data[6+5*i+k]=Math.floor(values[i]/2**(7*k))&127;
  const actual=parseFMStatus(data);
  assert.equal(actual.notes1,3);assert.equal(actual.notes2,9);assert.equal(actual.services,0xffffffff);
  assert.equal(actual.dspMagic,0x464d31);assert.equal(actual.dspFrames,32);assert.equal(actual.pending1,0x1003c64);
  assert.equal(parseFMStatus(boot),null);
  assert.throws(()=>parseFMStatus(data.subarray(0,106)),/Malformed/);
  const corrupt=data.slice();corrupt[10]=0x80;assert.throws(()=>parseFMStatus(corrupt),/Malformed/);
});

test('version 2 runtime report includes accepted DSP audio and burst errors',()=>{
  const data=new Uint8Array(132);data.set([0xf0,0x7d,0x52,0x46,0x4d,2]);data[131]=0xf7;
  const values=[3000,0x7fffff,0x800001,0,2999];
  for(let i=0;i<5;i++)for(let k=0;k<5;k++)data[106+5*i+k]=Math.floor(values[i]/2**(7*k))&127;
  const status=parseFMStatus(data);
  assert.equal(status.dspCopies,3000);assert.equal(status.dspSample1,0x7fffff);
  assert.equal(status.dspSample2,0x800001);assert.equal(status.bulkErrors,0);assert.equal(status.dspNonzeroCopies,2999);
  assert.throws(()=>parseFMStatus(data.subarray(0,131)),/Malformed/);
});

test('version 23 report from the actual ARM identity handler decodes the generator state',()=>{
  const facts=JSON.parse(fs.readFileSync(new URL('build/rhythm-fm/runtime-report-validation.json',root)));
  const data=new Uint8Array(Buffer.from(facts.report_hex,'hex'));
  assert.equal(data.length,272);
  const status=parseFMStatus(data);
  assert.equal(status.generated,facts.generated);
  assert.equal(status.lastSeed,facts.seed);
  assert.ok(status.lastGenerated.startsWith(GEN_FAMILIES[facts.family]));
  for(const name of ['services','blocks','auditionBlocks','dspMisses','avgRenderCycles','maxCallCycles','timerBacklogA','plockLocks','plockRouterFound','lastPadPress'])assert.ok(name in status,name);
  assert.equal(V14_FIELDS.length,32);assert.equal(V15_FIELDS.length,35);assert.equal(V16_FIELDS.length,37);assert.equal(V17_FIELDS.length,40);assert.equal(V18_FIELDS.length,42);assert.equal(V19_FIELDS.length,45);assert.equal(V20_FIELDS.length,48);assert.equal(V21_FIELDS.length,50);assert.equal(V22_FIELDS.length,51);assert.equal(V23_FIELDS.length,53);assert.ok(status.fxHits===0 && status.fxState.startsWith('free'));assert.ok(status.locksDirect===0);assert.ok(status.locksEarly===0 && status.locksSwept===0);assert.ok(status.lastStepLock==='none' && status.stepUnlocks===0 && status.stepState.includes('Record off'));assert.ok(status.lastSpice==='none' && 'spicePlayers' in status && status.spiceRefusals===0);assert.ok('kitKnown' in status && 'kitFocus' in status);assert.ok('kits' in status && 'kitClears' in status && status.kitProgress==='idle');assert.ok('storeRefusals' in status && 'modes' in status);
  for(const name of ['commits','commitErrors','lastStored','storeStep'])assert.ok(name in status,name);
  assert.throws(()=>parseFMStatus(data.subarray(0,271)),/Malformed/);
});
