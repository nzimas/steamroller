/* SPDX-License-Identifier: GPL-3.0-only
 * Circuit Rhythm updater protocol. No Web MIDI or device access in this module.
 */
const PREFIX = [0xf0, 0x00, 0x20, 0x29, 0x00];
export const IDENTITY_INQUIRY = Uint8Array.of(0xf0,0x7e,0x7f,6,1,0xf7);
const table = Uint32Array.from({length:256}, (_,i) => {
  let c = i << 24;
  for (let k=0;k<8;k++) c = c & 0x80000000 ? (c<<1)^0x04c11db7 : c<<1;
  return c >>> 0;
});
export function crc32(bytes) {
  let c=0xffffffff;
  for (const b of bytes) c=((c<<8)^table[((c>>>24)^b)&255])>>>0;
  return c>>>0;
}
function require(ok,message) { if (!ok) throw new Error(message); }
function nibble(bytes) {
  let value=0;
  for (const b of bytes) { require(b<=15,'Invalid hexadecimal nibble'); value=value*16+b; }
  return value;
}
function samePrefix(bytes) { return PREFIX.every((x,i)=>bytes[i]===x); }
function unpack(message) {
  require(message.length===44,'Wrong firmware block length');
  let bits=0n;
  for (const b of message.subarray(6,-1)) bits=(bits<<7n)|BigInt(b);
  require((bits&7n)===0n,'Nonzero block packing bits');
  bits>>=3n;
  const block=new Uint8Array(32);
  for (let i=31;i>=0;i--) { block[i]=Number(bits&255n); bits>>=8n; }
  return block;
}
export async function sha256(bytes) {
  const result=await globalThis.crypto.subtle.digest('SHA-256',bytes);
  return Array.from(new Uint8Array(result),b=>b.toString(16).padStart(2,'0')).join('');
}
export function parseFirmware(input) {
  const bytes=new Uint8Array(input), packets=[];
  for (let start=0;start<bytes.length;) {
    require(bytes[start]===0xf0,'Unexpected bytes outside SysEx');
    const end=bytes.indexOf(0xf7,start+1);
    require(end>=0,'Unterminated SysEx');
    const packet=bytes.slice(start,end+1);
    require(packet.length>=7 && samePrefix(packet),'Wrong manufacturer header');
    require(packet.subarray(1,-1).every(b=>b<128),'Non-MIDI data byte');
    packets.push(packet); start=end+1;
  }
  require(packets.length>=3,'Incomplete firmware');
  const init=packets[0], header=packets[1];
  require(init.length===15 && init[5]===0x71,'Invalid update initialization');
  require(init[6]===1 && init[7]===0x63,'This file is not Circuit Rhythm firmware');
  require(header.length===30 && header[5]===0x7c && header[6]===0,'Invalid update header');
  const protocol=nibble(init.subarray(8,14));
  require(protocol===0x731 && nibble(header.subarray(7,13))===protocol,'Unsupported Rhythm protocol version');
  const size=nibble(header.subarray(13,21)), expectedCRC=nibble(header.subarray(21,29));
  require(size>=0x1ee && size<=0x70000,'Invalid application image size');
  require(size%4===0,'Image size must be word-aligned for the bootloader CRC');
  const blocks=Math.ceil(size/32);
  require(packets.length===blocks+2,'Firmware block count does not match size');
  require(packets.at(-1)[5]===0x73 && packets.slice(2,-1).every(p=>p[5]===0x72),'Wrong block command order');
  const padded=new Uint8Array(blocks*32);
  padded.set(unpack(packets.at(-1)),0);
  for (let i=2;i<packets.length-1;i++) padded.set(unpack(packets[i]),32*(i-1));
  require(padded.subarray(size).every(b=>b===255),'Invalid final-block padding');
  const image=padded.slice(0,size), view=new DataView(image.buffer);
  require(view.getUint32(0,true)===0x20050000,'Unexpected initial stack pointer');
  const reset=view.getUint32(4,true);
  require((reset&1)===1 && reset>=0x08010001 && reset<0x08080000,'Invalid application reset vector');
  require(image[0x1cc]===1 && image[0x1cd]===0x63,'Image identifies a different product');
  require(crc32(image)===expectedCRC,'Application CRC mismatch');
  return {packets,image,size,blocks,crc:expectedCRC,protocol};
}
export async function verifyFirmware(input, approved) {
  const bytes=new Uint8Array(input), hash=await sha256(bytes);
  const entry=approved.find(item=>item.sysex_sha256===hash);
  require(entry,'Unrecognized firmware file: only the packaged candidate and official recovery file are accepted');
  const parsed=parseFirmware(bytes);
  require(parsed.size===entry.size,'Approved file size mismatch');
  require(await sha256(parsed.image)===entry.image_sha256,'Approved image hash mismatch');
  if(entry.role==='candidate') {
    require(entry.installation_marker && String.fromCharCode(...parsed.image.subarray(0x1e8,0x1ee))===entry.installation_marker,
            'Custom installation marker is missing');
    require(parsed.image[0x170d8+14]*128+parsed.image[0x170d8+15]===entry.identity_build,
            'Custom application identity is missing');
  }
  return {...parsed,hash,entry};
}
export function parseIdentity(bytes) {
  if (bytes.length!==17 || bytes[0]!==0xf0 || bytes[1]!==0x7e || bytes[3]!==6 || bytes[4]!==2 || bytes[16]!==0xf7) return null;
  if (!bytes.subarray(1,-1).every(b=>b<128)) return null;
  if (bytes[5]!==0 || bytes[6]!==0x20 || bytes[7]!==0x29 || bytes[9]!==0x63 || bytes[10]!==0 || bytes[11]!==0) return null;
  if (![1,0x11].includes(bytes[8])) return null;
  return {product:0x63,mode:bytes[8]===0x11?'bootloader':'application',build:bytes[14]*128+bytes[15],raw:Array.from(bytes)};
}
export function parseFMStatus(bytes) {
  const header=[0xf0,0x7d,0x52,0x46,0x4d];
  if(!header.every((x,i)=>bytes[i]===x)) return null;
  if(![1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23].includes(bytes[5])) return null;
  const length={1:107,2:132,3:202,4:212,5:217,6:232,7:282,8:277,9:302,10:302,11:317,12:357,13:362,14:167,15:182,16:192,17:207,18:217,19:232,20:247,21:257,22:262,23:272}[bytes[5]];
  require(bytes.length===length && bytes[length-1]===0xf7 && bytes.subarray(1,-1).every(b=>b<128),'Malformed FM runtime report');
  const names=bytes[5]===23?[...V23_FIELDS]:bytes[5]===22?[...V22_FIELDS]:bytes[5]===21?[...V21_FIELDS]:bytes[5]===20?[...V20_FIELDS]:bytes[5]===19?[...V19_FIELDS]:bytes[5]===18?[...V18_FIELDS]:bytes[5]===17?[...V17_FIELDS]:bytes[5]===16?[...V16_FIELDS]:bytes[5]===15?[...V15_FIELDS]:bytes[5]===14?[...V14_FIELDS]:bytes[5]===13?[...V13_FIELDS]:bytes[5]===12?[...V12_FIELDS]:bytes[5]===11?[...V11_FIELDS]:bytes[5]>=9?[...V9_FIELDS]:bytes[5]===8?[...V8_FIELDS]:bytes[5]>=4?[...V4_FIELDS,...(bytes[5]>=5?['voiceSheds']:[]),...(bytes[5]>=6?['minQueued','maxServiceGap','maxCallCycles']:[]),...(bytes[5]>=7?V7_FIELDS:[])]:['notes1','notes2','services','ticks','blocks','nonzeroBlocks','peak1','peak2',
    'dspMagic','dspSequence','dspStatus','dspFrames','dspHeartbeats','dspUnderruns',
    'dspDrops','resyncs','pending1','pending2','nextSequence','busy'];
  if(bytes[5]===2||bytes[5]===3) names.push('dspCopies','dspSample1','dspSample2','bulkErrors','dspNonzeroCopies');
  if(bytes[5]===3) names.push('notes3','notes4','peak3','peak4','pending3','pending4','dspSample3','dspSample4',
    'randomPatches','seed1','seed2','seed3','seed4','families');
  const status=Object.fromEntries(names.map((name,i)=>{
    let value=0;for(let k=0;k<5;k++)value+=bytes[6+5*i+k]*2**(7*k);
    require(value<=0xffffffff,'FM runtime value exceeds 32 bits');return [name,value];
  }));
  if(bytes[5]>=15) return v15(status);
  if(bytes[5]===14) return v14(status);
  if(bytes[5]>=12) return v12(status);
  if(bytes[5]===11) return v11(status);
  if(bytes[5]>=9) return v9(status,bytes[5]===10);
  if(bytes[5]===8) return v8(status);
  if(bytes[5]>=3) {
    const packed=status.families;delete status.families;
    for(let t=0;t<4;t++) {
      const family=Math.floor(packed/2**(8*t))%256;
      const lane=bytes[5]>=4&&family!==255?(family&128?' (rhythmic noise)':' (dark ambient)'):'';
      status['family'+(t+1)]=family===255?'boot default':(FM_FAMILIES[bytes[5]>=4?family&127:family]??'unknown '+family)+lane;
    }
  }
  if(bytes[5]>=7) {
    const roles=status.drumRoles;delete status.drumRoles;
    for(let t=0;t<2;t++) {
      const role=Math.floor(roles/2**(16*t))%256, kind=Math.floor(roles/2**(16*t+8))%256;
      status['drum'+(t+5)]=status['drumNotes'+(t+5)]?(DRUM_ROLES[role]??'?')+' / '+(DRUM_KINDS[kind]??'?'):'not played';
    }
    const pad=status.lastPad;delete status.lastPad;
    status.lastPadPress=status.padEvents?`pad ${pad%256}${pad&256?' with Shift':''}`:'none';
  }
  if(bytes[5]>=4) {
    const packed=status.voicesPerTrack;delete status.voicesPerTrack;
    for(let t=0;t<4;t++) status['voices'+(t+1)]=Math.floor(packed/2**(8*t))%256;
  }
  return status;
}
/* Version 4 (Rhythm FM 0.1.9): four FM tracks, shared voice pool, cycle counts. */
export const V4_FIELDS=['notes1','notes2','notes3','notes4','services','ticks','blocks','nonzeroBlocks',
  'peak1','peak2','peak3','peak4','dspMagic','dspSequence','dspStatus','dspFrames','dspHeartbeats',
  'dspUnderruns','dspDrops','resyncs','nextSequence','busy','dspCopies','dspSample1','dspSample2',
  'dspSample3','dspSample4','bulkErrors','dspNonzeroCopies','randomPatches','seed1','seed2','seed3','seed4',
  'families','voicesPerTrack','voiceSteals','eventOverflows','maxVoices','blockCycles','maxBlockCycles'];
/* Tonitor generator families, in fm_generate's numbering (fm_generator.c). */
export const FM_FAMILIES=['METAL','DRONE','BRUIT','IMPACT','ITERATIF','GLISSE','SPECTRE','ABYSS','FRICTION',
  'CHOIR','BOWED','HOLLOW','BLOOM','TIDE','SHARDS','STATIC','WIND','PLUCK','MALLET','WARP','GLITCH','RUBBER'];
export function parseReply(bytes) {
  if (!samePrefix(bytes)) return null;
  require(bytes.at(-1)===0xf7 && bytes.subarray(1,-1).every(b=>b<128),'Malformed update reply');
  if (bytes[5]===0x72) {
    // Rhythm 5706 constructor P:0x0802D0B0 encodes byteOffset >> 5
    // as three little-endian 7-bit values. No guessed legacy reply variants.
    require(bytes.length===10,'Unsupported block-request format; transfer stopped');
    return {type:'request',block:bytes[6]+bytes[7]*128+bytes[8]*16384};
  }
  if (bytes[5]===0x73) {
    require(bytes.length===10,'Unsupported status-reply format');
    return {type:'status',code:bytes[6]};
  }
  return null;
}
export class Transfer {
  constructor({firmware,send,onChange=()=>{},clock=globalThis}) {
    this.firmware=firmware; this.send=send; this.onChange=onChange; this.clock=clock;
    this.state='idle'; this.sent=0; this.nextBlock=1; this.lastRequest=null;
    this.finalSent=false; this.timer=null; this.message='Ready';
  }
  get active() { return ['starting','transferring','verifying'].includes(this.state); }
  snapshot() { return {state:this.state,sent:this.sent,total:this.firmware.packets.length,
                       nextBlock:this.nextBlock,finalSent:this.finalSent,message:this.message}; }
  changed(message) { this.message=message; this.onChange(this.snapshot()); }
  arm(ms) {
    if (this.timer!==null) this.clock.clearTimeout(this.timer);
    this.timer=this.clock.setTimeout(()=>this.fail('Device response timed out; no automatic retry'),ms);
  }
  finish(state,message) {
    if (this.timer!==null) this.clock.clearTimeout(this.timer);
    this.timer=null; this.state=state; this.changed(message);
  }
  fail(message) { this.finish('failed',message); }
  start(identity) {
    require(this.state==='idle','Create a new session before retrying');
    require(identity?.product===0x63 && identity.mode==='bootloader','A verified Rhythm bootloader is required');
    this.state='starting'; this.changed('Sending initialization and header');
    try {
      this.send(this.firmware.packets[0]); this.sent++;
      this.send(this.firmware.packets[1]); this.sent++;
      this.arm(25000); this.changed('Waiting for the first block request');
    } catch(error) { this.fail('MIDI send failed: '+error.message); }
  }
  receive(bytes) {
    if (!this.active) return;
    let reply;
    try { reply=parseReply(bytes); } catch(error) { this.fail(error.message); return; }
    if (!reply) return;
    if (reply.type==='request') {
      if (reply.block===this.lastRequest) return; // duplicate does not advance or replay data
      const expected=this.nextBlock<this.firmware.blocks ? this.nextBlock:0;
      if (this.finalSent || reply.block!==expected) {
        this.fail(`Unexpected block request ${reply.block}; expected ${this.finalSent?'completion status':expected}`); return;
      }
      const packet=reply.block===0 ? this.firmware.packets.at(-1):this.firmware.packets[reply.block+1];
      try { this.send(packet); } catch(error) { this.fail('MIDI send failed: '+error.message); return; }
      this.sent++; this.lastRequest=reply.block;
      if (reply.block===0) {
        this.finalSent=true; this.state='verifying'; this.arm(60000);
        this.changed('All blocks sent; waiting for device verification. Keep power connected.');
      } else {
        this.nextBlock++; this.state='transferring'; this.arm(10000);
        this.changed(`Sent block ${reply.block} of ${this.firmware.blocks-1}`);
      }
    } else if (reply.code===127) {
      if (!this.finalSent || this.sent!==this.firmware.packets.length) this.fail('Premature success status; transfer was incomplete');
      else this.finish('complete','Device reported successful receipt. Keep power connected while it restarts.');
    } else if ([16,17,124,126].includes(reply.code)) {
      const reasons={16:'Cancelled on the device',17:'Device confirmation timed out',124:'Device rejected the image',126:'Device reported a CRC error'};
      this.fail(reasons[reply.code]+'; no more data will be sent');
    } else if (reply.code===125) {
      this.arm(60000); this.changed('Device is processing the received image; completion is not confirmed');
    } else if (reply.code===0 || reply.code===1) {
      this.arm(25000);
      this.changed(reply.code===1?'Device requests confirmation: press Play if prompted on the unit':'Device acknowledged update initialization');
    } else this.fail('Unknown device status '+reply.code+'; no more data will be sent');
  }
  stop() { if (this.active) this.finish('stopped','Stopped sending. This does not undo bytes already sent. Keep power connected.'); }
  disconnect() {
    if (this.active) this.finish('unconfirmed','MIDI disconnected; completion was not confirmed. Wait for restart, then check the device.');
  }
}

/* Version 7 (Rhythm FM 0.1.12): tracks 5-6 poundhard drums and pad presses. */
export const V7_FIELDS=['drumNotes5','drumNotes6','drumPeak5','drumPeak6','dspSample5','dspSample6',
  'drumRoles','drumRandoms','padEvents','lastPad'];
export const DRUM_ROLES=['kick','snare','hi-hat','clap','tom','metal','noise','glitch'];
export const DRUM_KINDS=['kick','snare','hihat','clap','tom','clang','metal','anvil','rust','zap','glitch'];

/* Version 8 (Rhythm FM 0.1.13): FM tracks 1-3 (mono), poundhard drums 4-6. */
export const V8_FIELDS=['notes1','notes2','notes3','notes4','notes5','notes6','services','ticks','blocks','nonzeroBlocks',
  'peak1','peak2','peak3','peak4','peak5','peak6','dspMagic','dspSequence','dspStatus','dspFrames','dspHeartbeats',
  'dspUnderruns','dspDrops','resyncs','nextSequence','busy','dspCopies','dspSample1','dspSample2','dspSample3',
  'dspSample4','dspSample5','dspSample6','bulkErrors','dspNonzeroCopies','randomPatches','seed1','seed2','seed3',
  'families','voices','eventOverflows','blockCycles','maxBlockCycles','voiceSheds','minQueued','maxServiceGap',
  'maxCallCycles','drumRoles','drumRandoms','padEvents','lastPad','maxVoices','voiceSteals'];
/* Version 9 (Rhythm FM 0.1.14): version 8 plus average render and transfer
   cycles per block, the longest transfer, average drum render cycles, and the
   stock timer counters' largest backlog (all since the previous report). */
export const V9_FIELDS=[...V8_FIELDS,'avgRenderCycles','avgTransferCycles','maxTransferCycles','avgDrumCycles','timerBacklog'];
/* Version 10 (0.1.15): as 9, but each drum track's byte is role (0-15; 8-15
   the gnarly row) | kind << 4. */
/* Version 11 (0.1.16): version 10 plus parameter locks: locks sent, reverts
   (low 20 bits) and Shift + macro mixer/FX edits (above), and the last lock
   (value, lane, step; bit 24 = knob router found). */
export const V11_FIELDS=[...V9_FIELDS,'plockLocks','plockReverts','plockLast'];
const PLOCK_LANES=['macro 1','macro 2','macro 3','macro 4','macro 5','macro 6','macro 7','macro 8','mixer A','mixer B','mixer C','mixer D'];
/* Version 12 (0.1.20, no sampler): drums on tracks 4-8 sharing three voices.
   Adds notes7/8, peak7/8, dspSample7/8, the role/kind byte of drum track 8 and
   drumSteals (hits that took a voice from another drum track). */
export const V12_FIELDS=[...V11_FIELDS,'notes7','notes8','peak7','peak8','dspSample7','dspSample8','drumRole8','drumSteals'];
/* Version 13 (0.1.21): adds plockMixerLanes, the tracks whose old Mixer/FX
   lane data (level, pan, sends) was ignored because macros 1-4 own them. */
export const V13_FIELDS=[...V12_FIELDS,'plockMixerLanes'];
/* Version 14 (0.2.x): the stock sampler plus the poundhard sound generator
   (Shift + track button 1-8) and parameter locks. */
export const GEN_FAMILIES=['kick','snare','hi-hat/cymbal','metal','noise','glitch/click','impact','gnarly'];
export const V14_FIELDS=['services','ticks','blocks','auditionBlocks','dspMagic','dspSequence','dspStatus','dspFrames',
  'dspHeartbeats','dspMisses','dspDrops','resyncs','nextSequence','dspCopies','dspNonzeroCopies','bulkErrors',
  'generated','lastSound','lastSeed','auditionPeak','avgRenderCycles','maxBlockCycles','maxCallCycles','maxServiceGap',
  'minQueued','timerBacklog','avgTransferCycles','plockLocks','plockReverts','plockLast','padEvents','lastPad'];
/* Version 15 (0.2.1): v14 + generated sounds stored into sample slots. */
export const V15_FIELDS=[...V14_FIELDS,'commits','commitErrors','lastCommit'];
/* Version 16 (0.2.2): v15 + refused stores and the two-pad modes. */
export const V16_FIELDS=[...V15_FIELDS,'storeRefusals','modes'];
/* Version 17 (0.2.3): v16 + kits made, kit clears and kit progress. */
export const V17_FIELDS=[...V16_FIELDS,'kits','kitClears','kitProgress'];
/* Version 18 (0.2.6): the last kit page's census and focus. */
export const V18_FIELDS=[...V17_FIELDS,'kitKnown','kitFocus'];
/* Version 19 (0.2.7): Shift + track "spice" (random locks shaped by the pattern). */
export const V19_FIELDS=[...V18_FIELDS,'spices','spiceLast','spiceRefusals'];
/* Version 20 (0.2.8): per-step level/pan/delay/reverb sliders on the bottom rows (Record + held step). */
export const V20_FIELDS=[...V19_FIELDS,'stepLocks','stepLast','stepState'];
/* Version 21 (0.2.10): locks sent ahead of their hit, and locks erased from removed steps. */
export const V21_FIELDS=[...V20_FIELDS,'locksEarly','locksSwept'];
/* Version 22 (0.2.11): lock values written straight to the DSP. */
export const V22_FIELDS=[...V21_FIELDS,'locksDirect'];
/* Version 23 (0.3.0): per-step Grid FX locks on the FX bus. */
export const V23_FIELDS=[...V22_FIELDS,'fxHits','fxState'];
const GRID_FX=['looper','reverser','stutter','phaser','bit crusher','filter','vinyl'];
const STEP_LANES={8:'reverb send',9:'delay send',10:'level',11:'pan'};
const SPICE_PARAMS=['distortion','HPF','LPF','resonance','reverb send','delay send','level','pan'];
const KIT_CATEGORIES=['kick','gnarly kick','snare','gnarly snare','clap','impact','hat','cymbal','metal','gnarly metal','noise','click','destroyed'];
const COMMIT_STEPS=['idle','deleting old file','writing','closing'];
const COMMIT_ERRORS=['','delete failed','write failed','close failed','storage did not answer','scratch buffer busy','clear delete failed'];
function v15(status) {
  const c=status.lastCommit;delete status.lastCommit;
  const error=Math.floor(c/2**24)%256;
  status.lastStored=status.commits||status.commitErrors?(error?`slot ${(c%256)+1}: ${COMMIT_ERRORS[error]??'error '+error}`:`slot ${(c%256)+1} from track ${(Math.floor(c/256)%256)+1}`):'none';
  status.storeStep=COMMIT_STEPS[Math.floor(c/65536)%256]??'?';
  if('modes' in status){const m=status.modes;status.modes=m&1?'generate':m&2?'commit':'off';}
  if('kitFocus' in status){const f=status.kitFocus;status.kitFocus=f===0xffffffff?(status.kits?'starter page':'none'):[0,1,2,3].map(i=>Math.floor(f/2**(8*i))%256).filter(c=>c<13).map(c=>KIT_CATEGORIES[c]).join(', ');}
  if('fxState' in status){
    const h=status.fxHits,st=status.fxState;
    status.fxHits=h%65536;status.fxClaims=Math.floor(h/65536);status.fxReleases=Math.floor(st/65536);
    const owner=st%256,active=Math.floor(st/256)%256;
    status.fxState=owner?`track ${owner} on the FX bus: ${GRID_FX.filter((_,k)=>active>>k&1).join(', ')||'no effect'}`:'free (every track through the Grid FX, as stock)';
  }
  if('stepLast' in status){
    const n=status.stepLocks,v=status.stepLast,st=status.stepState;delete status.stepLast;
    status.stepLocks=n%65536;status.stepUnlocks=Math.floor(n/65536);
    status.lastStepLock=status.stepLocks||status.stepUnlocks?`track ${((v>>20)&7)+1} step ${((v>>12)&31)+1}: ${((v>>8)&255)>=32?`${GRID_FX[((v>>8)&255)-32]??'?'} preset ${(v&255)+1}`:`${STEP_LANES[(v>>8)&15]??'?'} = ${v&255}`}`:'none';
    status.stepState=`${st&1?'transport found':'transport not seen yet (press Record once)'}; Record ${st&2?'on':'off'}; rows ${['level/pan','pan/delay','delay/reverb'][(st>>4)&3]??'?'}`;
  }
  if('spiceLast' in status){
    const v=status.spiceLast,r=status.spiceRefusals;delete status.spiceLast;
    status.spiceRefusals=r%256;status.spicePlayers=[0,1,2,3,4,5,6,7].filter(t=>Math.floor(r/256)>>t&1).map(t=>t+1).join(',')||'none yet (press Play once)';
    const palette=Math.floor(v/256)%256;
    status.lastSpice=status.spices?`track ${(v&7)+1} pattern ${((v>>3)&7)+1}${(v>>6)&1?' (re-roll)':''}: ${Math.floor(v/65536)%256} locks on ${SPICE_PARAMS.filter((_,k)=>palette>>k&1).join(', ')||'nothing'}; period ${Math.floor(v/2**24)}`:'none';
  }
  if('kitProgress' in status){const k=status.kitProgress;const st=k%256;status.kitProgress=st===1?`making: sound ${(Math.floor(k/256)%256)+1} of 32 from slot ${Math.floor(k/65536)+1}`:st===2?`clearing: slot ${Math.floor(k/65536)+(Math.floor(k/256)%256)+1}`:'idle';}
  return v14(status);
}
function v14(status) {
  const sound=status.lastSound;delete status.lastSound;
  const family=sound%256, pad=Math.floor(sound/256)%256, kind=Math.floor(sound/65536)%256;
  status.lastGenerated=status.generated?`${GEN_FAMILIES[family]??'?'}: ${(pad&15)>=8?'gnarly ':''}${DRUM_ROLES[pad&7]??'?'} / ${DRUM_KINDS[kind]??'?'}`:'none';
  const backlog=status.timerBacklog;delete status.timerBacklog;
  status.timerBacklogA=backlog%256;status.timerBacklogB=Math.floor(backlog/256)%256;
  const last=status.plockLast;delete status.plockLast;
  status.plockRouterFound=Math.floor(last/2**24)&1?'yes':'no';
  status.plockLastLock=last&2**20?`lane ${((last>>8)&15)+1} = ${last&255} on step ${((last>>12)&255)+1}`:'none';
  const press=status.lastPad;delete status.lastPad;
  status.lastPadPress=status.padEvents?`pad ${press%256}${press&256?' with Shift':''}`:'none';
  return status;
}
function v12(status) {
  if('plockMixerLanes' in status){const m=status.plockMixerLanes;status.plockMixerLanes=m?[0,1,2,3,4,5,6,7].filter(t=>m>>t&1).map(t=>t+1).join(', '):'none';}
  const b=status.drumRole8;delete status.drumRole8;
  const voices=status.voices;
  const out=v11(status);
  out['drumVoice7']=Math.floor(voices/2**24)>>3&1?'sounding':'silent';
  out['drumVoice8']=Math.floor(voices/2**24)>>4&1?'sounding':'silent';
  const roles=out.drumRoles7;delete out.drumRoles7;
  out['drum7']=out.notes7?(roles??'?'):'not played';
  out['drum8']=out.notes8?((b&15)>=8?'gnarly ':'')+(DRUM_ROLES[b&7]??'?')+' / '+(DRUM_KINDS[b>>4]??'?'):'not played';
  return out;
}
function v11(status) {
  const reverts=status.plockReverts;status.plockReverts=reverts%2**20;status.plockMixerEdits=Math.floor(reverts/2**20);
  const last=status.plockLast;delete status.plockLast;
  status.plockRouterFound=Math.floor(last/2**24)&1?'yes':'no';
  status.plockLastLock=last&2**20?`${PLOCK_LANES[(last>>8)&15]??'?'} = ${last&255} on step ${((last>>12)&255)+1}`:'none';
  return v9(status,true);
}
function v9(status,wide=false) {
  const backlog=status.timerBacklog;delete status.timerBacklog;
  status.timerBacklogA=backlog%256;status.timerBacklogB=Math.floor(backlog/256)%256;
  return v8(status,wide);
}
function v8(status,wide=false) {
  const families=status.families;delete status.families;
  for(let t=0;t<3;t++) {
    const f=Math.floor(families/2**(8*t))%256;
    status['family'+(t+1)]=f===255?'boot default':(FM_FAMILIES[f&127]??'unknown '+f)+(f&128?' (rhythmic noise)':' (dark ambient)');
  }
  const voices=status.voices;delete status.voices;
  for(let t=0;t<3;t++) status['fmVoice'+(t+1)]=Math.floor(voices/2**(8*t))%256?'sounding':'silent';
  for(let t=0;t<3;t++) status['drumVoice'+(t+4)]=Math.floor(voices/2**24)>>t&1?'sounding':'silent';
  const roles=status.drumRoles;delete status.drumRoles;
  if(wide&&'notes7' in status){const b=Math.floor(roles/2**24)%256;status.drumRoles7=((b&15)>=8?'gnarly ':'')+(DRUM_ROLES[b&7]??'?')+' / '+(DRUM_KINDS[b>>4]??'?');}
  for(let t=0;t<3;t++) {
    const b=Math.floor(roles/2**(8*t))%256;
    const role=wide?b&15:b&7, kind=wide?b>>4:b>>3;
    status['drum'+(t+4)]=status['notes'+(t+4)]?(role>=8?'gnarly ':'')+(DRUM_ROLES[role&7]??'?')+' / '+(DRUM_KINDS[kind]??'?'):'not played';
  }
  const pad=status.lastPad;delete status.lastPad;
  status.lastPadPress=status.padEvents?`pad ${pad%256}${pad&256?' with Shift':''}`:'none';
  return status;
}
