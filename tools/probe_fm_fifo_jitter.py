"""Interleave real ARM producers and DSP consumers across UI-service gaps.

Service pauses and a mid-DMA DSP tick are explicit fixtures, not measured
hardware timing. No MIDI, device access, or installations.
"""
import hashlib
import json
import re
import struct
import subprocess
import zipfile
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_LR,UC_ARM_REG_PC
from validate_rhythm_fm import Machine,ROOT,OUT,TOOL,CTRL,MAGIC,PBASE
from rhythm_dsp_probe import unpack_stream,data_lines
from rhythm_fm_version import VERSION,STEM,OUT_TRACKS,SLOT_STRIDE,SLOT_COUNT,SLOT_BASE,SEQ_WRAP,TRACK_SHIFT
from build_rhythm_fm import CTRL_READ_SLOT
from rhythm_mix_probe import put
AUDITION=2   # current release: Shift + track 3 (hi-hat/cymbal) streams to track 3


class Stream(Machine):
    def __init__(self,image,symbols,fifo,initial=0,partial=False,tracks=2,stride=128):
        self.fifo,self.initial,self.partial=fifo,initial,partial
        self.tracks,self.stride=tracks,stride
        self.current=tracks==OUT_TRACKS
        self.wrap=SEQ_WRAP if self.current else 1<<24
        self.trace,self.frames,self.accepted=[],[],[]
        super().__init__(image,symbols)

    def hook(self,cpu,address,size,context):
        if address==0x08031228:
            target=cpu.reg_read(UC_ARM_REG_R2)
            value=(cpu.reg_read(UC_ARM_REG_R1)>>8)&0xffffff
            if target==CTRL+1 and value==0:value=self.initial
            if self.current and target==CTRL_READ_SLOT:value=SLOT_BASE+SLOT_STRIDE*(self.initial%SLOT_COUNT)
            self.memory[target]=value
            self.bus.append(('write',target,value))
            self.trace.append(('write',target,value))
            cpu.reg_write(UC_ARM_REG_R0,0)
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
            return
        if address==0x080315E6:
            raw=bytes(cpu.mem_read(cpu.reg_read(UC_ARM_REG_R1),cpu.reg_read(UC_ARM_REG_R2)))
            for i in range(0,len(raw),3):
                target=self.write_pointer+i//3
                value=int.from_bytes(raw[i:i+3],'big')
                self.memory[target]=value
                self.bus.append(('write',target,value))
                self.trace.append(('write',target,value))
                if self.partial and i==len(raw)//2-3:
                    self.partial=False;self.consume()
            cpu.reg_write(UC_ARM_REG_R0,0)
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
            return
        return super().hook(cpu,address,size,context)

    def consume(self):
        seq=self.memory.get(CTRL+1,0)
        slot=(SLOT_BASE+SLOT_STRIDE*(seq%SLOT_COUNT)) if self.current else 0x37800+self.stride*(seq&1)
        ready=self.memory.get(slot,0)==1
        tag=self.memory.get(slot+1,0)
        diff=(tag-seq)%self.wrap
        length=self.memory.get(slot+2,0)
        # Current format: length word = 32 | audition track << 6, 32 samples added to that track.
        valid=ready and not diff and (length&63 if self.current else length)==32
        n=32*self.tracks
        payload=[0]*n
        if valid and self.current:
            t=length>>TRACK_SHIFT&7
            payload[32*t:32*t+32]=[self.memory.get(slot+3+i,0) for i in range(32)]
        elif valid: payload=[self.memory.get(slot+3+i,0) for i in range(n)]
        bank=len(self.frames)&1
        self.trace.append(('consume',seq,bank))
        self.frames.append((seq,bank,valid,payload))
        if ready and (not diff or diff>=self.wrap//2):self.memory[slot]=0
        if valid or not self.fifo:self.memory[CTRL+1]=(seq+1)%self.wrap
        self.memory[CTRL+4]=1;self.memory[CTRL+5]=32
        self.memory[CTRL+6]=self.memory.get(CTRL+6,0)+1
        if valid:self.accepted.append(seq)
        else:self.memory[CTRL+2]=self.memory.get(CTRL+2,0)+1


def run_case(label,image,manifest,fifo,pause,initial=0,partial=False):
    symbols={k:int(v,16) for k,v in manifest['arm_symbols'].items()}
    tracks,stride=(OUT_TRACKS,SLOT_STRIDE) if label==VERSION else (2,128)
    arm=Stream(image,symbols,fifo,initial,partial,tracks,stride)
    arm.run(0x08025688)
    if label==VERSION: arm.run(symbols['fm_randomize_request'],AUDITION,0)   # router 0: no view check
    else:
        for track in range(tracks):arm.run(0x08040874,track,60+track,100)
    for _ in range(3):arm.run(symbols['fm_tick'])
    for frame in range(32):
        if not 8<=frame<8+pause:arm.run(symbols['fm_tick'])
        arm.consume()
    maps,_,_,_=unpack_stream(image)
    folder=OUT/'fifo-jitter';folder.mkdir(exist_ok=True)
    name=f'{label}-pause{pause}-start{initial:x}-partial{int(partial)}'
    program=folder/(name+'.bin')
    program.write_bytes(b''.join(maps['P'].get(a,0).to_bytes(3,'big') for a in range(max(maps['P'])+1)))
    current=label==VERSION
    lines=['CORE1',f'LOADP {program} 0']+data_lines(maps)+['BRIDGE 30000','MAX 400000' if current else 'MAX 10000','X 1 20']
    for op,a,b in arm.trace:
        if op=='write':lines.append(f'Y {a:x} {b:06x}')
        else:lines += ([put('Y',0x304F0,[0]*(64*tracks))] if current else [])+[f'IOY ffffd0 {b}',f'PC {PBASE+2:x}','STOP 8a6','RUN',f'DUMPY 304f0 {64*tracks}',f'DUMPY {CTRL:x} 11']
    path=folder/(name+'.txt');path.write_text('\n'.join(lines)+'\n')
    output=subprocess.run([str(TOOL),'run',str(path)],capture_output=True,text=True,check=True).stdout
    blocks=[[int(x,16) for x in row.split()] for row in re.findall(r'^Y 0304f0:((?: [0-9a-f]{6})+)$',output,re.M)]
    controls=[[int(x,16) for x in row.split()] for row in re.findall(r'^Y 037700:((?: [0-9a-f]{6})+)$',output,re.M)]
    assert len(blocks)==len(arm.frames)==len(controls)
    # Current release (the spans are cleared before each block, as the native pass does): the consumer adds the block to its track's span (the
    # spans start at zero here; the native pass is not run before PBASE+2).
    expected=[payload for _,_,_,payload in arm.frames]
    for actual,control,want,(seq,bank,valid,payload) in zip(blocks,controls,expected,arm.frames):
        assert sum((actual[64*t+32*bank:64*t+32*bank+32] for t in range(tracks)),[])==want
        assert control[1]==((seq+1)%arm.wrap if valid or not fifo else seq)
    if fifo:
        assert arm.accepted==[(initial+i)%arm.wrap for i in range(len(arm.accepted))], 'FIFO discarded rendered blocks'
        assert controls[-1][3]==0,'FIFO dropped a committed block'
    gaps=sum(not valid for _,_,valid,_ in arm.frames[4:])
    return {'image':label,'service_pause_frames':pause,'initial_sequence':initial,
            'mid_payload_tick':partial,'dsp_frames':len(arm.frames),'gaps_after_warmup':gaps,
            'fm_tracks':tracks,'slot_stride':stride,
            'accepted_blocks':len(arm.accepted),'accepted_sequences':arm.accepted,
            'payload_output_bit_exact':True,'drops':controls[-1][3]}


def main():
    with zipfile.ZipFile(ROOT/'dist/rhythm-fm-0.1.6.zip') as archive:
        previous=archive.read('rhythm-fm-0.1.6/rhythm-fm-0.1.6.bin')
        old_manifest=json.loads(archive.read('rhythm-fm-0.1.6/manifest.json'))
    assert hashlib.sha256(previous).hexdigest()=='4a0e54c172e041ca635a3d6196adae9bebf494c8b47954d3c1caa82254b0cc5d'
    image=(OUT/f'{STEM}.bin').read_bytes();manifest=json.loads((OUT/'manifest.json').read_text())
    assert hashlib.sha256(image).hexdigest()==manifest['image_sha256']
    cases=[]
    for pause in (0,2,4,6,10):
        for label,blob,info,fifo in [('0.1.6',previous,old_manifest,False),(VERSION,image,manifest,True)]:
            cases.append(run_case(label,blob,info,fifo,pause))
    for pause in (2,4,6):
        old=next(c for c in cases if c['image']=='0.1.6' and c['service_pause_frames']==pause)
        new=next(c for c in cases if c['image']==VERSION and c['service_pause_frames']==pause)
        assert old['gaps_after_warmup']>0 and new['gaps_after_warmup']==0
    cases.append(run_case(VERSION,image,manifest,True,4,SEQ_WRAP-4))
    cases.append(run_case(VERSION,image,manifest,True,4,0,True))
    report={'image_sha256':manifest['image_sha256'],'scope':__doc__.strip(),'hardware_access':False,'cases':cases}
    (OUT/'fifo-jitter-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='cases'}))
    for c in cases:print(json.dumps({k:v for k,v in c.items() if k!='accepted_sequences'}))


if __name__=='__main__':main()
