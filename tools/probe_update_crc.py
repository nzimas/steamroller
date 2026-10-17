"""Execute the reference Tracks bootloader CRC loop offline. No device access.

External NOR reads and watchdog calls are fixtures; CRC register feeds are
captured from the actual ARM loop. This is not a Rhythm bootloader dump.
"""
import hashlib
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M7, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_PC)
from inspect_firmware import ROOT, ct_fw

REFERENCE=ROOT/'research/CircuitTracksReverseEngineering/Firmware-Analysis/diffCompare/bootloader.bin'


def captured_crc_input(image):
    boot=REFERENCE.read_bytes()
    cpu=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
    cpu.mem_map(0x08000000,0x10000);cpu.mem_write(0x08000000,boot)
    cpu.mem_map(0x20000000,0x50000)
    # Resolve the external-flash object from the actual loop's literal pool.
    state=struct.unpack_from('<I',boot,0x5ce0)[0]
    cpu.mem_write(state+8,struct.pack('<I',0x2004c000))
    cpu.mem_write(0x2004c000,struct.pack('<I',0x2004c100))
    cpu.mem_write(0x2004c10c,struct.pack('<I',0x0800ff01))
    cpu.mem_write(0x2004c118,struct.pack('<I',0x0800ff11))
    feeds=bytearray()
    def hook(uc,address,size,context):
        args=[uc.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        result=0
        if address==0x08007e88:feeds.clear()
        elif address==0x08004ef0:uc.mem_write(args[0],bytes(args[1]))
        elif address==0x0800ff00:
            data=image[args[1]:args[1]+args[3]]
            if len(data)!=args[3]:raise ValueError('Reference NOR read exceeds image')
            uc.mem_write(args[2],data)
        elif address==0x08007eda:feeds.extend(args[0].to_bytes(4,'big'))
        elif address==0x08007ec6:result=ct_fw.crc32_mpeg2(feeds)
        elif address not in (0x08004ae2,0x0800ff10):return
        uc.reg_write(UC_ARM_REG_R0,result)
        uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    cpu.hook_add(UC_HOOK_CODE,hook)
    cpu.reg_write(UC_ARM_REG_R0,0);cpu.reg_write(UC_ARM_REG_R1,len(image))
    cpu.reg_write(UC_ARM_REG_SP,0x2004f000);cpu.reg_write(UC_ARM_REG_LR,0x0800fff1)
    cpu.emu_start(0x08005c69,0x0800fff0,count=4_000_000,timeout=20_000_000)
    if cpu.reg_read(UC_ARM_REG_PC)!=0x0800fff0:raise RuntimeError('Reference CRC did not return')
    return bytes(feeds),cpu.reg_read(UC_ARM_REG_R0)


def main():
    build=ROOT/'build/rhythm-fm'
    manifest=json.loads((build/'manifest.json').read_text())
    from rhythm_fm_version import STEM
    candidate=ct_fw.decode((build/f'{STEM}.syx').read_bytes())['image']
    stock=(ROOT/'firmware/official/circuitrhythm-firmware-5706.bin').read_bytes()
    cases=[]
    for name,image in [('official',stock),('candidate',candidate),
                       ('unaligned-regression',candidate[:-2]),
                       ('one-byte-remainder',b'abcde'),('three-byte-remainder',b'abcdefg')]:
        fed,actual=captured_crc_input(image)
        expected=image+bytes(-len(image)%4)
        if fed!=expected or actual!=ct_fw.crc32_mpeg2(expected):raise RuntimeError('Reference CRC model mismatch')
        exact=ct_fw.crc32_mpeg2(image)
        if name in ('official','candidate') and (len(image)%4 or actual!=exact):
            raise ValueError('Candidate is not compatible with the reference word CRC')
        if name not in ('official','candidate') and actual==exact:raise RuntimeError('Negative CRC fixture did not reproduce failure')
        cases.append({'case':name,'image_bytes':len(image),'crc_bytes':len(fed),
                      'header_crc':hex(exact),'reference_crc':hex(actual),'matches':actual==exact})
    report={'image_sha256':manifest['image_sha256'],'reference_bootloader_sha256':hashlib.sha256(REFERENCE.read_bytes()).hexdigest(),
            'function':'0x08005C68','cases':cases,'hardware_access':False,
            'scope':'Actual reference Tracks ARM CRC loop; external flash and CRC hardware are fixtures. Rhythm behavior inferred.'}
    (build/'update-crc-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
