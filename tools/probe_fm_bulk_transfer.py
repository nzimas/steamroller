"""Execute linked ARM burst code and native SPI helpers, then DSP receivers.

Only GPIO/DMA/mutex endpoints are fixtures. No hardware access or timing claims.
"""
import hashlib
import json
import struct
import subprocess
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_LR, UC_ARM_REG_PC
from build_rhythm_fm import ROOT, OUT, BASE, elf
from rhythm_transfer_probe import Probe
from rhythm_receiver_probe import receive, execute, put, OUT as RECEIVER_OUT
from rhythm_dsp_probe import unpack_stream, data_lines
from rhythm_fm_version import STEM, BLOCK_WORDS
TX_WORDS = 170   # fm_write_block's DMA chunk: 510 of the native TX buffer's 512 bytes

TX, RX, SOURCE = 0x200058B8, 0x20005AB8, 0x3000


class BurstProbe(Probe):
    def __init__(self, image):
        super().__init__(image)
        self.cpu.mem_map(0, 0x4000)

    def hook(self, cpu, address, size, context):
        if address in (0x0802B684, 0x0802B67E):
            cpu.reg_write(UC_ARM_REG_R0, TX if address == 0x0802B684 else RX)
            cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))
            return
        # Validate the real locking API arguments, including pointer-to-lock
        # passed to the native RAII-style release routine.
        if address == 0x08029C8C:
            assert cpu.reg_read(UC_ARM_REG_R0) == 0x20003688
        if address == 0x08030EB8:
            arg = cpu.reg_read(UC_ARM_REG_R0)
            assert struct.unpack('<I', cpu.mem_read(arg, 4))[0] == 0x20003688
        super().hook(cpu, address, size, context)


def main():
    image = (OUT/f'{STEM}.bin').read_bytes()
    sections, symbols = elf(OUT/'extension.elf')
    # fm_write_block runs from ITCM: load the fast path exactly as fm_boot copies it.
    itcm = next(raw for name, address, raw in sections if name == '.itcm')
    assert image[symbols['__itcm_load']-BASE:symbols['__itcm_load']-BASE+len(itcm)] == itcm
    cases = []
    capture = None
    # BLOCK_WORDS (34 since 0.2.0) fits one DMA chunk; larger counts are refused.
    for count in (0, 1, 16, 32, BLOCK_WORDS-1, BLOCK_WORDS, BLOCK_WORDS+1):
        arm = BurstProbe(image)
        arm.cpu.mem_write(symbols['__itcm_start'], itcm)
        samples = [((i*0x18531) ^ 0x812345) & 0xffffff for i in range(BLOCK_WORDS)]
        arm.cpu.mem_write(SOURCE, struct.pack(f'<{BLOCK_WORDS}I', *samples))
        arm.cpu.mem_write(TX-4, b'\xa5'*520)
        result = arm.call(symbols['fm_write_block'], 0x37801, SOURCE, count)
        tx = [e['tx'] for e in arm.events if e['kind']=='transmit']
        command = [e['tx'] for e in arm.events if e['kind']=='duplex']
        if count in (0,BLOCK_WORDS+1):
            assert result == 0 and not arm.events
        else:
            # Chunks of at most 170 words, each its own locked address setup and DMA.
            chunks=[(o,min(TX_WORDS,count-o)) for o in range(0,count,TX_WORDS)]
            assert result == 1 and command == [f'0b{(0x37801+o)&0xffff:04x}' for o,_ in chunks]
            assert tx == [b''.join(x.to_bytes(3,'big') for x in samples[o:o+n]).hex() for o,n in chunks]
            locks=[e['kind'] for e in arm.events if e['kind'] in ('lock','unlock')]
            assert locks==['lock','unlock']*len(chunks) and arm.events[0]['kind']=='lock' and arm.events[-1]['kind']=='unlock'
            for k,i in enumerate(i for i,e in enumerate(arm.events) if e['kind']=='transmit'):
                assert any(e.get('function')=='0x8031032' for e in arm.events[:i])
                assert arm.events[i+1]['kind']=='ready_fixture'
                assert arm.events[i+2]=={'kind':'wait_fixture','function':'0x802b66c'}
            longest=max(n for _,n in chunks)
            assert arm.cpu.mem_read(TX-4,4)==b'\xa5'*4
            assert arm.cpu.mem_read(TX+3*longest,512-3*longest)==b'\xa5'*(512-3*longest)
            assert struct.unpack('<I',arm.cpu.mem_read(0x20003684,4))[0]==0xffff
            if count==BLOCK_WORDS: capture=(samples,command,tx)
        cases.append({'count':count,'accepted':bool(result),'payload_dma_transfers':len(tx),'max_bytes_per_dma':3*max([n for _,n in chunks] or [0]) if result else 0})
    assert capture
    maps,_,_,_=unpack_stream(image)
    folder=OUT/'bulk-transfer';folder.mkdir(exist_ok=True)
    RECEIVER_OUT.mkdir(parents=True,exist_ok=True)
    p1=folder/'core1-P.bin'
    p1.write_bytes(b''.join(maps['P'].get(a,0).to_bytes(3,'big') for a in range(max(maps['P'])+1)))
    pointer=struct.unpack_from('<I',image,0x08031658-BASE)[0]-BASE
    count=int.from_bytes(image[pointer:pointer+3],'big')
    p0=folder/'core0-P.bin';p0.write_bytes(image[pointer+6:pointer+6+3*count])
    samples,commands,payloads=capture
    for core in (0,1):
        for mode in (0,0x8000):
            state=0x10 if core==0 else 6
            initial=[f'LOADP {p0 if core==0 else p1} 0']+(data_lines(maps) if core else [])
            initial+=['BRIDGE 30000','IOX ffff91 1','IOX ffffbf 300','IOX ffffbe 200','MAX 1000']
            lines=[put('X',state,[0,mode]),put('Y',0x37800,[0x112233]*(BLOCK_WORDS+2))]
            for command,payload in zip(commands,payloads):
                lines+=receive(core,int(command,16),True)
                for i in range(0,len(payload)//2,3):lines+=receive(core,int.from_bytes(bytes.fromhex(payload)[i:i+3],'big'),False)
            for space in 'XYP':lines+=[f'DUMP{space} 37800 {BLOCK_WORDS+2}']
            lines+=[f'DUMPX {state:x} 2']
            memory,runs=execute(f'fm-bulk-c{core}-m{mode:x}',initial,lines)
            assert all(memory[f'{space}:37800']==[0x112233]+samples+[0x112233] for space in 'XYP')
            assert memory[f'X:{state:x}']==[0x37801+BLOCK_WORDS,mode] and runs==BLOCK_WORDS+len(commands)
            cases.append({'core_handler':core,'mode':mode,'received_words':BLOCK_WORDS,'wire_order_and_neighbors_verified':True})
    report={'image_sha256':hashlib.sha256(image).hexdigest(),'hardware_access':False,
            'scope':__doc__.strip(),'cases':cases}
    (OUT/'bulk-transfer-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'{len(cases)} ARM burst / DSP receiver cases passed')


if __name__=='__main__':main()
