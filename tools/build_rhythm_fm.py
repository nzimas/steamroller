"""Build the current Rhythm FM candidate: FM tracks 1..4, native sampler tracks 5..8.

Patches only a hash-checked official Rhythm image. Never opens a device.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from compare_platform import ROOT, BASE, IMAGES, digest
from rhythm_dsp_probe import unpack_stream
from inspect_firmware import ct_fw
from rhythm_dsp_layout import verify_stock_configuration, require_mapped
from rhythm_fm_version import (VERSION, NAME, STEM, INSTALL_MARKER, IDENTITY_BUILD, MEMORY_FAILURE_BUILD,
                               FM_TRACKS, OUT_TRACKS, FRAMES, SLOT_STRIDE, SLOT_COUNT, SLOT_WORDS, SLOT_BASE, SEQ_WRAP)

sys.path.insert(0,str(ROOT/'research/CIRCUITS-HACKS-RC/tools'))
import dsp56 as D
from thumb import b_w, bl, h

OUT = ROOT/'build/rhythm-fm'
SRC = ROOT/'firmware/rhythm_fm'
PBASE, SCRATCH = 0x0D40, 0x2B00
CTRL, SLOTS = 0x37700, tuple(SLOT_BASE+SLOT_STRIDE*i for i in range(SLOT_COUNT))
CTRL_WORDS = 10
# DSP control words: 1 read sequence, 2 underruns (blocks without audition
# data, normal while idle), 3 drops, 4 status, 5 frames, 6 heartbeats,
# 7 copies, 8 (unused), 9 the read slot's address (advanced with the sequence).
CTRL_NONZERO, CTRL_READ_SLOT = CTRL+8, CTRL+9
# Every register the consumer touches.
SAVED = ['sr','a0','a1','a2','b0','b1','b2','x0','x1','y0','r0','r1','r5','n5','m1','m5']
SCRATCH_WORDS = len(SAVED)
TRACK_SHIFT = 6   # length word: FRAMES | audition track << 6
SHIFT_HOOK = 0x0803D956
PAD_HOOK = 0x0803D7FE          # stock controller pad press (controller vtable 0x080672E4 slot 0)
LOWFLASH = (0x080101F0, 0x0801A838)   # stock core-1 stream, reclaimed once the pointer is relocated
MAGIC = 0x464D31
IDENTITY_ADDRESS = 0x080270D8
_cache = {}


def T(line):
    if line not in _cache:
        r=subprocess.run([str(ROOT/'research/analysis/dsptool-io'),'asm'],input=line+'\n',
                         text=True,capture_output=True,check=True).stdout.strip().split()
        if not r or r[0]=='ERR' or len(r)!=int(r[0])+1:
            raise RuntimeError(f'DSP assembly failed: {line}: {r}')
        _cache[line]=[int(x,16) for x in r[1:]]
    return _cache[line]


def J(cc):
    return lambda target:[{'al':0x0AF080,'ne':0x0AF0A2,'eq':0x0AF0AA,'ge':0x0AF0A1,'lt':0x0AF0A9,'cs':0x0AF0A8}[cc],target]


def consumer():
    """DSP core-1 audition mixer, called in place of the native sampler pass
    (P:$11D): runs that pass ($0221: span clear, voices), then adds one
    queued 32-sample block from the ARM into its track's active span. No
    block (no audition sounding) changes nothing."""
    a=D.Asm(PBASE)
    def emit(line): a.emit(T(line))
    def load(address,reg='a'): emit(f'move y:>${address:x},{reg}')
    def store(address,reg='a'): emit(f'move {reg},y:>${address:x}')
    a.label('entry')
    a.emit(D.jsr,0x221)  # displaced stock sampler pass
    for i,r in enumerate(SAVED): emit(f'move {r},x:>${SCRATCH+i:x}')
    emit('move #>$ffff,m1'); emit('move #>$ffff,m5')
    emit('move x:>$1,a'); store(CTRL+5)
    load(CTRL+6); emit('add #>$1,a'); emit('and #>$ffffff,a'); store(CTRL+6)
    load(CTRL); emit(f'cmp #>${MAGIC:x},a'); a.emit(J('ne'),'restore')
    emit('move x:>$1,a'); emit(f'cmp #>${FRAMES:x},a'); a.emit(J('eq'),'enabled')
    emit('move #>$2,a'); store(CTRL+4); a.emit(J('al'),'restore')
    a.label('enabled')
    emit('move #>$1,a'); store(CTRL+4)
    load(CTRL+1); emit('move a1,x0')
    load(CTRL_READ_SLOT,'r5')
    emit('move r5,r0'); emit('move r5,r1')
    emit('move y:(r1)+,a'); emit('cmp #>$1,a'); a.emit(J('ne'),'miss')
    emit('move y:(r1)+,a'); emit('cmp x0,a'); a.emit(J('eq'),'length')
    # tag - sequence modulo the wrap: below half the wrap the block is a future
    # one and stays queued; otherwise it is stale and released.
    emit('sub x0,a'); a.emit(J('ge'),'tag_diff')
    emit(f'add #>${SEQ_WRAP:x},a')
    a.label('tag_diff')
    emit(f'cmp #>${SEQ_WRAP//2:x},a'); a.emit(J('lt'),'miss'); a.emit(J('al'),'drop')
    a.label('length')
    emit('move y:(r1)+,a'); emit('move a1,x1'); emit('and #>$3f,a'); emit(f'cmp #>${FRAMES:x},a'); a.emit(J('eq'),'copy')
    a.label('drop')
    emit('clr a'); emit('move a,y:(r0)')
    load(CTRL+3); emit('add #>$1,a'); store(CTRL+3)
    a.emit(J('al'),'miss')
    a.label('copy')
    # destination: Y:$304F0 + 64*track + 32*bank (the active bank)
    # (logical shifts act on a1 only; an arithmetic shift would carry the
    # length bits through a0 and back)
    emit('move x1,a'); emit(f'lsr #{TRACK_SHIFT},a'); emit('and #>$7,a'); emit('lsl #6,a'); emit('move a1,y0')
    emit('movep y:<<$ffffd0,a'); emit('and #>$1,a'); emit('lsl #5,a'); emit('add y0,a'); emit('add #>$304f0,a')
    emit('move a1,r5')
    load(CTRL+7); emit('add #>$1,a'); store(CTRL+7)
    # 32 samples as 8 x 4 unrolled, counted in B: no hardware DO loop.
    emit(f'move #>${FRAMES//4:x},b')
    a.label('add')
    for _ in range(4):
        for line in ('move y:(r1)+,x0','move y:(r5),a','add x0,a','move a,y:(r5)+'): emit(line)
    emit('sub #$1,b'); a.emit(J('ne'),'add')
    emit('clr a'); emit('move a,y:(r0)')   # release the slot
    a.emit(J('al'),'advance')
    a.label('miss')
    load(CTRL+2); emit('add #>$1,a'); emit('and #>$ffffff,a'); store(CTRL+2)
    a.emit(J('al'),'restore')
    a.label('advance')
    load(CTRL+1); emit('add #>$1,a'); emit(f'cmp #>${SEQ_WRAP:x},a'); a.emit(J('ne'),'seq_ok')
    emit('clr a')
    a.label('seq_ok')
    store(CTRL+1)
    load(CTRL_READ_SLOT); emit(f'add #>${SLOT_STRIDE:x},a'); emit(f'cmp #>${SLOT_BASE+SLOT_COUNT*SLOT_STRIDE:x},a'); a.emit(J('ne'),'slot_ok')
    emit(f'move #>${SLOT_BASE:x},a')
    a.label('slot_ok')
    store(CTRL_READ_SLOT)
    a.label('restore')
    for i,r in reversed(list(enumerate(SAVED))): emit(f'move x:>${SCRATCH+i:x},{r}')
    a.emit(D.jmp,0x8A6)  # displaced following call, returns to native P:$11F
    return a.assemble()


def elf(path):
    data=path.read_bytes(); header=struct.unpack_from('<16sHHIIIIIHHHHHH',data)
    if data[:6]!=b'\x7fELF\x01\x01': raise ValueError('Expected ELF32 little-endian')
    sections=[struct.unpack_from('<10I',data,header[6]+i*header[11]) for i in range(header[12])]
    names=sections[header[13]]; strings=data[names[4]:names[4]+names[5]]
    allocated=[]; symbols={}
    for row in sections:
        name=strings[row[0]:].split(b'\0',1)[0].decode()
        if row[2]&2:
            if row[1]==8 and row[5]: raise ValueError('Unallocated extension BSS')
            allocated.append((name,row[3],data[row[4]:row[4]+row[5]]))
        if row[1]==2:
            st=sections[row[6]]; strings2=data[st[4]:st[4]+st[5]]
            for off in range(row[4],row[4]+row[5],row[9]):
                n,value,size,info,other,index=struct.unpack_from('<IIIBBH',data,off)
                name2=strings2[n:].split(b'\0',1)[0].decode()
                if index and name2: symbols[name2]=value
    return allocated,symbols


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    stock=(ROOT/'firmware/official'/IMAGES['rhythm'][0]).read_bytes()
    if digest(stock)!=IMAGES['rhythm'][3]: raise ValueError('Stock image hash mismatch')
    verify_stock_configuration(stock)
    # Reject the withdrawn placement before compiling or replacing any output.
    require_mapped('P',PBASE,1,'FM consumer entry')
    # The drum voice uses the single-precision FPU, which the stock RTOS port
    # enables (CPACR) and preserves per task; integer calling convention kept.
    common=['clang','--target=arm-none-eabi','-mcpu=cortex-m7','-mthumb','-mfloat-abi=softfp','-mfpu=fpv5-sp-d16',
            '-ffp-contract=off',   # no fused multiply-add: host and ARM drums stay bit-identical
            # No literal pools: constants are built with movw/movt. The stock
            # firmware leaves the D-cache off, so a literal load is an uncached
            # 3-wait-state flash read; the I-cache (on during fm_tick) serves code.
            '-mexecute-only',
            '-O2','-fwrapv','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections',
            '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-fstack-usage']
    # The ITCM fast path (0x400) is beyond BL range of flash code: its functions
    # are declared long_call (FM6_HOT in extension.c); nothing in it calls flash.
    objects=[]
    for filename in ('extension.c','trampolines.S'):
        target=OUT/(Path(filename).stem+'.o'); objects.append(str(target))
        subprocess.run(common+['-c',str(SRC/filename),'-o',str(target)],check=True)
    env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/zig-cache'),
             ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/zig-local'))
    subprocess.run([sys.executable,'-m','ziglang','build-exe','-target','thumb-freestanding-eabi',
                    '-mcpu','cortex_m7','-fentry=fm_boot','-T',str(SRC/'extension.ld'),
                    '--no-gc-sections',*objects,'-femit-bin='+str(OUT/'extension.elf')],env=env,check=True)
    sections,symbols=elf(OUT/'extension.elf')
    image=bytearray(stock)
    patches=[]
    def write(address,value,reason):
        offset=address-BASE
        if offset<0: raise ValueError('Bootloader mutation forbidden')
        if offset+len(value)>0x70000: raise ValueError('Flash overflow')
        if offset+len(value)>len(image): image.extend(b'\xff'*(offset+len(value)-len(image)))
        old=bytes(image[offset:offset+len(value)])
        image[offset:offset+len(value)]=value
        patches.append({'address':hex(address),'bytes':len(value),'purpose':reason,
                        'before_sha256':digest(old),'after_sha256':digest(value)})
    # Receipt success does not establish installation. The reference bootloader
    # discards staged images whose 38-byte version block matches installed flash.
    # Keep Rhythm's product/version intact; distinguish this revision in padding.
    if stock[0x1e8:0x1ee] != bytes(6):raise ValueError('Version marker padding is not empty')
    write(BASE+0x1e8,INSTALL_MARKER,'unique installation-version marker')
    # This is the stock standard identity response literal, not a new SysEx API.
    # Give the running custom application an observable identity distinct from
    # stock 5706. This identifies the image; it does not attest FM initialization.
    original_identity=bytes.fromhex('f07e0006020020290163000001012c4af7')
    offset=IDENTITY_ADDRESS-BASE
    if stock[offset:offset+17]!=original_identity:raise ValueError('Unexpected identity response literal')
    write(IDENTITY_ADDRESS+14,bytes([IDENTITY_BUILD>>7,IDENTITY_BUILD&127]),f'custom application identity build {IDENTITY_BUILD}')
    if stock[0x08031660-BASE:0x08031664-BASE]!=struct.pack('<I',LOWFLASH[0]): raise ValueError('Unexpected core-1 stream pointer')
    if int.from_bytes(stock[LOWFLASH[0]-BASE:LOWFLASH[0]-BASE+3],'big')*3+6!=LOWFLASH[1]-LOWFLASH[0]-1: raise ValueError('Unexpected core-1 stream size')
    for name,address,raw in sections:
        if name=='.lowflash':
            if not LOWFLASH[0]<=address or address+len(raw)>LOWFLASH[1]: raise ValueError('Drum code outside the reclaimed stream flash')
            write(address,raw,'poundhard drum code and tables in the reclaimed core-1 stream flash')
            continue
        if name=='.itcm':
            # Runs at ITCM 0x100..; stored in flash at its load address, copied by fm_boot.
            if address!=symbols['__itcm_start'] or address!=0x100 or address+len(raw)>0x1000: raise ValueError('ITCM table misplaced')
            address=symbols['__itcm_load']
        if not 0x0806A580<=address or address+len(raw)>0x08075000: raise ValueError('Extension outside reserved append range')
        write(address,raw,'ARM extension '+name)
    hooks=[(0x08025688,'fm_boot',6),
           (0x08029f70,'fm_service',4), (0x08026f32,'fm_identity_send',4),
           (SHIFT_HOOK,'fm_shift_button',4),
           (PAD_HOOK,'fm_pad',4),
           (0x0803D9A4,'fm_track_hook',4),         # track button case: keep the Sample Rec view while a sound waits
           # Parameter locks (firmware/rhythm_fm/plock.c, docs/RHYTHM-AUTOMATION-PLOCKS.md)
           (0x08055324,'plock_tick',4),            # track player lane playback, replaced
           (0x080552F6,'plock_reset_hook',4),      # player reset: revert locks, then stock
           (0x0805518C,'plock_stop_hook',6),       # player transport clear: revert locks, then stock
           (0x0802854E,'plock_router_capture',4),  # save the knob router after its constructor
           (0x08038E90,'plock_knob',4),            # knob router vt0: Shift + macros 1-4 -> mixer/FX lanes
           (0x0805CE84,'plock_preview',4)]         # step preview: play the locks, then restore the knobs
    # The Shift hook displaces exactly the stock mask literal load; the
    # trampoline re-materializes that mask for every id it does not consume.
    if stock[SHIFT_HOOK-BASE:SHIFT_HOOK-BASE+4]!=bytes.fromhex('dff8ec1a'):raise ValueError('Unexpected Shift button branch')
    if struct.unpack_from('<I',stock,0x0803E444-BASE)[0]!=0x07DFFF00:raise ValueError('Unexpected Shift button mask')
    # Expected bytes come directly from the pinned stock, and are also retained
    # for review in the manifest. Instruction widths are checked by Capstone.
    from capstone import Cs,CS_ARCH_ARM,CS_MODE_THUMB
    decoder=Cs(CS_ARCH_ARM,CS_MODE_THUMB)
    for address,symbol,size in hooks:
        old=stock[address-BASE:address-BASE+size]
        instructions=list(decoder.disasm(old,address))
        if sum(i.size for i in instructions)!=size: raise ValueError('Hook splits an instruction')
        target=symbols[symbol]&~1
        patch=(bl(address,target) if symbol=='fm_identity_send' else b_w(address,target))+b'\x00\xbf'*((size-4)//2)
        write(address,patch,'hook '+symbol)
        patches[-1]['original_bytes']=old.hex(); patches[-1]['target']=hex(target)
    # The Sample Rec view's frame method (vt+0x1C, vtable word 0x08067A24 =
    # 0x0804FE9B) -> fm_samplerec_frame, which lights pads 22/23 for the
    # generator's modes after stock composes the frame.
    if struct.unpack_from('<I',stock,0x08067A24-BASE)[0]!=0x0804FE9B: raise ValueError('Unexpected Sample Rec frame vtable word')
    if not symbols['fm_samplerec_frame']&1: raise ValueError('fm_samplerec_frame is not Thumb')
    write(0x08067A24,struct.pack('<I',symbols['fm_samplerec_frame']),'generator: Sample Rec view frame -> fm_samplerec_frame')
    # The router's pad release (vtable word 0x080672E8 = 0x0803D897) ->
    # fm_pad_release, which calls it after watching the kit pad.
    if struct.unpack_from('<I',stock,0x080672E8-BASE)[0]!=0x0803D897: raise ValueError('Unexpected router pad-release vtable word')
    if not symbols['fm_pad_release']&1: raise ValueError('fm_pad_release is not Thumb')
    write(0x080672E8,struct.pack('<I',symbols['fm_pad_release']),'generator: router pad release -> fm_pad_release')
    # Step mixer locks (firmware/rhythm_fm/steplock.c): the router's button
    # press, the track view's frame and the transport object's message
    # listener (both entries) go through the extension, which calls stock.
    for word,stock_value,symbol,what in ((0x080672EC,0x0803D8B9,'fm_button','router button press -> fm_button (sample-page buttons scroll the step sliders)'),
                                         (0x080678F8,0x0804DD1D,'fm_track_frame','track view frame -> fm_track_frame (step sliders on the bottom rows)'),
                                         (0x08066B40,0x08033CA9,'fm_transport_message','transport listener -> fm_transport_message (finds Record)'),
                                         (0x08066B6C,0x08033CA7,'fm_transport_message_thunk','transport listener thunk -> fm_transport_message_thunk')):
        if struct.unpack_from('<I',stock,word-BASE)[0]!=stock_value: raise ValueError(f'Unexpected vtable word at {word:#x}')
        if not symbols[symbol]&1: raise ValueError(f'{symbol} is not Thumb')
        write(word,struct.pack('<I',symbols[symbol]),'step locks: '+what)
    # DSP core 0 (flat program image at [0x08031658]: u24 count, 3 bytes, then
    # P words from 0): the per-block glide of each track's left/right gain
    # (P:$2D9, `move #>$20000,b` -> coefficient) is 2^-6 per 0.67 ms block, a
    # ~42 ms time constant. A locked level/pan therefore reached a note's
    # transient only after ~100 ms (hardware 0.2.11: a hard-right kick locked
    # to level 0 still sounded its attack on the left). 0x400000 halves the
    # distance every block (~1 ms); the mixer still ramps per sample inside a
    # block (measured with the stock code in dsptool-io).
    core0=struct.unpack_from('<I',stock,0x08031658-BASE)[0]
    glide=core0+6+3*0x2D9
    if stock[glide-BASE:glide-BASE+6]!=bytes.fromhex('57f400020000'): raise ValueError('Unexpected core-0 level/pan glide')
    write(glide+3,bytes.fromhex('400000'),'DSP core 0: track level/pan glide 2^-6 -> 1/2 per block (locks land on the transient)')
    # DSP core 0: the per-track Grid FX bus (tools/rhythm_fm_fxbus.py). The
    # flat image is loaded word for word from P:0, so the new routines go into
    # its unused interrupt vectors (only $00, $3E and $44 are used) and three
    # stock calls are redirected; every stock word is checked first.
    import rhythm_fm_fxbus as fxbus
    fx_base,fx_words,fx_labels=fxbus.build()
    def core0_word(a): return core0+6+3*a
    if int.from_bytes(stock[core0-BASE:core0-BASE+3],'big')<0xB8: raise ValueError('Core-0 image too short')
    if any(stock[core0_word(fx_base+i)-BASE:core0_word(fx_base+i)-BASE+3]!=bytes(3) for i in range(len(fx_words))) or fx_base+len(fx_words)>0x100:
        raise ValueError('Core-0 vector space for the FX bus is not free')
    write(core0_word(fx_base),b''.join(w.to_bytes(3,'big') for w in fx_words),'DSP core 0: FX bus routines (FXSEL, SWAP, ADD) in unused vectors P:$80..')
    for address,old,new in fxbus.sites(fx_labels):
        if stock[core0_word(address)-BASE:core0_word(address)-BASE+3*len(old)]!=b''.join(w.to_bytes(3,'big') for w in old): raise ValueError(f'Unexpected core-0 code at P:{address:#x}')
        write(core0_word(address),b''.join(w.to_bytes(3,'big') for w in new),f'DSP core 0: P:{address:#x} -> FX bus routine')
    # The step editor (0x08047D82) writes a held step's value to micro-step 0
    # and 0xFF to micro-steps 1-5: those five stores become nop.w, so
    # micro-steps 1-5 of the level/pan lanes can hold the step's FX locks.
    for address in (0x08047E32,0x08047E40,0x08047E4E,0x08047E5C,0x08047E6A):
        old=stock[address-BASE:address-BASE+4]
        if old[2:]!=bytes([0x29+(address-0x08047E32)//14,0x01]) or old[0]&0xf0!=0x80 or old[1]!=0xf8: raise ValueError(f'Unexpected editor store at {address:#x}: {old.hex()}')
        write(address,bytes.fromhex('aff30080'),'step editor: keep micro-steps 1-5 (FX locks)')
    # Parameter locks, in place. Recording: no interpolation fill between knob
    # events (the bl to 0x08049706 becomes nop.w) ...
    if stock[0x08049544-BASE:0x08049548-BASE]!=bytes.fromhex('00f0dff8'): raise ValueError('Unexpected record fill call')
    write(0x08049544,bytes.fromhex('aff30080'),'plock: no interpolation fill when recording')
    # ... and the recorded value goes to the step's micro-step 0 only, never
    # overwriting a lock with 0xFF: movs r1,#6; muls r1,r6,r1; cmp.w sl,#0xFF;
    # it ne; strbne.w sl,[r0,r1] (exactly the 14 stock bytes).
    if stock[0x0804982C-BASE:0x0804983A-BASE]!=bytes.fromhex('06eb460108eb4101c9b200f801a0'): raise ValueError('Unexpected record writer')
    plock_record=bytes.fromhex('06217143baf1ff0f18bf00f801a0')
    decoded=[(i.mnemonic,i.op_str) for i in decoder.disasm(plock_record,0x0804982C)]
    if decoded!=[('movs','r1, #6'),('muls','r1, r6, r1'),('cmp.w','sl, #0xff'),('it','ne'),('strbne.w','sl, [r0, r1]')]:
        raise ValueError(f'plock record patch decodes as {decoded}')
    write(0x0804982C,plock_record,'plock: record at micro-step 0, keep locks')
    maps,segments,count,trailing=unpack_stream(stock)
    if SLOT_WORDS>SLOT_STRIDE or SLOTS[-1]+SLOT_WORDS>0x38000 or SLOT_BASE<CTRL+CTRL_WORDS:raise ValueError('FM FIFO slot layout overflows shared SRAM or control words')
    for slot in SLOTS:
        require_mapped('Y',slot,SLOT_WORDS,'FM FIFO slot')
        if any(slot<=a<slot+SLOT_WORDS for space in ('P','X','Y') for a in maps[space]):
            raise ValueError('FM FIFO overlaps initialized stock shared memory')
    require_mapped('Y',CTRL,CTRL_WORDS,'FM FIFO control')
    require_mapped('X',SCRATCH,SCRATCH_WORDS,'FM consumer scratch')
    if any(SCRATCH<=a<SCRATCH+SCRATCH_WORDS for a in maps['X']): raise ValueError('FM consumer scratch overlaps stock X data')
    if any(CTRL<=a<CTRL+CTRL_WORDS for space in ('P','X','Y') for a in maps[space]):
        raise ValueError('FM control words overlap initialized stock shared memory')
    words,labels=consumer()
    require_mapped('P',PBASE,len(words),'FM consumer program')
    if PBASE+len(words)>0x1000:raise ValueError('FM consumer exceeds physical P memory')
    if any(PBASE<=a<PBASE+len(words) for a in maps['P']): raise ValueError('DSP program overlap')
    # Replace the two adjacent one-word calls with a long call to our wrapper.
    if [maps['P'][a] for a in (0x11d,0x11e)] != [0x0d0221,0x0d08a6]: raise ValueError('Unexpected DSP hook')
    maps['P'][0x11d],maps['P'][0x11e]=0x0bf080,PBASE
    stream=[]
    for segment in segments:
        space,address,n=segment['space'],segment['address'],segment['words']
        stream += ['PXY'.index(space),n,address]+[maps[space][address+i] for i in range(n)]
    stream += [0,len(words),PBASE]+words+[0x100]+trailing
    if len(stream)>0xffff: raise ValueError('DSP loader count overflow')
    if (BASE+len(image)+15&~15)+len(stream)*3+6>BASE+0x70000: raise ValueError('Relocated DSP stream does not fit the application flash')
    dsp_blob=len(stream).to_bytes(3,'big')+bytes(3)+D.to_bytes(stream)
    stream_address=(BASE+len(image)+15)&~15
    write(stream_address,dsp_blob,'relocated patched core-1 segmented download')
    write(0x08031660,struct.pack('<I',stream_address),'core-1 download pointer')
    write((BASE+len(image)+3)&~3,f'RHYTHM-FM-{VERSION}\0TRACKS-1-4-FM\0'.encode(),'build marker')
    # The reference bootloader CRC loop feeds ceil(length/4) complete words.
    # Include any final-word padding in BOTH the image size and header CRC.
    if len(image)%4:
        write(BASE+len(image),b'\xff'*(-len(image)%4),'CRC word-alignment padding included in header size')
    image=bytes(image)
    if image[:0x1c8]!=stock[:0x1c8]: raise ValueError('Vector table changed')
    if image[0x1c8:0x1ee]==stock[0x1c8:0x1ee]:raise ValueError('Installation version block unchanged')
    official=ct_fw.decode((ROOT/'firmware/official/circuitrhythm-firmware-5706.syx').read_bytes())
    syx=ct_fw.encode(image,proto=official['proto'],product=0x63)
    decoded=ct_fw.decode(syx)
    if decoded['image']!=image or decoded['product']!=0x63: raise ValueError('Container roundtrip failed')
    (OUT/f'{STEM}.bin').write_bytes(image)
    (OUT/f'{STEM}.syx').write_bytes(syx)
    (OUT/'dsp-extension.bin').write_bytes(D.to_bytes(words))
    listing=subprocess.run([str(ROOT/'research/analysis/dsptool-io'),'dis',str(OUT/'dsp-extension.bin'),hex(PBASE)],
                           capture_output=True,text=True,check=True).stdout
    (OUT/'dsp-extension.asm').write_text(listing)
    manifest={'name':NAME,'base_sha256':digest(stock),'image_sha256':digest(image),
              'sysex_sha256':digest(syx),'image_bytes':len(image),'physical_limit_bytes':0x70000,
              'product':0x63,'tracks':{'4-6':'poundhard FM drums (8 kinds by sample slot; Shift + pad randomizes)','1-3':'FM, six operators, monophonic (one voice per track); native distortion/HPF/LPF knobs; Shift + track button draws a random patch',
                                     '7-8':'native sampler'},
              'randomizer':{'gesture':'Shift + track button 1-4','stock_handler':hex(0x0803D8B8),'hook':hex(SHIFT_HOOK),
                            'generator':'Tonitor fm_generate gen 3 via rhythm_generate: dark-ambient / rhythmic-noise lanes, no GLISSE/WARP/RUBBER, no pitch envelope, vibrato only; no fm_measure',
                            'persistence':'none: random patches are lost on power-off or project change'},
              'fm_effects':{'chain':'native P:$0648 distortion, P:$056E high-pass, P:$0590 low-pass+resonance','params':'Y:$307C8+32*track: $A distortion, $B high-pass, $C low-pass, $D resonance'},
              'drums':{'tracks':'4-6','engine':'Vascular poundhard port (poundhard.c)','kind':'selected sample slot & 7: kick snare hat clap tom metal noise glitch',
                       'randomize':'Shift + pad (pad hook 0x0803D7FE)','code':'reclaimed core-1 stream flash 0x080101F0','itcm_voices':'0x2100'},
              'polyphony':{'voices':'1 per FM track','itcm_pool':'0x1B60','overload':'oldest released voice faded out over one block when FIFO < 3 queued blocks'},
              'itcm_fast_path':{'run_address':'0x100','contents':'FM6 operator loops, core render, envelopes, LFO, voice resampler, SINE; the rest runs from flash with the I-cache on during fm_tick'},
              'fifo':{'slots':SLOT_COUNT,'stride_words':SLOT_STRIDE,'slot_words':SLOT_WORDS,'fm_tracks':FM_TRACKS,
                      'base':hex(SLOT_BASE),'max_queued':10,'blocks_per_service_call':6,'sequence_wrap':SEQ_WRAP,
                      'last_word':hex(SLOTS[-1]+SLOT_WORDS-1)},
              'arm_symbols':{k:hex(v) for k,v in symbols.items() if k.startswith(('fm_','rhythm_'))},
              'arm_reserved_state':{'start':hex(0x3000),'bytes':4096,'region':'ITCM RAM','native_heap_limit_unchanged':True},
              'dsp_program':{'start':hex(PBASE),'words':len(words),'labels':{k:hex(v) for k,v in labels.items()}},
              'dsp_state':{'scratch_x':hex(SCRATCH),'control':hex(CTRL),'slots':[hex(x) for x in SLOTS]},
              'source_sha256':{str(p.relative_to(ROOT)):digest(p.read_bytes()) for p in sorted(SRC.rglob('*')) if p.is_file()},
              'patches':patches,'fm_core_rate':44100,'target_output_rate':48000,
              'installation_marker':INSTALL_MARKER.decode(),'identity_build':IDENTITY_BUILD,
              'image_word_aligned':len(image)%4==0,
              'hardware_tested':False,'release_status':f'{VERSION}: FM tracks 1-3 (mono), poundhard drums 4-6, samplers 7-8; hardware untested',
              'memory_failure_identity_build':MEMORY_FAILURE_BUILD,
              'allocation_limits':'DSP scratch/staging selected from static map gaps; full runtime allocation audit pending'}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'image':str(OUT/f'{STEM}.syx'),'bytes':len(image),
                      'dsp_words':len(words),'sha256':digest(image)},indent=2))


if __name__=='__main__': main()
