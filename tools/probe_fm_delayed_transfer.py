"""Interleave actual ARM producer writes and actual DSP consumer execution.

The DSP clock advances every N native write/transfer calls (an explicit latency fixture,
not measured hardware timing). This reproduces a missed deadline that the old
all-writes-before-consumption tests could not expose. No device access.
"""
import json
import hashlib
import re
import struct
import subprocess
from pathlib import Path
import zipfile

from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2
from validate_rhythm_fm import Machine, ROOT, OUT, TOOL, CTRL, MAGIC, PBASE
# Historical 0.1.4-0.1.6 slot layout (two slots, 128-word stride), not the current FIFO.
SLOTS = (0x37800, 0x37880)
from rhythm_dsp_probe import unpack_stream, data_lines


class ClockedMachine(Machine):
    def __init__(self, image, symbols, period, future_safe, render_ticks=0):
        self.period, self.future_safe = period, future_safe
        self.render_ticks, self.sequence_reads = render_ticks, 0
        self.trace, self.writes, self.consumed = [], 0, []
        super().__init__(image, symbols)

    def consume(self):
        sequence = self.memory.get(CTRL+1, 0)
        slot = SLOTS[sequence & 1]
        ready = self.memory.get(slot, 0) == 1
        tag = self.memory.get(slot+1, 0)
        diff = (tag-sequence) & 0xffffff
        valid = ready and not diff and self.memory.get(slot+2, 0) == 32
        self.trace.append(('consume', sequence, sequence & 1))
        self.consumed.append(valid)
        # This small scheduling model only supplies ARM readback fixtures. The
        # resulting writes/ticks are independently replayed through real DSP
        # instructions below, including payload output and slot ownership.
        if ready and (not self.future_safe or not diff or diff & 0x800000):
            self.memory[slot] = 0
        self.memory[CTRL+1] = (sequence+1) & 0xffffff
        self.memory[CTRL+4] = 1
        self.memory[CTRL+5] = 32
        self.memory[CTRL+6] = self.memory.get(CTRL+6, 0)+1
        if not valid:self.memory[CTRL+2] = self.memory.get(CTRL+2, 0)+1

    def hook(self, cpu, address, size, context):
        if address == 0x080314D6 and cpu.reg_read(UC_ARM_REG_R1) == CTRL+1:
            self.sequence_reads += 1
            # The second sequence read in the burst producer occurs after
            # rendering. Explicitly advance DSP frames during that render.
            if self.sequence_reads == 2:
                for _ in range(self.render_ticks):self.consume()
        if address not in (0x08031228,0x080315E6):
            return super().hook(cpu, address, size, context)
        if address == 0x08031228:
            transactions=[(cpu.reg_read(UC_ARM_REG_R2),(cpu.reg_read(UC_ARM_REG_R1)>>8)&0xffffff)]
        else:
            raw=bytes(cpu.mem_read(cpu.reg_read(UC_ARM_REG_R1),cpu.reg_read(UC_ARM_REG_R2)))
            transactions=[(self.write_pointer+i//3,int.from_bytes(raw[i:i+3],'big')) for i in range(0,len(raw),3)]
        super().hook(cpu, address, size, context)
        for target,value in transactions:self.trace.append(('write',target,value))
        # One fixture time unit per native transfer call, not per payload word.
        # This compares transaction overhead; it is not a measured SPI clock.
        self.writes += 1
        if self.writes % self.period == 0:self.consume()


def run_case(label, image, symbols, period, future_safe, render_ticks=0):
    machine = ClockedMachine(image, symbols, period, future_safe, render_ticks)
    machine.run(0x08025688)
    machine.run(0x08040874, 0, 60, 100)
    machine.run(0x08040874, 1, 61, 100)
    machine.run(symbols['fm_tick'])
    for _ in range(4):machine.consume()
    maps, _, _, _ = unpack_stream(image)
    folder = OUT / 'delayed-transfer'
    folder.mkdir(exist_ok=True)
    program = folder / (label+'-P.bin')
    program.write_bytes(b''.join(maps['P'].get(a,0).to_bytes(3,'big') for a in range(max(maps['P'])+1)))
    lines = ['CORE1',f'LOADP {program} 0']+data_lines(maps)+['BRIDGE 30000','MAX 10000','X 1 20']
    for operation, a, b in machine.trace:
        if operation == 'write':lines += [f'Y {a:x} {b:06x}']
        else:
            lines += [f'IOY ffffd0 {b}',f'PC {PBASE+2:x}','STOP 8a6','RUN',
                      'DUMPY 304f0 128',f'DUMPY {CTRL:x} 7']
    path = folder / f'{label}-{period}.txt'
    path.write_text('\n'.join(lines)+'\n')
    output = subprocess.run([str(TOOL),'run',str(path)],capture_output=True,text=True,check=True).stdout
    blocks = [[int(v,16) for v in row.split()] for row in re.findall(r'^Y 0304f0:((?: [0-9a-f]{6})+)$',output,re.M)]
    controls = [[int(v,16) for v in row.split()] for row in re.findall(r'^Y 037700:((?: [0-9a-f]{6})+)$',output,re.M)]
    frames = [item for item in machine.trace if item[0]=='consume']
    if len(blocks)!=len(frames) or len(controls)!=len(frames):raise RuntimeError('DSP replay did not complete every scheduled frame')
    audible = []
    for output_words, control, (_, sequence, bank), expected in zip(blocks, controls, frames, machine.consumed):
        active = output_words[32*bank:32*bank+32]+output_words[64+32*bank:96+32*bank]
        audible.append(any(active))
        if audible[-1] != expected or control[1] != (sequence+1)&0xffffff:
            raise RuntimeError('Actual DSP replay differs from the scheduling fixture')
    return dict(image=label, writes_per_dsp_tick=period, producer_writes=machine.writes, render_delay_frames=render_ticks,
                dsp_frames=len(frames), nonzero_frames=sum(audible), frame_audio=audible)


def main():
    with zipfile.ZipFile(ROOT/'dist/rhythm-fm-0.1.4.zip') as archive:
        old=archive.read('rhythm-fm-0.1.4/rhythm-fm-0.1.4.bin')
        manifest=json.loads(archive.read('rhythm-fm-0.1.4/manifest.json'))
    with zipfile.ZipFile(ROOT/'dist/rhythm-fm-0.1.5.zip') as archive:
        previous=archive.read('rhythm-fm-0.1.5/rhythm-fm-0.1.5.bin')
        previous_manifest=json.loads(archive.read('rhythm-fm-0.1.5/manifest.json'))
    if hashlib.sha256(previous).hexdigest()!='66134ec4928d40d37d25c68b7ebafee10e14d62ce71155e24aff3d56328bd864':
        raise ValueError('Wrong preserved 0.1.5 reference image')
    with zipfile.ZipFile(ROOT/'dist/rhythm-fm-0.1.6.zip') as archive:
        current=archive.read('rhythm-fm-0.1.6/rhythm-fm-0.1.6.bin')
        current_manifest=json.loads(archive.read('rhythm-fm-0.1.6/manifest.json'))
    if hashlib.sha256(current).hexdigest()!='4a0e54c172e041ca635a3d6196adae9bebf494c8b47954d3c1caa82254b0cc5d':
        raise ValueError('Wrong immutable 0.1.6 regression input')
    if hashlib.sha256(old).hexdigest()!='ed2f2371a910f23ffbc21c2acfca22de849a18c3ee943035220fdcb68e60804b':
        raise ValueError('Wrong preserved 0.1.4 reference image')
    if hashlib.sha256(current).hexdigest()!=current_manifest['image_sha256']:
        raise ValueError('Current manifest does not match delayed-transfer test image')
    cases=[]
    for label,image,info,future_safe in [('0.1.4',old,manifest,False),('0.1.5',previous,previous_manifest,True),('0.1.6',current,current_manifest,True)]:
        symbols={name:int(value,16) for name,value in info['arm_symbols'].items()}
        for period in (40,64,100):cases.append(run_case(label,image,symbols,period,future_safe))
    for period in (40,64):
        old_case=next(c for c in cases if c['image']=='0.1.4' and c['writes_per_dsp_tick']==period)
        new_case=next(c for c in cases if c['image']=='0.1.5' and c['writes_per_dsp_tick']==period)
        if old_case['nonzero_frames'] or not new_case['nonzero_frames']:
            raise RuntimeError('Delayed-transfer regression did not reproduce and resolve the missed-frame race')
    symbols={name:int(value,16) for name,value in current_manifest['arm_symbols'].items()}
    late=run_case('0.1.6-render-delay',current,symbols,100,True,3)
    if not late['nonzero_frames']:raise RuntimeError('Post-render rescheduling failed')
    cases.append(late)
    for case in cases:
        if case['image']=='0.1.6' and (case['nonzero_frames']!=2 or case['producer_writes']>=30):
            raise RuntimeError('Burst producer did not publish both frames with fewer transfer calls')
    report=dict(image_sha256=json.loads((OUT/'manifest.json').read_text())['image_sha256'],
        historical_reference_sha256=current_manifest['image_sha256'],hardware_access=False,
        scope='Historical 0.1.4/0.1.5/0.1.6 ARM/DSP scheduling regression; current FIFO is tested by fifo-jitter-validation.json. Explicit latency fixtures, not hardware timing.',cases=cases)
    (OUT/'delayed-transfer-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
