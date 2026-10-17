"""Capture the Rhythm update request encoder in ARM emulation; no MIDI access."""
import json
import struct

from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS,UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M7,UC_ARM_REG_R0,UC_ARM_REG_R1,
                               UC_ARM_REG_R2,UC_ARM_REG_SP,UC_ARM_REG_PC,UC_ARM_REG_LR)
from compare_platform import ROOT,BASE,IMAGES,digest


def main():
    image=(ROOT/'firmware/official'/IMAGES['rhythm'][0]).read_bytes()
    if digest(image)!=IMAGES['rhythm'][3]:raise ValueError('Unexpected image')
    cases=[]
    for offset in (0,32,64,0xFE0,0x1000,0x1FFE0,0x20000,0x6FFE0,0x70000):
        cpu=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
        cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
        cpu.mem_map(0x08000000,0x80000);cpu.mem_map(0x20000000,0x50000)
        cpu.mem_write(BASE,image);cpu.reg_write(UC_ARM_REG_SP,0x2004F000)
        cpu.reg_write(UC_ARM_REG_LR,0x08000001);cpu.reg_write(UC_ARM_REG_R0,offset)
        sent=[]
        def hook(uc,address,size,context):
            r0,r1,r2=[uc.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2)]
            if address==0x08022D10:uc.mem_write(r0,bytes(uc.mem_read(r1,r2)))
            elif address==0x080298B8:
                pointer,length=struct.unpack('<II',uc.mem_read(r2,8))
                sent.append(bytes(uc.mem_read(pointer,length)).hex())
            else:return
            uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
        cpu.hook_add(UC_HOOK_CODE,hook)
        cpu.emu_start(0x0802D0B1,0x08000000,count=10000,timeout=1000000)
        if cpu.reg_read(UC_ARM_REG_PC)!=0x08000000 or len(sent)!=1:raise RuntimeError('Request encoder did not return')
        wire=bytes.fromhex(sent[0]);block=offset>>5
        want=bytes([0xF0,0,0x20,0x29,0,0x72,block&127,(block>>7)&127,(block>>14)&127,0xF7])
        if wire!=want:raise RuntimeError('Unexpected encoded request')
        cases.append({'byte_offset_argument':offset,'requested_block':block,'sysex_hex':sent[0]})
    report={'firmware_sha256':digest(image),'function':'0x0802D0B0','cases':cases,
            'scope':'Official application request encoder; MIDI send replaced with capture. Not a bootloader transfer test.',
            'hardware_access':False}
    path=ROOT/'build/rhythm-fm/request-encoder-validation.json'
    path.write_text(json.dumps(report,indent=2)+'\n')
    print(f'{len(cases)} real ARM request-encoder captures passed; {path}')


if __name__=='__main__':main()
