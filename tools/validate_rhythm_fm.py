"""Validate the actual built Rhythm FM 0.2.x candidate (stock sampler plus the
poundhard sound generator and parameter locks) in ARM and DSP emulators.

Hardware dependencies are stubbed. No connected-device access or installation.
"""
import json
import re
import struct
import subprocess
import wave

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R5,
                               UC_ARM_REG_LR,UC_ARM_REG_PC,UC_ARM_REG_SP)
from build_rhythm_fm import (OUT, ROOT, BASE, elf, CTRL, SLOTS, MAGIC, PBASE, T, INSTALL_MARKER, IDENTITY_BUILD,
                             IDENTITY_ADDRESS, SHIFT_HOOK, PAD_HOOK, CTRL_WORDS, CTRL_READ_SLOT, TRACK_SHIFT, SAVED)
from rhythm_fm_version import (MAX_AHEAD, SLOT_COUNT, SEQ_WRAP, STEM, FRAMES, BLOCK_WORDS, SLOT_WORDS,
                               REPORT_VERSION, REPORT_FIELDS, MEMORY_FAILURE_BUILD, GEN_FAMILIES)
from compare_platform import initialize_ram
from rhythm_dsp_layout import validate_segments
from investigate_fm_boot import boot_continuation
from rhythm_note_probe import Probe as NoteProbe, NOTE_ON
from rhythm_dsp_probe import unpack_stream, data_lines
from rhythm_mix_probe import put
from inspect_firmware import ct_fw

STATE=0x2000
TOOL=ROOT/'research/analysis/dsptool-io'
TEST=OUT/'validation'
REPORT_BYTES=6+5*REPORT_FIELDS+1


def host_harness():
    """Compile the exact extension source for the host; return its State layout."""
    subprocess.run(['clang','-O2','-fwrapv','-fno-builtin','-ffp-contract=off',str(ROOT/'tools/rhythm_fm_host.c'),
                    '-o',str(OUT/'host-render')],check=True,capture_output=True)
    return json.loads(subprocess.run([str(OUT/'host-render'),'--layout'],capture_output=True,text=True,check=True).stdout)


def require(value,message):
    if not value: raise RuntimeError(message)


class Machine(NoteProbe):
    def __init__(self,image,symbols):
        self.symbols=symbols
        self.memory={}
        self.bus=[]
        self.boot_calls=0
        super().__init__(image)
        self.cpu.mem_map(0,0x4000)
        self.cpu.mem_map(0xe0000000,0x100000)
        self.cpu.mem_write(0xe000ef90,struct.pack('<I',5<<3))
        self.cpu.mem_write(0x20002B54,struct.pack('<II',0x2004F760,0x20007B90))

    def hook(self,cpu,address,size,context):
        args=[cpu.reg_read(x) for x in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2)]
        result=0
        if address==(self.symbols['rhythm_boot_original']&~1):
            self.boot_calls+=1
        elif address==0x080298B8:
            pointer,length=struct.unpack('<II',cpu.mem_read(args[2],8))
            self.events.append({'stub':'midi_send','sysex':bytes(cpu.mem_read(pointer,length)).hex()})
        elif address in (0x08029C8C,0x08030EB8,0x08046414,0x08046534):
            pass
        elif address==0x0802B684:
            result=0x200058B8
        elif address==0x080313EE:
            self.write_pointer=args[1]
        elif address==0x080315E6:
            raw=bytes(cpu.mem_read(args[1],args[2]))
            for i in range(0,len(raw),3):
                target=self.write_pointer+i//3
                word=int.from_bytes(raw[i:i+3],'big')
                self.memory[target]=word
                self.bus.append(('write',target,word))
        elif address==0x080314D6:
            self.bus.append(('read',args[1]))
            result=self.memory.get(args[1],0)<<8
        elif address==0x08031228:
            word=(args[1]>>8)&0xFFFFFF
            self.memory[args[2]]=word
            self.bus.append(('write',args[2],word))
        else:
            return super().hook(cpu,address,size,context)
        cpu.reg_write(UC_ARM_REG_R0,result)
        cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))

    def run(self,address,*args):
        self.events=[]
        self.cpu.reg_write(UC_ARM_REG_SP,0x2004FF00)
        self.cpu.reg_write(UC_ARM_REG_LR,0x08000001)
        for r,v in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),args): self.cpu.reg_write(r,v)
        self.cpu.emu_start(address|1,0x08000000,count=300_000_000,timeout=600_000_000)
        require(self.cpu.reg_read(UC_ARM_REG_PC)==0x08000000,'ARM call failed to return')
        require(self.cpu.reg_read(UC_ARM_REG_SP)==0x2004FF00,'ARM stack imbalance')
        return list(self.events)

    def word(self,address): return struct.unpack('<I',self.cpu.mem_read(address,4))[0]

    def shift_button(self,button):
        """Enter the patched stock button handler at its Shift-held branch with
        its own stack frame (push {r3-r7,lr}); r4 = controller, r5 = id."""
        self.events=[]
        sp=0x2004FF00-24
        self.cpu.mem_write(sp,struct.pack('<6I',0,0x11111111,0x22222222,0x33333333,0x44444444,0x08000001))
        self.cpu.reg_write(UC_ARM_REG_SP,sp)
        self.cpu.reg_write(UC_ARM_REG_R4,0x20010000); self.cpu.reg_write(UC_ARM_REG_R5,button)
        self.cpu.reg_write(UC_ARM_REG_R0,1)
        stops=[]
        def stop(cpu,address,size,data):
            stops.append(address); cpu.reg_write(UC_ARM_REG_R1,cpu.reg_read(UC_ARM_REG_R1)); cpu.emu_stop()
        handle=self.cpu.hook_add(UC_HOOK_CODE,stop,begin=0x0803D962,end=0x0803D962)
        try:
            self.cpu.emu_start(SHIFT_HOOK|1,0x08000000,count=200_000,timeout=10_000_000)
        finally:
            self.cpu.hook_del(handle)
        if stops: return 'stock_dispatch',self.cpu.reg_read(UC_ARM_REG_R1)
        require(self.cpu.reg_read(UC_ARM_REG_PC)==0x08000000 and self.cpu.reg_read(UC_ARM_REG_SP)==0x2004FF00,
                'Shift button path did not return through the stock epilogue')
        require(self.cpu.reg_read(UC_ARM_REG_R4)==0x11111111 and self.cpu.reg_read(UC_ARM_REG_R5)==0x22222222,
                'Shift button path did not restore callee-saved registers')
        return 'returned',None


def parse(result):
    require('CORE1 bounds enabled' in result,'DSP physical-memory profile was not enabled')
    memory={}
    for m in re.finditer(r'^([XY]) ([0-9a-f]+):((?: [0-9a-f]{6})+)$',result,re.M):
        memory[f'{m[1]}:{int(m[2],16):x}']=[int(x,16) for x in m[3].split()]
    runs=re.findall(r'RAN (\d+) pc=([0-9a-f]+)',result)
    require(runs and all(int(pc,16)==0x8A6 for _,pc in runs),'DSP consumer failed to reach stock continuation')
    return memory


def instructions(machine,address,*args):
    """Count executed ARM instructions for one call (emulator cost, not cycles)."""
    count=[0]
    def tick(cpu,a,size,data):count[0]+=1
    handle=machine.cpu.hook_add(UC_HOOK_CODE,tick)
    try:machine.run(address,*args)
    finally:machine.cpu.hook_del(handle)
    return count[0]




GFX_WRITERS=(0x08030988,0x080309DA,0x08030A10,0x08030A56,0x08030B0E,0x08030B96,0x08030C18)


class PlockMachine(Machine):
    """Fake stock sequencer objects in RAM: a track player with its lanes,
    setter, stores and vtable, a knob router with live value stores, and a
    controller with a Shift store. Fake methods live at FAKE+n."""
    FAKE=0x20040F00
    def __init__(self,image,symbols):
        super().__init__(image,symbols)
        self.sets=[];self.posts=[];self.frame_buf=0
    def hook(self,cpu,address,size,context):
        if self.FAKE<=address<self.FAKE+0x40:
            args=[cpu.reg_read(x) for x in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
            result=0
            if address==self.FAKE: result=self.lanes+192*args[2]           # player vt+0x14: lane address
            elif address==self.FAKE+0x10: self.sets.append((args[1],args[2]))   # player setter vt0(id, value)
            elif address==self.FAKE+0x18: self.sets.append(('R',args[1],args[2]))   # knob router's real setter
            elif address==self.FAKE+0x20: result=self.frame_buf                  # stock track view frame
            cpu.reg_write(UC_ARM_REG_R0,result)
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
            return
        if address in GFX_WRITERS:   # stock Grid FX DSP writers (read the param cache, write the DSP)
            self.sets.append(('W',address));cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR));return
        if address==0x0804DD1C:   # stock track view frame: a fixed buffer
            cpu.reg_write(UC_ARM_REG_R0,self.frame_buf);cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR));return
        if address==0x08047038:   # knob router -> sequencer message (store, track, lane, value)
            args=[cpu.reg_read(x) for x in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
            self.posts.append((args[1],args[2],args[3]))
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
            return
        return super().hook(cpu,address,size,context)
    def u32(self,a,*v): self.cpu.mem_write(a,struct.pack(f'<{len(v)}I',*v))
    def store(self,a,buf,*values):
        """A stock double-buffered store (0x0803FC58): [+4] buffer bits, [+0xC] buffer 0."""
        self.u32(a,0,0,0,buf,buf)
        for i,v in enumerate(values): self.u32(buf+4*i,v)
        return a



def plock_cases(image,symbols,layout):
    """Parameter locks on the actual patched image: the hooked stock entry
    points run against fabricated stock sequencer objects. With the sampler
    on every track, macros keep their stock meanings: a lock in a lane is
    sent as that lane's own parameter."""
    cases=[]
    arm=PlockMachine(image,symbols);arm.run(0x08025688)
    F=arm.FAKE
    P,VT,SET,SVT=0x20041000,0x20041800,0x20041900,0x20041980
    IDS,HELD,LAST=0x20041A00,0x20041A40,0x20041A80
    arm.lanes=0x20042000
    R,STORES=0x20043000,0x20043100
    t=1                                   # track 2: ids 16..23, mixer 0x81, 0x89, 0x92, 0x9A
    ids=[16*t+i for i in range(8)]+[0x80+t,0x88+t,0x91+t,0x99+t]
    arm.cpu.mem_write(IDS,struct.pack('<12H',*ids))
    def reset_arrays():
        arm.cpu.mem_write(HELD,struct.pack('<12H',*[0xFFFF]*12));arm.cpu.mem_write(LAST,struct.pack('<12H',*[0xFFFF]*12))
    reset_arrays()
    arm.u32(VT,0x20044001,0,0,F+0x21,0,F+1)            # vt+0xC reset callee, vt+0x14 lane address
    arm.u32(SET,SVT);arm.u32(SVT,F+0x11)
    arm.u32(P,VT);arm.u32(P+0x2C,SET)
    def stores(mask,touched,recording):
        arm.store(P+0x100,P+0x200,mask);arm.u32(P+0x30,P+0x100)
        arm.store(P+0x120,P+0x210,touched);arm.u32(P+0x34,P+0x120)
        arm.store(P+0x140,P+0x220,recording);arm.u32(P+0x38,P+0x140)
    stores(1<<6|1<<9,0,0)
    arm.cpu.mem_write(P+0x3C,bytes([12]));arm.u32(P+0x40,IDS);arm.cpu.mem_write(P+0x44,struct.pack('<H',16*t))
    arm.u32(P+0x48,HELD,LAST)
    arm.cpu.mem_write(arm.lanes,b'\xff'*(12*192))
    def lock(lane,step,value): arm.cpu.mem_write(arm.lanes+192*lane+6*step,bytes([value]))
    lock(6,4,20);lock(9,4,100);lock(9,5,30)     # macro 7 (low-pass) and the pan lane
    def hits(*steps_):                           # step byte 2 (micro-step triggers) in the pattern before the lanes
        for s_ in range(32): arm.cpu.mem_write(arm.lanes-0x128+8*s_+2,bytes([1 if s_ in steps_ else 0]))
    hits(*range(32))
    arm.u32(P+0x10,arm.store(P+0x2A0,P+0x2B0,0,3))   # sync rate 3: 6 ticks per step
    # knob router live values: track store 1 index 6 = 90 (macro 7); mixer group 9 (0x88..) index 1 = 64 (pan centre)
    group=[arm.store(STORES+0x40+0x20*g,STORES+0x200+0x40*g,*([0]*16)) for g in range(13)]
    arm.u32(STORES,*group);arm.u32(STORES+0x200+0x40*1+4*6,90);arm.u32(STORES+0x200+0x40*9+4*1,64)
    arm.u32(R+0x24,STORES)
    RSET,RSVT=0x20043A00,0x20043A40      # the knob router's real setter (live turns)
    arm.u32(RSET,RSVT);arm.u32(RSVT,F+0x19);arm.u32(R+0x08,RSET)
    arm.run(symbols['plock_router_set'],R)
    base=STATE+layout['plock_locks']
    def play(step,tick=0):
        arm.sets=[];arm.run(0x08055324,P,0,step,tick);return list(arm.sets)
    require([play(s) for s in range(4)]==[[]]*4,'Unlocked steps sent values')
    arm.bus=[]
    require(play(4)==[(22,20),('R',0x89,100)],'Locked step did not send its locks')
    # ... and each value went straight into the DSP's parameter block (not only
    # the stock setter's cache, drained later): Y:$307C8 + 32 t + param, with
    # param from the setter table (macro 7 = 12 low-pass, 0x89 = 19).
    require([e for e in arm.bus if e[0]=='write']==[('write',0x307C8+32*t+12,20),('write',0x307C8+32*t+19,100)],
            f'Locks not written straight to the DSP: {arm.bus}')
    require(play(4,1)==[] and play(4,3)==[],'Locks sent off the step start')
    require(play(5)==[(22,90),('R',0x89,30)],'Next step did not revert macro 7 / change the pan lock')
    require(play(6)==[('R',0x89,64)],'Unlocked step did not return the pan to its base')
    require(play(7)==[],'Reverted lane sent again')
    require(arm.word(base)==3 and arm.word(base+4)==2,'Lock/revert counters wrong')
    cases.append({'case':'locked_steps_send_and_unlocked_steps_revert_to_the_knob'})
    # A lock lasts its whole sound: steps without a hit keep it (their own
    # lock bytes are dead); the next hit without a lock reverts.
    hits(4,7);reset_arrays();play(3)
    require(play(4)==[(22,20),('R',0x89,100)],'Locked hit did not send')
    require(play(5)==[] and play(6)==[],'An empty step reverted a lock (or played a lock of its own)')
    require(play(7)==[(22,90),('R',0x89,64)],'The next hit did not revert to the knobs')
    hits(*range(32));reset_arrays();play(7)
    cases.append({'case':'locks_hold_over_empty_steps_until_the_next_hit'})
    # The next hit's locks go out on the last tick of the step before it
    # (rate 3: tick 5), ahead of its transient; tick 0 does not resend them.
    reset_arrays();play(3)
    require(play(3,4)==[] and play(3,5)==[(22,20),('R',0x89,100)],'Next hit\'s locks not sent a tick early')
    require(play(4)==[],'Locks sent twice')
    require(play(4,5)==[(22,90),('R',0x89,30)] and play(5)==[],'Early change to the next hit\'s lock / revert wrong')
    require(play(5,5)==[('R',0x89,64)] and play(6)==[],'Revert before an unlocked hit not early')
    reset_arrays();play(7)
    cases.append({'case':'next_hit_locks_sent_one_tick_early'})
    # A lock belongs to its step's sound: a step without a trigger never plays
    # it, and the sweep erases it.
    hits(*[s_ for s_ in range(32) if s_!=4]);reset_arrays();play(3)
    require(play(3,5)==[] and play(4)==[],'A lock on a step without a trigger was played')
    TRK=0x20030000                              # a whole track: 8 patterns of 0xA28 (player [+0x50])
    track=bytearray(b'\xff'*(8*0xA28))
    for pt in range(8):
        for s_ in range(32): track[0xA28*pt+8*s_:0xA28*pt+8*s_+8]=bytes([0,6,0 if (pt==2 and s_==4) else 1,0,7,0xff,0,0])
    for pt,l,s_,v in ((2,6,4,20),(2,9,4,100),(2,9,5,30)): track[0xA28*pt+0x128+192*l+6*s_]=v
    arm.cpu.mem_write(TRK,bytes(track));arm.u32(P+0x50,TRK)
    for _ in range(64): arm.run(symbols['lock_sweep'])
    after=arm.cpu.mem_read(TRK,8*0xA28)
    require(after[0xA28*2+0x128+192*6+6*4:][:6]==b'\xff'*6 and after[0xA28*2+0x128+192*9+6*4]==0xff,'Sweep left locks on a removed step')
    require(after[0xA28*2+0x128+192*9+6*5]==30,'Sweep erased a lock on a step with a trigger')
    hits(*range(32));reset_arrays();play(7)
    cases.append({'case':'locks_of_removed_steps_are_dead_and_swept'})
    # The knob being turned (not recording) wins: no revert to the stale base.
    play(4);stores(1<<6|1<<9,1<<6,0)
    require(play(5)==[('R',0x89,30)],'Touched lane was reverted or locked')
    stores(1<<6|1<<9,0,0);play(6);play(7)
    cases.append({'case':'turned_knob_overrides_and_rebases_a_lock'})
    # Reset (0x080552F6) and transport clear (0x0805518C) revert active locks first.
    play(4);arm.sets=[];arm.run(0x080552F6,P)
    require(arm.sets==[(22,90),('R',0x89,64)],'Player reset left locks applied')
    require(struct.unpack('<12H',arm.cpu.mem_read(HELD,24))==(0xFFFF,)*12,'Stock reset did not run after the revert')
    reset_arrays();play(7);play(4)
    arm.u32(P+0x14,1);arm.u32(P+0x18,2)
    arm.store(P+0x160,P+0x230,1,0,0);arm.u32(P+0x1C,P+0x160)
    arm.store(P+0x180,P+0x240,0);arm.u32(P+0x20,P+0x180)
    arm.store(P+0x1A0,P+0x250,1);arm.u32(P+0x28,P+0x1A0)
    arm.sets=[];arm.run(0x08055120,P,2)
    require(arm.sets==[(22,90),('R',0x89,64)],'Transport clear left locks applied')
    require(struct.unpack('<12H',arm.cpu.mem_read(LAST,24))==(0xFFFF,)*12,'Stock transport clear did not run after the revert')
    cases.append({'case':'reset_and_transport_clear_revert_locks','hooks':['0x080552f6','0x0805518c']})
    # Live knob turns are stock on every track (no macro remapping): sent
    # through the router's real setter and mirrored under their own id.
    for id_,value in ((16*t+1,70),(0x60+2,90)):
        arm.sets=[];arm.run(0x08038E90,R,id_,value,0)
        require(arm.sets==[('R',id_,value)],f'Live knob {id_:#x} not stock: {arm.sets}')
    require(arm.word(STORES+0x200+0x40*1+4*1)==70,'Live macro 2 not mirrored')
    cases.append({'case':'live_knobs_stock_on_all_tracks'})
    # Step preview (a held step while stopped): micro 0 plays the step's locks,
    # micro 5 restores the knobs; micro 1-4 send nothing.
    reset_arrays();play(7)
    PV,PVL=0x20044100,0x20044200
    arm.u32(PV+0x0C,PVL);arm.u32(PVL+0x18,arm.lanes)
    arm.u32(PV+0x18,arm.store(PV+0x80,PV+0xC0,0,4))
    arm.u32(PV+0x50,SET);arm.u32(PV+0x54,IDS)
    seen=[]
    for micro in range(6):
        arm.sets=[];arm.run(0x0805CE84,PV,micro);seen.append(list(arm.sets))
    require(seen==[[(22,20),('R',0x89,100)],[],[],[],[],[(22,90),('R',0x89,64)]],f'Step preview locks wrong: {seen}')
    cases.append({'case':'step_preview_plays_locks_then_restores_knobs'})
    require(image[0x08049544-0x08010000:0x08049548-0x08010000]==bytes.fromhex('aff30080') and
            image[0x0804982C-0x08010000:0x0804983A-0x08010000]==bytes.fromhex('06217143baf1ff0f18bf00f801a0'),'Recording patches missing')
    cases.append({'case':'recording_quantized_to_step_and_keeps_locks'})
    require(all(image[a-0x08010000:a-0x08010000+4]==bytes.fromhex('aff30080') for a in (0x08047E32,0x08047E40,0x08047E4E,0x08047E5C,0x08047E6A)),
            'Step editor still overwrites micro-steps 1-5 (FX locks)')
    cases.append({'case':'step_editor_keeps_micro_steps_1_to_5'})
    cases+=spice_cases(image,arm,symbols,layout,P,t)
    return cases


def spice_cases(image,arm,symbols,layout,P,t):
    """Shift + track outside the Sample Rec view: structure-aware random
    locks on the track's current pattern (spice.c), on the fabricated player."""
    cases=[]
    PAT=arm.lanes-0x128                     # the fake lane address ignores the pattern: base = lanes - 0x128
    arm.cpu.mem_write(arm.lanes,b'\xff'*(12*192))
    steps=bytearray()
    hits={4:110,7:40,10:40,12:110,13:70,14:80,15:95}   # backbeat accents, ghosts, a fill into the loop
    for s_ in range(32): steps+=bytes([0,6,1 if s_ in hits else 0,hits.get(s_,0),7,0xff,0,0])
    arm.cpu.mem_write(PAT,bytes(steps)+bytes([15,0,3,0]))      # end 15, start 0 (16 steps), sync rate 3
    arm.u32(P+0x08,arm.store(P+0x2C0,P+0x2E0,0,0,0))           # playhead {pattern 0, step, tick}
    USER=33
    STORES=arm.word(0x20043000+0x24)
    for group,index,value in ((t,4,0),(t,5,0),(t,6,90),(t,7,20),(8,t,0),(9,t,0),(11,t,100),(12,t,64)):
        arm.u32(arm.word(arm.word(STORES+4*group)+0xC)+4*index,value)   # knobs: level 100, pan centre, sends off
    arm.cpu.mem_write(arm.lanes+192*6+6*7,bytes([USER]))        # the user's own low-pass lock on step 8
    setup_router(arm,view=0)
    S_=STATE+layout['spices']
    def lanes(): return [list(arm.cpu.mem_read(arm.lanes+192*l,192))[::6][:32] for l in range(12)]
    names=['dist','hpf','lpf','reso','rev','dly','lvl','pan']
    def show(grid,info):
        print(f'spice (palette {info>>8&255:#04x}, {info>>16&255} locks):')
        print('      '+''.join(' x ' if s_ in hits else ' . ' for s_ in range(16)))
        for l in range(4,12): print(f'{names[l-4]:>5} '+''.join(f'{v:3d}' if v!=0xff else '  -' for v in grid[l][:16]))
    before=lanes()
    fresh=PlockMachine(image,symbols);fresh.run(0x08025688);setup_router(fresh,view=0)
    require(fresh.shift_button(t)==('returned',None) and fresh.word(S_+8)&255==1,'Spice before any playback was not refused')
    arm.sets=[];arm.run(0x08055324,P,0,0,0)                     # the player plays (captured)
    require(arm.shift_button(t)==('returned',None),'Spice path did not return')
    first=lanes();info=arm.word(S_+4)
    require(arm.word(S_)==1 and info&7==t and not info>>6&1,f'Spice report wrong {info:#x}')
    require(first[:4]==before[:4],'Spice touched macros 1-4 (pitch, decay, attack, start/end)')
    require(first[6][7]==USER,'Spice overwrote the user\'s own lock')
    locks=0
    for l in range(4,12):
        for s_ in range(16):
            v=first[l][s_]
            if v==0xff or (l==6 and s_==7): continue
            locks+=1
            if s_ not in hits: require(first[l][(s_-1)%16]==v,f'Lane {l} step {s_+1}: a lock off a hit that does not hold the previous hit')
        require(all(v==0xff for v in first[l][16:]),f'Lane {l}: locks past the pattern end')
    require(locks>0 and info>>16&255>0,'Spice wrote no locks')
    require(all(first[l][s_]==0xff for l in (10,11) for s_ in (4,12)),'Level/pan locked on the backbone accents')
    palette=info>>8&255
    require(3<=bin(palette).count('1')<=4 and palette&0xc0 and palette&0x07,f'Palette {palette:#x} lacks pan/level or a tone parameter')
    require(all((palette>>(l-4)&1) or first[l]==before[l] for l in range(4,12)),'Locks outside the palette')
    cases.append({'case':'spice_locks_hits_by_structure','palette':palette,'locks':info>>16&255,'period':info>>24})
    # Again on the unchanged pattern: a re-roll from the same starting point (no pile-up).
    rolls=set()
    for _ in range(6):
        arm.shift_button(t);again=lanes();info=arm.word(S_+4)
        require(info>>6&1,'Second press was not a re-roll')
        require(again[6][7]==USER,'Re-roll lost the user\'s lock')
        palette=info>>8&255
        require(all((palette>>(l-4)&1) or again[l]==before[l] for l in range(4,12)),'Re-roll kept the previous spice')
        rolls.add(bytes(sum(again[4:],[])))
        if len(rolls)<=3: show(again,info)
    require(len(rolls)>=3,'Re-rolls did not vary')
    # Played: the locks are sent on their steps.
    arm.cpu.mem_write(arm.lanes+192*4+6*12,bytes([90]))         # a user edit: next press snapshots afresh
    arm.shift_button(t);require(not arm.word(S_+4)>>6&1,'An edited pattern was re-rolled from a stale snapshot')
    sent=[]
    for s_ in range(16): arm.sets=[];arm.run(0x08055324,P,0,s_,0);sent+=arm.sets
    require(sent,'Spiced pattern sent no locks')
    cases.append({'case':'spice_reroll_restores_then_varies','variants':len(rolls),'sent':len(sent)})
    cases+=step_lock_cases(arm,symbols,layout,t)
    return cases


def step_lock_cases(arm,symbols,layout,t):
    """Record on + a step held in the track view: the bottom rows are level /
    pan / delay / reverb sliders for that step (steplock.c)."""
    cases=[]
    R=0x20043000;STORES=arm.word(R+0x24)
    TV,TR,X,XO=0x20045000,0x20045400,0x20045600,0x20045700
    arm.frame_buf=0x20045800
    arm.cpu.mem_write(arm.frame_buf,struct.pack('<42H',*([5]*42)))
    arm.cpu.mem_write(arm.lanes,b'\xff'*(12*192))
    setup_router(arm,view=0);arm.u32(ROUTER+0x3C,TV)
    arm.cpu.mem_write(TV+0x264,b'\x00');arm.u32(TV+0x25C,arm.store(TV+0x300,TV+0x340,t))
    arm.u32(R+0x2C,4);arm.u32(R+0x34,R+0x400);arm.u32(R+0x400+4*t,X)
    held=arm.store(X+0x80,X+0xC0,0);arm.u32(X+0x24,held)
    STEP=5;arm.u32(X+0x0C,arm.store(X+0x100,X+0x140,0,STEP));arm.u32(X,XO);arm.u32(XO+0x18,arm.lanes)
    T=0x20045A00;rec=arm.store(T+0x80,T+0xC0,0,0,0);arm.u32(T+0x28,rec)
    arm.run(symbols['fm_transport_message'],T,0x1234,0,0)          # the transport's listener runs once: captured
    for g,v in ((11,100),(12,64),(8,0),(9,30)): arm.u32(arm.word(arm.word(STORES+4*g)+0xC)+4*t,v)   # level, pan, reverb, delay knobs
    def frame():
        arm.run(symbols['fm_track_frame'],TV);return list(struct.unpack('<32H',arm.cpu.mem_read(arm.frame_buf,64)))
    def pad(n):
        arm.posts=[];arm.run(symbols['fm_pad'],ROUTER,n,100);arm.run(symbols['fm_pad_release'],ROUTER,n,0);return list(arm.posts)
    arm.u32(X+0xC0,1)
    require(frame()[16:]==[5]*16,'Sliders shown without Record')
    arm.u32(T+0xC0+4,1)                                             # Record on (store index 1)
    arm.u32(X+0xC0,0)
    require(frame()[16:]==[5]*16,'Sliders shown without a held step')
    arm.u32(X+0xC0,1)
    f=frame()
    require(f[:16]==[5]*16,'Step pads changed')
    require(f[16:24]==[0x24]*7+[0],f'Level row (knob 100, unlocked) wrong: {f[16:24]}')
    require(f[24:32]==[0,0,0,0x19,0x19,0,0,0],f'Pan row (centre, unlocked) wrong: {f[24:32]}')
    require(pad(16+2)==[(t,10,36)],'Level pad did not post a level lock for the held step')
    arm.cpu.mem_write(arm.lanes+192*10+6*STEP,bytes([36]))          # the editor stores it
    f=frame();require(f[16:24]==[0x22]*3+[0x24]*5,f'Locked level bar wrong: {f[16:24]}')
    require(pad(24+6)==[(t,11,112)],'Pan pad did not post a right pan')
    arm.posts=[];arm.run(symbols['fm_pad'],ROUTER,24+3,100);arm.run(symbols['fm_pad'],ROUTER,24+4,100)
    require(arm.posts==[(t,11,48),(t,11,64)],f'Middle pan pair did not centre: {arm.posts}')
    arm.run(symbols['fm_pad_release'],ROUTER,27,0);arm.run(symbols['fm_pad_release'],ROUTER,28,0)
    require(pad(16+2)==[] and arm.cpu.mem_read(arm.lanes+192*10+6*STEP,6)==b'\xff'*6,'The lock\'s own pad did not unlock it')
    # the sample-page buttons scroll: level/pan -> pan/delay -> delay/reverb
    for _ in range(2): arm.run(symbols['fm_button'],ROUTER,0x13,0,0)
    require(arm.cpu.reg_read(UC_ARM_REG_R0)==1,'Page button not consumed while active')
    f=frame()
    require(f[16:24]==[0x73]*3+[0]*5 and f[24:32]==[0x48]+[0]*7,f'Delay/reverb rows wrong: {f[16:32]}')
    require(pad(16+7)==[(t,9,127)] and pad(24+4)==[(t,8,73)],'Delay/reverb pads posted wrong lanes')
    arm.run(symbols['fm_button'],ROUTER,0x14,0,0);f=frame()
    require(f[16:24]==[0,0,0,0x19,0x19,0,0,0],'Page-1 button did not scroll back to pan/delay')
    arm.u32(X+0xC0,0);frame();arm.u32(X+0xC0,1)
    require(frame()[16:24]==[0x24]*7+[0],'Rows did not reset to level/pan after the step was released')
    cases.append({'case':'step_sliders_record_held_step','lanes':'level 10, pan 11, delay 9, reverb 8'})
    cases+=grid_fx_cases(arm,symbols,layout,t,STEP)
    return cases


def grid_fx_cases(arm,symbols,layout,t,STEP):
    """Grid FX rows below reverb (gridfx.c): a preset per pad, stored in
    micro-steps 1-5 of the level/pan lanes; a hit with FX locks takes the
    FX bus (newest wins), the owner's next hit without them or a stop
    releases it."""
    cases=[]
    G=STATE+layout['gfx_owner']
    def fxbyte(k): return arm.cpu.mem_read(arm.lanes+192*(10 if k<5 else 11)+6*STEP+1+(k if k<5 else k-5),1)[0]
    def frame():
        arm.run(symbols['fm_track_frame'],0x20045000);return list(struct.unpack('<32H',arm.cpu.mem_read(arm.frame_buf,64)))
    def pad(n): arm.run(symbols['fm_pad'],ROUTER,n,100);arm.run(symbols['fm_pad_release'],ROUTER,n,0)
    for _ in range(4): arm.run(symbols['fm_button'],ROUTER,0x13,0,0)
    f=frame()
    require(f[16:24]==[0x11]*8 and f[24:32]==[0x2e]*8,f'Bit crusher / stutter rows wrong: {f[16:32]}')
    pad(16+3);pad(24+6)
    require(fxbyte(4)==0x83 and fxbyte(2)==0x86,'FX presets not stored in the step')
    f=frame();require(f[16:24]==[0x11]*3+[0x10]+[0x11]*4 and f[24:32]==[0x2e]*6+[0x2d,0x2e],f'FX preset pads not lit: {f[16:32]}')
    pad(16+3);require(fxbyte(4)==0xff,'FX preset not removed by its own pad')
    for _ in range(9): arm.run(symbols['fm_button'],ROUTER,0x13,0,0)
    f=frame();require(f[16:24]==[0x42]*8 and f[24:32]==[0x1d]*8,f'Last rows are not reverser / phaser: {f[16:32]}')
    pad(24+0);require(fxbyte(3)==0x80,'Phaser preset not stored')
    require(arm.cpu.mem_read(arm.lanes+192*10+6*STEP,1)==b'\xff','FX locks touched the level lock (micro-step 0)')
    cases.append({'case':'grid_fx_rows_store_presets_per_step'})
    # Playback: the step is a hit; its locks take the bus.
    arm.cpu.mem_write(arm.lanes-0x128+8*STEP+2,b'\x01')
    def hit(track,tick_call=True):
        arm.sets=[];arm.bus=[]
        if tick_call: arm.run(0x08055324,0x20041000,0,STEP,0)
        else: arm.run(symbols['gfx_hit'],0x20041000,track,0,STEP)
        return [e for e in arm.sets if e[0]=='W' or (e[0]=='R' and e[1]>=0xa9)],[(e[1],e[2]) for e in arm.bus if e[0]=='write' and 0x3770a<=e[1]<0x37712]
    fx,routes=hit(t)
    stutter=[('R',0xaf,7),('R',0xae,1),('W',0x08030A10)]
    phaser=[('R',0xb1+i,v) for i,v in enumerate((0,64,40,0,0,30))]+[('R',0xb0,1),('W',0x08030A56)]
    require(fx==stutter+phaser,f'FX claim sets wrong: {fx}')
    require(sorted(routes)==[(0x3770a+u,1) for u in range(8) if u!=t],f'Only the locked track must stay on the FX bus: {routes}')
    require(arm.word(G)==t+1 and arm.word(G+4)==0b1100,'FX owner / active types wrong')
    require(hit(t)==([],[]),'A held FX was re-sent on the next locked hit')
    fx,routes=hit(2,False)
    require(fx==[] and sorted(routes)==[(0x3770a+t,1),(0x3770a+2,0)],f'Newest lock did not take the bus: {routes}')
    arm.cpu.mem_write(arm.lanes+192*10+6*STEP+3,b'\xff')          # the stutter lock removed
    fx,routes=hit(2,False)
    require(fx==[('R',0xae,0),('W',0x08030A10)] and routes==[],f'An FX the hit no longer locks was not switched off: {fx}')
    arm.cpu.mem_write(arm.lanes+192*10+6*STEP+4,b'\xff')          # no FX locks left
    fx,routes=hit(2,False)
    require(fx==[('R',0xb0,0),('W',0x08030A56)] and sorted(routes)==[(0x3770a+u,0) for u in range(8) if u!=2],f'Owner\'s hit without FX did not release: {fx} {routes}')
    require(arm.word(G)==0 and arm.word(G+4)==0,'FX bus still owned after release')
    arm.cpu.mem_write(arm.lanes+192*10+6*STEP+5,b'\x82');hit(t)      # crusher preset 2, then transport stop
    arm.sets=[];arm.run(0x080552F6,0x20041000)
    require(('R',0xb7,0) in arm.sets and arm.word(G)==0,'Transport stop did not release the FX bus')
    cases.append({'case':'grid_fx_locks_take_the_bus_newest_wins_and_release'})
    return cases


def gain_glide_case(image):
    """DSP core 0 from the built image: a track's level/pan change (level
    127 pan 0 -> level 0 pan 127) settles in a few 0.67 ms blocks (stock: a
    ~42 ms glide that let locks miss the note's transient)."""
    core0=struct.unpack_from('<I',image,0x08031658-BASE)[0]-BASE
    count=int.from_bytes(image[core0:core0+3],'big')
    folder=OUT/'validation';folder.mkdir(parents=True,exist_ok=True)
    (folder/'core0-P.bin').write_bytes(image[core0+6:core0+6+3*count])
    lines=[f'LOADP {folder/"core0-P.bin"} 0','P 3f00 0bf080 00033e 0bf080 0002d9 000000','X 3a 000020','Y 10 400000',
           'Y 307d8 00007f','Y 307d9 000000','STOP 3f04','MAX 100000']
    lines+=['PC 3f00','RUN']*400+['Y 307d8 000000','Y 307d9 00007f']
    for _ in range(8): lines+=['PC 3f00','RUN','DUMPX 9d 2']
    (folder/'gain-glide.txt').write_text('\n'.join(lines)+'\n')
    out=subprocess.run([str(TOOL),'run',str(folder/'gain-glide.txt')],capture_output=True,text=True,check=True).stdout
    left=[int(m[1],16) for m in re.finditer(r'^X 00009d: ([0-9a-f]{6}) ([0-9a-f]{6})$',out,re.M)]
    require(len(left)==8 and left[0]<0x480000 and left[6]<0x020000,f'Track gain glide too slow: {[hex(v) for v in left]}')
    return [{'case':'dsp_track_gain_glide_settles_in_blocks','left_gain_per_block':[hex(v) for v in left]}]


def fx_bus_cases(image):
    """DSP core 0 from the built image: the Grid FX bus patch. A track routed
    to the FX bus (route word 0) is mixed there and not into the main bus;
    before the looper the dry master is set aside and the FX bus takes its
    place (then cleared); after vinyl the dry master is added back."""
    import rhythm_fm_fxbus as fxbus
    core0=struct.unpack_from('<I',image,0x08031658-BASE)[0]-BASE
    count=int.from_bytes(image[core0:core0+3],'big')
    words=[int.from_bytes(image[core0+6+3*i:core0+9+3*i],'big') for i in range(count)]
    base,patch,labels=fxbus.build()
    require(words[base:base+len(patch)]==patch,'Core-0 FX bus routines missing from the image')
    for address,_,new in fxbus.sites(labels): require(words[address:address+len(new)]==new,f'Core-0 call at P:{address:#x} not redirected')
    folder=OUT/'validation';folder.mkdir(parents=True,exist_ok=True)
    img=folder/'core0-fx.bin';img.write_bytes(image[core0+6:core0+6+3*count])
    def run(name,lines):
        script=folder/f'{name}.txt';script.write_text('\n'.join(lines)+'\n')
        out=subprocess.run([str(TOOL),'run',str(script)],capture_output=True,text=True,check=True).stdout
        return {(m[1],int(m[2],16)):[int(x,16) for x in m[3].split()] for m in re.finditer(r'^([XY]) ([0-9a-f]+):((?: [0-9a-f]{6})+)$',out,re.M)}
    init=[f'LOADP {img} 0','BRIDGE 30000','MAX 2000000','X 3a 000020','IOY ffffd0 0']
    gains=['X %x %s'%(a,' '.join(['400000']*16)) for a in range(0xde,0xde+0x600,16)]
    spans=['Y %x %s'%(0x304f0+64*t+32*b,' '.join(['%06x'%((t+1)*0x10000)]*32)) for t in range(8) for b in range(2)]
    mix=init+gains+spans+['P 7000 05f420 00ffff 0461a0 0462a0 0463a0 0464a0 0465a0 0466a0 0467a0 0bf080 000367 000000','STOP 700c',
                          'X 37900 '+' '.join(['000000']*64),'Y 3770a 000000 000001 000001 000001 000001 000001 000001 000001',
                          'PC 7000','RUN','DUMPX 30430 2','DUMPX 37900 2']
    mem=run('fx-bus-mix',mix)
    require(mem[('X',0x37900)]==[0x8000]*2 and mem[('X',0x30430)]==[35*0x8000]*2,f'FX bus routing wrong: {mem}')
    stock=run('fx-bus-stock',mix[:-6]+['X 37900 '+' '.join(['000000']*64),'Y 3770a '+' '.join(['000000']*8),'PC 7000','RUN','DUMPX 30430 2','DUMPX 37900 2'])
    require(stock[('X',0x37900)]==[36*0x8000]*2 and stock[('X',0x30430)]==[0]*2,'Default routing does not send every track through the Grid FX')
    swap=init+['P 166b 00000c','P 1302 00000c',
               'P 7100 05f420 00ffff 0461a0 0462a0 0463a0 0bf080 %06x 000000'%labels['swap'],'P 7200 0bf080 %06x 000000'%labels['add'],
               'X 4ad8 '+' '.join('%06x'%(0x1000+i) for i in range(64)),'X 37900 '+' '.join('%06x'%(0x200000+i) for i in range(64)),
               'X 4b18 '+' '.join(['010000']*64),'STOP 7109','PC 7100','RUN','DUMPX 4ad8 64','DUMPX 37940 64','DUMPX 37900 64',
               'STOP 7203','PC 7200','RUN','DUMPX 4b18 64']
    mem=run('fx-bus-swap',swap)
    require(mem[('X',0x4ad8)]==[0x200000+i for i in range(64)] and mem[('X',0x37940)]==[0x1000+i for i in range(64)]
            and mem[('X',0x37900)]==[0]*64,'FX bus swap before the looper wrong')
    require(mem[('X',0x4b18)]==[0x11000+i for i in range(64)],'Dry master not added back after the Grid FX')
    return [{'case':'core0_fx_bus_routes_swaps_and_adds_back','routines':{k:hex(v) for k,v in labels.items()}}]


def drain(arm,symbols,queued=0):
    """One service call as if the DSP had consumed all but `queued` of the blocks."""
    nxt=arm.word(STATE+4);expected=(nxt-queued)%SEQ_WRAP
    for i in range(SLOT_COUNT):arm.memory[SLOTS[(expected+i)%SLOT_COUNT]]=1 if i<queued else 0
    arm.memory[CTRL]=MAGIC;arm.memory[CTRL+1]=expected
    arm.run(symbols['fm_tick'])


ROUTER=0x20010000
FRAME=0x20012000            # a stock frame buffer (42 colour words)


def setup_router(arm,view=4,page=0,held=0):
    """Fake stock UI router at ROUTER (r4/r0 of the hooked handlers): view
    property +0x14, Sample Rec view object +0x58 (page at +0xC), held-button
    bits +0xA0, Shift property +0x0C (not held)."""
    def store(a,buf,value):
        arm.cpu.mem_write(a,struct.pack('<5I',0,0,0,buf,buf));arm.cpu.mem_write(buf,struct.pack('<I',value))
    store(ROUTER+0x200,ROUTER+0x280,view);arm.cpu.mem_write(ROUTER+0x14,struct.pack('<I',ROUTER+0x200))
    store(ROUTER+0x300,ROUTER+0x380,0);arm.cpu.mem_write(ROUTER+0x0C,struct.pack('<I',ROUTER+0x300))
    arm.cpu.mem_write(ROUTER+0x400+0xC,bytes([page]));arm.cpu.mem_write(ROUTER+0x58,struct.pack('<I',ROUTER+0x400))
    arm.cpu.mem_write(ROUTER+0xA0,struct.pack('<I',held))


class StoreMachine(Machine):
    """Stock storage task, scratch, file-changed event and slot audition as
    recorded stubs; storage callbacks are invoked by the test, as the main
    task would deliver them."""
    def __init__(self,image,symbols):
        super().__init__(image,symbols)
        self.calls=[];self.pad_stock=0
    def hook(self,cpu,address,size,context):
        r=[cpu.reg_read(x) for x in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        sp=cpu.reg_read(UC_ARM_REG_SP)
        stack=struct.unpack('<2I',cpu.mem_read(sp,8))
        def path(a): return struct.unpack('<IIB',cpu.mem_read(a,9))
        result=0
        if address==0x08046098: self.calls.append(('delete',path(r[0]),r[1]))
        elif address==0x0804603A: self.calls.append(('read',path(r[0]),r[2]));result=5    # sync tag read: no file
        elif address==0x0804607A: self.calls.append(('write',path(r[0]),r[1],bytes(cpu.mem_read(r[2],r[3])),r[2],stack[0]))
        elif address==0x080460C8: self.calls.append(('close',path(r[0]),r[1]))
        elif address==0x0802EF6A: self.calls.append(('acquire',));result=0x20003F94
        elif address==0x0802EF98: self.calls.append(('release',))
        elif address==0x08035D62: self.calls.append(('changed',r[0],path(r[1])))
        elif address==0x080407F0: self.calls.append(('audition',r[0]))
        elif address==(self.symbols['rhythm_pad_original']&~1): self.pad_stock+=1
        elif address==0x0804FE9A: result=FRAME          # stock Sample Rec frame compose
        else: return super().hook(cpu,address,size,context)
        cpu.reg_write(UC_ARM_REG_R0,result)
        cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
    def callback(self,cb,path_address,kind,result,a,b):
        """cb(path*, kind, result, a, b, ctx) with b and ctx on the stack."""
        sp=0x2004FF00-8
        self.cpu.mem_write(sp,struct.pack('<2I',b,0))
        self.cpu.reg_write(UC_ARM_REG_SP,sp);self.cpu.reg_write(UC_ARM_REG_LR,0x08000001)
        for reg,v in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),(path_address,kind,result,a)):self.cpu.reg_write(reg,v)
        self.cpu.emu_start(cb|1,0x08000000,count=300_000_000,timeout=600_000_000)
        require(self.cpu.reg_read(UC_ARM_REG_PC)==0x08000000 and self.cpu.reg_read(UC_ARM_REG_SP)==sp,'Storage callback did not return cleanly')


def generator_cases(image,symbols,layout):
    """Sample Rec view, two-pad UI: pad 22 toggles generate mode (track press =
    new sound of that family, auditioned on that track; Shift + track = again),
    pad 23 toggles commit mode. Idle costs no DSP traffic."""
    cases=[]
    arm=StoreMachine(image,symbols);arm.run(0x08025688);setup_router(arm)
    def requested(): return struct.unpack('<8I',arm.cpu.mem_read(STATE+layout['requested'],32))
    def press(track):
        arm.run(symbols['fm_track_press'],ROUTER,track);return arm.cpu.reg_read(UC_ARM_REG_R0)
    for button in range(8):
        require(arm.shift_button(button)==('returned',None),f'Shift + track {button+1} reached the stock mask test')
    require(requested()==(0,)*8 and press(2)==0,'Generator active without a mode')
    arm.run(symbols['fm_pad'],ROUTER,22,100)
    require(arm.pad_stock==0,'Pad 22 reached the stock view')
    require(press(2)==1 and requested()==(0,0,1,0,0,0,0,0),'Generate mode: track press did not request a sound')
    other=StoreMachine(image,symbols);other.run(0x08025688);setup_router(other,view=0)
    other.run(symbols['fm_pad'],ROUTER,22,100);setup_router(other,view=0)
    other.run(symbols['fm_track_press'],ROUTER,2)
    require(other.cpu.reg_read(UC_ARM_REG_R0)==0 and other.pad_stock==1,'Pads/tracks outside the Sample Rec view not stock')
    cases.append({'case':'pad22_generate_mode_track_press_requests_a_sound','outside_view':'stock'})
    arm.bus=[];drain(arm,symbols)
    writes=[e for e in arm.bus if e[0]=='write']
    commits=[e for e in writes if e[1] in SLOTS and e[2]==1]
    require(len(commits)==4 and commits[0]==('write',SLOTS[0],1),'Audition did not start streaming (4 blocks per call)')
    require(arm.memory[SLOTS[0]+1]==0 and arm.memory[SLOTS[0]+2]==FRAMES|2<<TRACK_SHIFT,'Audition block header wrong (track 3)')
    first=[arm.memory[SLOTS[0]+3+i] for i in range(FRAMES)]
    require(any(first),'Audition block silent')
    require(arm.word(STATE+layout['generated'])==1 and arm.cpu.mem_read(STATE+layout['last_family'],1)[0]==2,'Generator state wrong')
    cases.append({'case':'audition_streams_to_its_track','track':3})
    for _ in range(3):drain(arm,symbols,queued=MAX_AHEAD)
    old=list(arm.bus);drain(arm,symbols,queued=MAX_AHEAD)
    require([e for e in arm.bus[len(old):] if e[0]=='write']==[],'Producer exceeded the queue')
    cases.append({'case':'audition_backpressure','max_ahead':MAX_AHEAD})
    for _ in range(4000):
        drain(arm,symbols)
        if not arm.cpu.mem_read(layout['drum_pool']+layout['drum_active'],1)[0]:break
    require(not arm.cpu.mem_read(layout['drum_pool']+layout['drum_active'],1)[0],'Audition never ended (2 s cap)')
    blocks=arm.word(STATE+12)
    require(0<blocks<=3000,f'Audition length {blocks} blocks outside 0..2 s')
    arm.bus=[]
    for _ in range(64): arm.run(symbols['fm_tick'])
    require(all(e[0]=='read' and CTRL<=e[1]<CTRL+CTRL_WORDS for e in arm.bus) and len(arm.bus)<=8,
            f'Idle service accessed the DSP beyond its 1-in-128 telemetry reads: {arm.bus[:9]}')
    cases.append({'case':'idle_after_audition_costs_no_dsp_traffic','audition_blocks':blocks})
    # Shift + track in generate mode: the same sound again, bit-exact.
    arm.shift_button(2);arm.memory={k:v for k,v in arm.memory.items() if not SLOTS[0]<=k<SLOTS[0]+SLOT_WORDS}
    nxt=arm.word(STATE+4);slot=SLOTS[nxt%SLOT_COUNT]
    drain(arm,symbols)
    again=[arm.memory[slot+3+i] for i in range(FRAMES)]
    require(again==first and arm.word(STATE+layout['generated'])==1,'Shift + track did not replay the same sound')
    cases.append({'case':'shift_track_in_generate_mode_replays_bit_exact'})
    # LEDs: pad 22 green in generate mode, pad 23 red in commit mode, stock otherwise.
    def frame():
        arm.cpu.mem_write(FRAME,struct.pack('<42H',*([0x09]*42)))
        arm.run(symbols['fm_samplerec_frame'],ROUTER+0x400)
        return struct.unpack('<42H',arm.cpu.mem_read(FRAME,84))
    f=frame();require(f[22]==0x22 and f[23]==0x54 and f[21]==0x38 and f[20]==0x09,'Generate mode LED wrong')
    arm.run(symbols['fm_pad'],ROUTER,23,100)
    f=frame();require(f[22]==0x24 and f[23]==0x55,'Commit mode LED wrong (modes not exclusive?)')
    arm.run(symbols['fm_pad'],ROUTER,23,100)
    f=frame();require(f[22]==0x24 and f[23]==0x54,'Mode LEDs not back to dim')
    require(image[0x08067A24-BASE:0x08067A28-BASE]==struct.pack('<I',symbols['fm_samplerec_frame']),'Frame vtable word not patched')
    cases.append({'case':'mode_leds_on_pads_22_23_via_frame_vtable','vtable_word':'0x08067a24'})
    # The first audition block of family 1 (kick) equals the host build of the same source.
    arm=Machine(image,symbols);arm.run(0x08025688)
    arm.run(symbols['fm_randomize_request'],0,0);drain(arm,symbols)
    kick=[arm.memory[SLOTS[0]+3+i] for i in range(FRAMES)]
    host=subprocess.run([str(OUT/'host-render'),str(OUT/'generator-preview.pcm'),str(OUT/'first-host-block.pcm')],
                        capture_output=True,text=True,check=True)
    require((OUT/'first-host-block.pcm').read_bytes()==b''.join(v.to_bytes(3,'big') for v in kick),
            'Host and ARM generator audio differ')
    pcm=(OUT/'generator-preview.pcm').read_bytes()
    with wave.open(str(OUT/'generator-preview.wav'),'wb') as wav:
        wav.setnchannels(1);wav.setsampwidth(3);wav.setframerate(48000);wav.writeframes(pcm)
    lines=[l for l in host.stdout.splitlines() if l.startswith('family')]
    require(len(lines)==GEN_FAMILIES and all(int(l.split()[7])>0 for l in lines),'A family generated silence')
    cases.append({'case':'host_and_arm_generator_audio_bit_identical','host_render':host.stdout.strip()})
    arm.cpu.mem_write(0x20010000,struct.pack('<II',0x20010010,6))
    arm.cpu.mem_write(0x20010010,bytes.fromhex('f07e7f0601f7'))
    before=list(arm.bus)
    report=bytes.fromhex(arm.run(0x08026F0C,0,0x20010000)[0]['sysex'])
    values=[sum(report[6+5*i+k]<<(7*k) for k in range(5)) for i in range(REPORT_FIELDS)]
    require(len(report)==REPORT_BYTES and report[5]==REPORT_VERSION and values[16]==1 and values[17]&255==0 and values[19]>0,
            'Report did not capture the generated sound')
    require(arm.bus==before,'Identity callback accessed the DSP bus')
    (OUT/'runtime-report-validation.json').write_text(json.dumps({'report_hex':report.hex(),'generated':values[16],
        'family':values[17]&255,'seed':values[18]},indent=2)+'\n')
    cases.append({'case':'runtime_report_without_dsp_bus_calls','version':REPORT_VERSION})
    return cases


def store_cases(image,symbols,layout):
    """Commit mode (pad 23): hold a track button with a sound and press a slot
    pad: the sound is written as a WAV into that slot through the stock
    storage task, then the stock file-changed event reloads it."""
    cases=[]
    for family in (2,):   # one short sound (hi-hat) keeps the emulated store quick
        arm=StoreMachine(image,symbols);arm.run(0x08025688)
        setup_router(arm)
        arm.run(symbols['fm_pad'],ROUTER,22,100)            # generate mode
        arm.run(symbols['fm_track_press'],ROUTER,family)     # new sound
        for _ in range(4000):
            drain(arm,symbols)
            if arm.word(STATE+layout['generated']) and not arm.cpu.mem_read(layout['drum_pool']+layout['drum_active'],1)[0]:break
        arm.run(symbols['fm_pad'],ROUTER,23,100)            # commit mode
        arm.run(symbols['fm_track_press'],ROUTER,family)
        require(arm.cpu.reg_read(UC_ARM_REG_R0)==1,'Commit mode: track press not kept in the view')
        setup_router(arm,page=1,held=1<<family)
        arm.calls=[];arm.run(symbols['fm_pad'],ROUTER,3,100)
        require(arm.pad_stock==0,'Store pad press reached the stock view')
        require([c[0] for c in arm.calls]==['acquire','delete'] and arm.calls[1][1]==(4,19,0xFE),f'Commit start wrong: {arm.calls}')
        path_address=STATE+layout['path']
        delete_cb=arm.calls[1][2]
        arm.calls=[];arm.callback(delete_cb,path_address,2,5,0,0)   # 5: there was no file
        data=b'';guard=0;auditioned=0
        while True:
            call=arm.calls[-1]
            if call[0]!='write':break
            require(call[1]==(4,19,0xFE) and call[2]==len(data) and call[4]==0x20003F94,'Write chunk misplaced')
            data+=call[3];guard+=1;require(guard<200,'Too many chunks')
            # as on the instrument: the commit-mode replay keeps playing through
            # the audio service between storage steps (0.2.3 baked it into the sample)
            if arm.cpu.mem_read(layout['drum_pool']+layout['drum_active'],1)[0]: auditioned+=1
            drain(arm,symbols)
            arm.callback(call[5],path_address,1,0,call[4],len(call[3]))
        require([c[0] for c in arm.calls[-2:]]==['release','close'],f'No release/close after the last chunk: {arm.calls[-3:]}')
        close_cb=arm.calls[-1][2]
        arm.calls=[];arm.callback(close_cb,path_address,5,0,0,0)
        require([c[0] for c in arm.calls]==['changed','audition'] and arm.calls[0][2][:2]==(4,19) and arm.calls[1][1]==19,
                f'Reload event wrong: {arm.calls}')
        n=struct.unpack('<I',data[60:64])[0]//2
        require(data[:4]==b'RIFF' and struct.unpack('<I',data[4:8])[0]==len(data)-8 and data[8:16]==b'WAVEfmt '
                and struct.unpack('<IHHIIHHH',data[16:38])==(36,1,1,48000,96000,2,16,18) and data[38:42]==b'PHK1'
                and data[56:60]==b'data' and len(data)==64+2*n and n>256 and data[-512:]==bytes(512),'Stored WAV malformed')
        require(data[46]==family and data[43]==0xFF,'Stored WAV tag wrong')
        samples=struct.unpack(f'<{n}h',data[64:])
        peak=max(abs(x) for x in samples)
        require(28000<peak<=29200,f'Stored sound not normalised to -1 dBFS (peak {peak})')
        subprocess.run([str(OUT/'host-render'),'--store',str(family),str(OUT/f'store-host-{family}.wav')],check=True,capture_output=True)
        require(auditioned>0,'The replay audition was not running during the store (test lost its point)')
        require((OUT/f'store-host-{family}.wav').read_bytes()==data,'Host and ARM stored WAV differ (audition leaked into the store?)')
        (OUT/f'store-arm-{family}.wav').write_bytes(data)
        cases.append({'case':'commit_mode_hold_track_and_press_pad_stores_wav_and_reloads','family':family+1,'slot':19,'samples':n-256,'chunks':guard,'peak':peak,'audition_running_during_store_chunks':auditioned})
        # a second store while one is still running is refused (pad passed on, pad 23 flashes)
        arm.calls=[];arm.run(symbols['fm_pad'],ROUTER,4,100)
        require([c[0] for c in arm.calls]==['acquire','delete'],'Second store did not start after the first finished')
        before=arm.pad_stock;arm.run(symbols['fm_pad'],ROUTER,5,100)
        require(arm.pad_stock==before+1,'A refused store swallowed the pad')
    cases.append({'case':'busy_store_refused_and_pad_passed_on'})
    # commit mode with no sound on the held track: refused, pad passed on; other pads stock
    arm=StoreMachine(image,symbols);arm.run(0x08025688)
    setup_router(arm);arm.run(symbols['fm_pad'],ROUTER,23,100)
    setup_router(arm,held=1<<4);arm.run(symbols['fm_pad'],ROUTER,3,100)
    setup_router(arm,held=0);arm.run(symbols['fm_pad'],ROUTER,3,100)
    setup_router(arm,view=0,held=1);arm.run(symbols['fm_pad'],ROUTER,3,100)
    require(arm.pad_stock==3 and not arm.calls,'Ordinary or refused pad presses not passed to stock')
    cases.append({'case':'refused_and_ordinary_pad_presses_stay_stock'})
    return cases

def kit_cases(image,symbols,layout):
    """Kit pad (21): idle LEDs dim; a tap plans 16 sounds for the current page
    from the tags of the pack (here: none) and stores them (the first one
    checked against the host build, which grows a whole 128-sound kit); a
    1.5 s hold clears the page."""
    cases=[]
    arm=StoreMachine(image,symbols);arm.run(0x08025688);setup_router(arm,page=0)
    def frame():
        arm.cpu.mem_write(FRAME,struct.pack('<42H',*([0x09]*42)))
        arm.run(symbols['fm_samplerec_frame'],ROUTER+0x400)
        return struct.unpack('<42H',arm.cpu.mem_read(FRAME,84))
    f=frame();require(f[21:24]==(0x38,0x24,0x54) and f[20]==0x09,f'Idle pad LEDs not dim: {f[20:24]}')
    cases.append({'case':'pads_21_22_23_dim_when_idle'})
    arm.calls=[];arm.run(symbols['fm_pad'],ROUTER,21,100)
    require(arm.pad_stock==0 and not arm.calls,'Kit pad press not consumed or started early')
    arm.run(symbols['fm_pad_release'],ROUTER,21,0)
    reads=[c for c in arm.calls if c[0]=='read']
    require(sorted(c[1][1] for c in reads)==list(range(16,128)) and all(c[2]==0x20003F94 for c in reads),
            'Kit planning did not read the tags of every other slot')
    rest=[c for c in arm.calls if c[0]!='read']
    require([c[0] for c in rest]==['acquire','release','acquire','delete'] and rest[-1][1]==(4,0,0xFE),
            f'Kit did not start at slot 1: {rest}')
    require(image[0x080672E8-BASE:0x080672EC-BASE]==struct.pack('<I',symbols['fm_pad_release']),'Pad release vtable word not patched')
    host=OUT/'kit-host';host.mkdir(exist_ok=True)
    run=subprocess.run([str(OUT/'host-render'),'--kit',str(host)],check=True,capture_output=True,text=True)
    path_address=STATE+layout['path']
    delete_cb=arm.calls[-1][2]
    arm.calls=[];arm.callback(delete_cb,path_address,2,5,0,0)
    data=b''
    while arm.calls[-1][0]=='write':
        call=arm.calls[-1];data+=call[3]
        arm.callback(call[5],path_address,1,0,call[4],len(call[3]))
    close_cb=arm.calls[-1][2]
    arm.calls=[];arm.callback(close_cb,path_address,5,0,0,0)
    require(data==(host/'slot001.wav').read_bytes(),'Kit sound 1 differs from the host build')
    require(data[38:42]==b'PHK1' and data[42]==0 and data[44]==0,'Kit sound 1 tag is not a plain kick')
    require(arm.calls[0][0]=='changed' and arm.calls[0][2][:2]==(4,0) and 'audition' not in [c[0] for c in arm.calls],
            'Kit sound not reloaded, or auditioned')
    require([c[0] for c in arm.calls[-2:]]==['acquire','delete'] and arm.calls[-1][1]==(4,1,0xFE),'Kit did not continue')
    f=frame();require(f[21] in (0x37,0x38),'Kit pad not blinking while making a kit')
    whole=[l for l in run.stdout.splitlines() if l.startswith('whole kit:')][0]
    cases.append({'case':'kit_tap_plans_16_from_the_pack_and_stores_them','host':whole})
    arm=StoreMachine(image,symbols);arm.run(0x08025688);setup_router(arm,page=2)
    arm.cpu.mem_write(0xe0001004,struct.pack('<I',1000))
    arm.run(symbols['fm_pad'],ROUTER,21,100)
    arm.calls=[];frame();require(not arm.calls,'Clear started before the hold time')
    arm.cpu.mem_write(0xe0001004,struct.pack('<I',1000+150_000_000))
    arm.calls=[];frame()
    require([c[0] for c in arm.calls]==['delete'] and arm.calls[0][1]==(4,32,0xFE),f'Hold did not start the clear at slot 33: {arm.calls}')
    deleted=[]
    for _ in range(16):
        cb=arm.calls[-1][2];deleted.append(arm.calls[-1][1][1])
        arm.calls=[];arm.callback(cb,path_address,2,0,0,0)
        require(arm.calls[0][0]=='changed' and arm.calls[0][2][1]==deleted[-1],'Cleared slot not reloaded')
    require(deleted==list(range(32,48)) and len(arm.calls)==1,'Clear did not cover exactly the page')
    arm.run(symbols['fm_pad_release'],ROUTER,21,0)
    require(not arm.calls[1:],'Release after a clear made a kit')
    cases.append({'case':'kit_hold_clears_the_page','first_slot':33})
    return cases


def main():
    TEST.mkdir(parents=True,exist_ok=True)
    layout=host_harness()
    require(STATE+layout["state"]<=0x4000 and layout['report_bytes']==REPORT_BYTES,
            'Host State layout exceeds ITCM or report size mismatch')
    image=(OUT/f'{STEM}.bin').read_bytes()
    validate_segments(image,unpack_stream(image)[1])
    require(len(image)%4==0,'Image size must be word-aligned for bootloader CRC')
    decoded=ct_fw.decode((OUT/f'{STEM}.syx').read_bytes())
    require(decoded['image']==image and decoded['product']==0x63,'Firmware container invalid')
    stock=(ROOT/'firmware/official/circuitrhythm-firmware-5706.bin').read_bytes()
    require(image[0x1c8:0x1ee]!=stock[0x1c8:0x1ee], 'Bootloader installation gate: unchanged version block')
    require(image[0x1e8:0x1ee]==INSTALL_MARKER and image[0x1c8:0x1e8]==stock[0x1c8:0x1e8],
            'Installation marker changed stock product/version fields')
    identity=image[IDENTITY_ADDRESS-BASE:IDENTITY_ADDRESS-BASE+17]
    require(identity[14]*128+identity[15]==IDENTITY_BUILD and identity[:14]==stock[IDENTITY_ADDRESS-BASE:IDENTITY_ADDRESS-BASE+14],
            'Custom standard identity response missing or product changed')
    # The stock sampler is intact: note path, core-1 sampler code and tables.
    for address in (0x08040778,0x0804083C,0x08040814,0x08039C7C):
        require(image[address-BASE:address-BASE+4]==stock[address-BASE:address-BASE+4],f'Stock sampler code changed at {address:#x}')
    maps,segments,_,_=unpack_stream(image)
    smaps,ssegments,_,_=unpack_stream(stock)
    require(all(maps['P'].get(a)==smaps['P'].get(a) for a in (0x122,0x234,0x2e4)) and
            any(g['space']=='X' and g['address']==0x200 for g in segments),'Native sampler voices or tables removed')
    _,symbols=elf(OUT/'extension.elf')
    continuation=boot_continuation(image,state_address=STATE,trampoline=symbols['rhythm_boot_original']&~1)
    reference=boot_continuation(stock)
    require(continuation['continuation']==reference['continuation'] and continuation['fm_magic']==hex(MAGIC),
            'Real ARM boot trampoline did not preserve native continuation')
    arm=Machine(image,symbols)
    before=bytes(arm.cpu.mem_read(0x20000000,0x4f760))
    arm.run(0x08025688)
    require(arm.word(STATE)==MAGIC and arm.word(0x20002B54)==0x2004F760 and arm.boot_calls==1,
            'Boot hook failed to initialize ITCM without shrinking the native heap')
    require(bytes(arm.cpu.mem_read(0x20000000,0x4f760))==before,'Initialization wrote into native SRAM/heap')
    arm_cases=[{'case':'patched_boot_uses_itcm_and_preserves_entire_native_heap'}]
    startup=Machine(image,symbols)
    startup.cpu.mem_write(0x20000000,initialize_ram(image,0x08025688))
    startup.run(0x08025688)
    require(startup.word(STATE)==MAGIC and startup.word(0x20002B54)==0x2004F760,
            'Actual reset-produced RAM does not initialize the extension')
    arm_cases.append({'case':'actual_reset_ram_then_patched_boot_initializes'})
    inquiry=Machine(image,symbols)
    inquiry.run(0x08025688)
    inquiry.cpu.mem_write(0x20010000,struct.pack('<II',0x20010010,6))
    inquiry.cpu.mem_write(0x20010010,bytes.fromhex('f07e7f0601f7'))
    replies=inquiry.run(0x08026F0C,0,0x20010000)
    require(len(replies)==2 and replies[-1]['sysex']==identity.hex(),
            'Actual standard identity handler did not return custom build')
    status=bytes.fromhex(replies[0]['sysex'])
    require(len(status)==REPORT_BYTES and status[:6]==bytes([0xf0,0x7d,0x52,0x46,0x4d,REPORT_VERSION]) and status[-1]==0xf7,
            'Runtime status packet missing or not ordered before standard identity')
    require(all(x<128 for x in status[1:-1]),'Runtime status contains non-MIDI bytes')
    arm_cases.append({'case':'actual_identity_handler_reports_custom_build','build':IDENTITY_BUILD,'reply_hex':identity.hex()})
    for track in range(8):
        events=arm.run(NOTE_ON,track,60+track,100)
        pairs=[e for e in events if e['stub']=='queue_pair']
        require(pairs and pairs[-1]['words']==[hex(0x900000|(track<<16)|(100<<8)|track),hex(track<<16)],
                f'Track {track+1} note no longer reaches the stock sampler')
    arm_cases.append({'case':'all_eight_tracks_play_the_stock_sampler'})
    arm_cases+=generator_cases(image,symbols,layout)
    arm_cases+=store_cases(image,symbols,layout)
    arm_cases+=kit_cases(image,symbols,layout)
    arm_cases+=plock_cases(image,symbols,layout)
    arm_cases+=gain_glide_case(image)
    arm_cases+=fx_bus_cases(image)
    blocked=Machine(image,symbols)
    blocked.cpu.mem_write(0xe000ef90,struct.pack('<I',0))
    blocked.run(0x08025688)
    require(blocked.word(STATE)!=MAGIC and blocked.word(0x20002B54)==0x2004F760,'Unavailable ITCM guard failed')
    blocked.cpu.mem_write(0x20010000,struct.pack('<II',0x20010010,6))
    blocked.cpu.mem_write(0x20010010,bytes.fromhex('f07e7f0601f7'))
    failure=blocked.run(0x08026F0C,0,0x20010000)
    reply=bytes.fromhex(failure[0]['sysex'])
    require(reply[14]*128+reply[15]==MEMORY_FAILURE_BUILD,'Unavailable ITCM did not report memory-failure identity')
    require(blocked.shift_button(0)==('returned',None) and blocked.word(STATE+layout['requested'])!=1,
            'Shift + track without initialized state misbehaved')
    arm_cases.append({'case':'unavailable_itcm_preserves_heap_and_reports_15713'})
    full_heap=Machine(image,symbols)
    full_heap.cpu.mem_write(0x20002B58,struct.pack('<I',0x2004F700))
    full_heap.run(0x08025688)
    require(full_heap.word(STATE)==MAGIC and full_heap.word(0x20002B58)==0x2004F700,
            'ITCM initialization depends on native heap headroom')
    arm_cases.append({'case':'itcm_initializes_with_only_96_bytes_native_heap_remaining'})

    program=TEST/'patched-core1-P.bin'
    program.write_bytes(b''.join(maps['P'].get(a,0).to_bytes(3,'big') for a in range(max(maps['P'])+1)))
    init=['CORE1',f'LOADP {program} 0']+data_lines(maps)+['BRIDGE 30000','MAX 400000','X 1 20','X 2 10']
    violations=[('program-load','P 1000 00000c'),('program-fetch','PC 1000\nRUN'),
                ('x-load','X 3000 0'),('y-load','Y 2000 0')]
    for space,address in [('X',0x3000),('Y',0x2000)]:
        code=T(f'move #>${address:x},r0')+T(f'move {space.lower()}:(r0),a')
        violations.append((space.lower()+'-read',put('P',0xf00,code)+'\nPC f00\nSTOP f10\nRUN'))
    for name,body in violations:
        fixture=TEST/('bounds-'+name+'.txt');fixture.write_text('CORE1\nMAX 100\n'+body+'\n')
        result=subprocess.run([str(TOOL),'run',str(fixture)],capture_output=True,text=True)
        require(result.returncode==2 and 'CORE1 RAM VIOLATION' in result.stderr,
                'DSP physical bounds guard failed: '+name)
    def consume(lines,name):
        script=TEST/f'{name}.txt';script.write_text('\n'.join(init+lines)+'\n')
        return parse(subprocess.run([str(TOOL),'run',str(script)],capture_output=True,text=True,check=True).stdout)
    def control_words(magic,seq):
        return [magic,seq]+[0]*(CTRL_WORDS-3)+[SLOTS[seq%SLOT_COUNT]]
    def signed(x): return x-0x1000000 if x&0x800000 else x
    payload=[((i*0x18531)^0x12345)&0x3fffff for i in range(FRAMES)]   # positive 22-bit fixture
    payload[1]=0x7fffff                                                # saturates on the fixed base below
    dsp_cases=[]
    for bank in (0,1):
        for case in ('valid','empty','partial','stale','future','future_wrap','wrong_length','wrap','disabled','unsupported_length'):
            seq=SEQ_WRAP-2 if case=='future_wrap' else SEQ_WRAP-1 if case=='wrap' else bank
            slot=SLOTS[seq%SLOT_COUNT]
            track=(5+bank) if case!='wrap' else 7
            ready=0 if case in ('empty','partial') else 1
            slot_seq=(seq-2)%SEQ_WRAP if case=='stale' else seq
            if case in ('future','future_wrap'):slot_seq=(seq+SLOT_COUNT)%SEQ_WRAP
            length=(FRAMES-1 if case=='wrong_length' else FRAMES)|track<<TRACK_SHIFT
            lines=[put('Y',0x304F0,[0x010000]*512), f'IOY ffffd0 {bank}',
                   put('Y',CTRL,control_words(0 if case=='disabled' else MAGIC,seq)),
                   put('Y',slot,[ready,slot_seq,length]+payload),
                   'X 1 '+('10' if case=='unsupported_length' else '20')]
            lines += [f'PC {PBASE+2:x}','STOP 8a6','RUN','DUMPY 304f0 512',
                      f'DUMPY {CTRL:x} {CTRL_WORDS}',f'DUMPY {slot:x} {SLOT_WORDS}']
            memory=consume(lines,f'consumer-{bank}-{case}')
            want=[0x010000]*512
            enabled=case not in ('disabled','unsupported_length')
            valid=case in ('valid','wrap')
            if enabled and valid:
                start=64*track+32*bank
                want[start:start+32]=[min(0x010000+x,0x7fffff) for x in payload]
            require(memory['Y:304f0']==want,f'DSP output wrong: {case}, bank {bank}')
            control=memory[f'Y:{CTRL:x}']; slot_words=memory[f'Y:{slot:x}']
            require(control[1]==((seq+1)%SEQ_WRAP if enabled and valid else seq),'Wrong DSP sequence advance')
            require(control[CTRL_READ_SLOT-CTRL]==SLOTS[(seq+1)%SEQ_WRAP%SLOT_COUNT if enabled and valid else seq%SLOT_COUNT],
                    'DSP read slot did not advance with the sequence')
            require(control[2]==int(enabled and not valid),'Wrong miss count')
            require(control[7]==int(enabled and valid),'Accepted block counter incorrect')
            require(slot_words[3:]==payload,'DSP mutated producer payload')
            require(slot_words[0]==(ready if case in ('disabled','unsupported_length','future','future_wrap') else 0),'Wrong slot release')
            dsp_cases.append({'bank':bank,'case':case,'track':track+1,'added_with_saturation':valid,'other_spans_untouched':True})
    # Register preservation (the consumer saves what it touches; the native
    # pass before it is stock). Redirect only the emulator continuation.
    registers={'sr':0x300,'a0':0x123456,'a1':0x654321,'a2':0x12,'b0':0x0a0b0c,'b1':0x5a5a5a,'b2':0x21,
               'x0':0x246810,'x1':0x112233,'y0':0x135790,'y1':0x445566}
    for i in range(8): registers[f'r{i}']=0x1230+i; registers[f'n{i}']=0x40+i
    for i in range(8): registers[f'm{i}']={1:0x1ff,5:0xff}.get(i,0xffff)
    prepare=[]
    for name,value in registers.items():
        if name!='sr': prepare+=T(f'move #>${value:x},{name}')
    prepare+=T('move #>$300,sr')
    prepare += [0x0af080,PBASE+2]
    snapshot=[]
    for index,name in enumerate(registers): snapshot+=T(f'move {name},x:>${0x2c00+index:x}')
    snap_at=0x0F40+len(prepare)
    require(snap_at+len(snapshot)<0x1000,'Register test stubs exceed P memory')
    lines=[put('P',0x0F40,prepare),put('P',snap_at,snapshot),put('P',0x8a6,[0x0af080,snap_at]),
           put('Y',CTRL,control_words(MAGIC,0)),put('Y',SLOTS[0],[1,0,FRAMES|3<<TRACK_SHIFT]+payload),
           'IOY ffffd0 0','PC f40',f'STOP {snap_at+len(snapshot):x}','RUN',
           f'DUMPX 2c00 {len(registers)}']
    script=TEST/'register-preservation.txt';script.write_text('\n'.join(init+lines)+'\n')
    result=subprocess.run([str(TOOL),'run',str(script)],capture_output=True,text=True,check=True).stdout
    m=re.search(r'^X 002c00:((?: [0-9a-f]{6})+)$',result,re.M)
    require(m is not None,'Register snapshot missing')
    require([int(x,16) for x in m[1].split()]==list(registers.values()),'DSP consumer clobbered caller registers')
    # Enter through the actual patched P:$11D call with inactive sampler voices:
    # the native pass clears the spans, then the audition block is added.
    lines=[put('Y',0x304F0,[0x123456]*512),put('Y',CTRL,control_words(MAGIC,0)),put('Y',SLOTS[0],[1,0,FRAMES|3<<TRACK_SHIFT]+payload),'IOY ffffd0 0']
    for track in range(8): lines += [f'X {0x44+16*track:x} 0']
    setup=T('move #>$ffff,m0')
    for i in range(1,8): setup+=T(f'move #>$ffff,m{i}')
    setup += [0x0af080,0x11d]
    lines += [put('P',0x0F40,setup),'PC f40','STOP 8a6','RUN','DUMPY 304f0 512']
    memory=consume(lines,'native-sampler-then-audition')
    want=[]
    for track in range(8):want+=(payload if track==3 else [0]*32)+[0x123456]*32
    require(memory['Y:304f0']==want,'Native sampler pass / audition mixer did not compose')
    words=len(open(OUT/'dsp-extension.bin','rb').read())//3
    require(PBASE+words<=0xF40,'DSP consumer overlaps test stub area')
    report={'image_sha256':ct_fw.sha256(image),'arm_cases':arm_cases,'dsp_cases':dsp_cases,
            'dsp_consumer_words':words,'ram_layout':layout,
            'physical_dsp_layout_checked':True,'dsp_runtime_bounds_enforced':True,
            'dsp_bounds_negative_cases':[name for name,_ in violations],
            'actual_arm_boot_trampoline':continuation,'image_word_aligned':True,
            'installation_version_block_changed':True,'installation_marker':INSTALL_MARKER.decode(),
            'custom_identity_build':IDENTITY_BUILD,'container_crc_roundtrip':True,
            'actual_dsp_consumer_executed':True,'dsp_register_preservation':True,
            'actual_patched_sampler_call_composed':True,'hardware_tested':False,
            'limitations':['ARM bus and locks are stubs; no hardware throughput measurement.',
                           'Shift + track is entered at the hooked Shift branch; the full UI scan/controller path is not emulated.',
                           'Storing a generated sound into a sample slot is not implemented yet.',
                           'Consumer starts after native sampler pass; full pipeline scheduling not simulated.']}
    path=OUT/'validation.json';path.write_text(json.dumps(report,indent=2)+'\n')
    manifest_path=OUT/'manifest.json'
    manifest=json.loads(manifest_path.read_text())
    require(manifest['image_sha256']==report['image_sha256'],'Validation manifest mismatch')
    manifest['release_status']='offline validated experimental candidate; hardware untested'
    manifest['validation_report']='validation.json'
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'{len(arm_cases)} ARM integration checks, {len(dsp_cases)} DSP consumer cases, register preservation, native sampler composition and container roundtrip passed; {path}')


if __name__=='__main__': main()
