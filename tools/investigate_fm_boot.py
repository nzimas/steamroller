"""Audit the exact withdrawn 0.1.2 image offline. Never opens MIDI or USB.

The DSP memory audit uses NXP's documented physical map, not the interpreter's
permissive backing arrays. The ARM check executes reset, FM boot and the actual
boot trampoline; it replaces the board/peripheral initialization call only.
It does not reproduce a full board reset or identify its reset-cause register.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR,
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
from compare_platform import ROOT, BASE, initialize_ram
from rhythm_dsp_probe import unpack_stream
from rhythm_dsp_layout import validate_segments, verify_stock_configuration, require_mapped, SOURCE

EXPECTED='9bba793bda28baa0194097448608f0b43f42ec00de64561b67d7f2367c6f90e9'


def boot_continuation(image, *, state_address=0x2004e760, trampoline=0x0806cce8):
    cpu=initialize_ram(image,0x08025688,return_cpu=True)
    if state_address < 0x4000:
        cpu.mem_map(0,0x4000)
        cpu.mem_write(0xe000ef90,struct.pack('<I',5<<3))
    setup=[];continuation=[];path=[]
    def hook(uc,address,size,context):
        if address in (0x08025688,0x0806b000,trampoline):path.append(hex(address))
        if address==0x08025cc4:
            setup.append({'sp':hex(uc.reg_read(UC_ARM_REG_SP)),'lr':hex(uc.reg_read(UC_ARM_REG_LR))})
            # Peripheral/clock/interrupt setup is outside this narrow ABI check.
            for reg,value in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),(0x10,0x20,0x30,0x40)):
                uc.reg_write(reg,value)
            uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
        elif address==0x0802568e:
            sp=uc.reg_read(UC_ARM_REG_SP)
            continuation.append({'sp':hex(sp),'saved_stack':bytes(uc.mem_read(sp,8)).hex()})
            uc.emu_stop()
    cpu.hook_add(UC_HOOK_CODE,hook)
    cpu.emu_start(0x08025689,0,count=100000,timeout=2000000)
    if len(setup)!=1 or len(continuation)!=1:raise RuntimeError('Boot trampoline did not reach native continuation')
    return {'path':path,'setup_calls':setup,'continuation':continuation[0],
            'fm_magic':hex(struct.unpack('<I',cpu.mem_read(state_address,4))[0]),
            'heap_limit':hex(struct.unpack('<I',cpu.mem_read(0x20002b54,4))[0]),
            'scope':'Actual reset and ARM boot trampoline; board initialization call stubbed. No full boot claim.'}


def main():
    archive=ROOT/'dist/withdrawn/rhythm-fm-0.1.2.zip'
    with zipfile.ZipFile(archive) as z:
        image=z.read('rhythm-fm-0.1.2/rhythm-fm-0.1.2.bin')
        manifest=json.loads(z.read('rhythm-fm-0.1.2/manifest.json'))
    if hashlib.sha256(image).hexdigest()!=EXPECTED:raise ValueError('Wrong withdrawn image')
    stock=(ROOT/'firmware/official/circuitrhythm-firmware-5706.bin').read_bytes()
    maps,segments,_,_=unpack_stream(image)
    stock_maps,stock_segments,_,_=unpack_stream(stock)
    configuration=validate_segments(stock,stock_segments)
    rejected=None
    try:validate_segments(image,segments)
    except ValueError as error:rejected=str(error)
    if not rejected:raise RuntimeError('Physical map failed to reject the broken release')
    added=[s for s in segments if s not in stock_segments]
    if added!=[{'space':'P','address':0x1000,'words':383}]:raise ValueError('Unexpected changed DSP segments')
    if [maps['P'][a] for a in (0x11d,0x11e)]!=[0x0bf080,0x1000]:raise ValueError('Expected broken call missing')
    # Geometry check only: a future relocation may fit here. No new image is built.
    replacement=0xD40
    require_mapped('P',replacement,383,'hypothetical replacement')
    if any(a in stock_maps['P'] for a in range(replacement,replacement+383)):
        raise ValueError('Hypothetical replacement overlaps stock code')
    # Regression: both internal edges and shared space; reject precisely the hole
    # that the old 0x80000-word interpreter silently treated as RAM.
    for space,addr,count in [('P',0xfff,1),('X',0x2fff,1),('Y',0x1fff,1),('P',0x30000,383)]:
        require_mapped(space,addr,count)
    for space,addr,count in [('P',0x1000,1),('P',0xfff,2),('X',0x3000,1),('Y',0x2000,1),('Y',0x37fff,2)]:
        try:require_mapped(space,addr,count)
        except ValueError:pass
        else:raise RuntimeError('Physical RAM boundary guard failed')
    stock_boot=boot_continuation(stock);custom_boot=boot_continuation(image)
    if stock_boot['continuation']!=custom_boot['continuation'] or custom_boot['fm_magic']!='0x464d31':
        raise RuntimeError('ARM boot trampoline ABI differs from stock')
    if '0x806cce8' not in custom_boot['path']:raise RuntimeError('Test bypassed the real trampoline')
    # Exercise the same placement guard imported by the builder, without
    # invoking a build of the newer release or modifying preserved artifacts.
    from build_rhythm_fm import require_mapped as build_guard
    try:build_guard('P',added[0]['address'],added[0]['words'])
    except ValueError:pass
    else:raise RuntimeError('Build guard did not reject withdrawn placement')
    report={
        'image_sha256':EXPECTED,'source_archive':str(archive),'hardware_access':False,
        'stock_recovery':'User confirmed official firmware restored and boots normally.',
        'configuration':configuration,'stock_segments_fit':True,
        'failed_dsp_segment':added[0],'physical_layout_rejection':rejected,
        'audio_loop_hook':{'address':'P:0x011D','target':'P:0x1000','unconditional':True},
        'possible_in_bounds_relocation':{'start':'P:0x0D40','end':'P:0x0EBE','overlap':False,'tested_on_hardware':False},
        'old_test_model':{'words_per_space':0x80000,'validator':'DefaultMemoryValidator accepts all addresses',
                          'boot_trampoline_was_stubbed':True,'dsp_transfer_was_stubbed':True},
        'stock_arm_boot':stock_boot,'custom_arm_boot':custom_boot,
        'shared_build_guard_rejects_withdrawn_layout':True,
        'reset_cause':'No hardware reset register or instruction trace captured. Loss of DSP execution/handshake is a plausible reset path, not directly measured.',
        'reference':SOURCE}
    output=ROOT/'docs/rhythm-fm-boot-investigation.json'
    output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
