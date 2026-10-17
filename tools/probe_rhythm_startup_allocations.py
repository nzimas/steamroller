"""Exploratory offline startup allocation trace; no full-boot claim.

Hardware completion is fixture-driven. The standalone constructor deliberately
stops at the same uninitialized worker interface in stock and custom images.
It runs on the pre-scheduler stack, not the real application task stack.
"""
import struct,collections,json
from pathlib import Path
from compare_platform import initialize_ram,ROOT
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_UNMAPPED,UC_HOOK_MEM_WRITE,UC_HOOK_INTR
from unicorn.arm_const import UC_ARM_REG_PC,UC_ARM_REG_LR,UC_ARM_REG_R0,UC_ARM_REG_SP
for label,path in [('stock',ROOT/'firmware/official/circuitrhythm-firmware-5706.bin'),('fm',ROOT/'build/rhythm-fm/rhythm-fm-0.1.3.bin')]:
 image=path.read_bytes();cpu=initialize_ram(image,0x8025688,return_cpu=True)
 counts=collections.Counter();events=[];allocs=[];ticks=[0];minimum=[cpu.reg_read(UC_ARM_REG_SP)];stopped=[]
 def code(uc,a,n,c):
  counts[a]+=1;minimum[0]=min(minimum[0],uc.reg_read(UC_ARM_REG_SP))
  if a in (0x8025a98,):allocs.append((hex(uc.reg_read(UC_ARM_REG_LR)),uc.reg_read(UC_ARM_REG_R0)))
  if a==0x8026134:stopped.append('before scheduler start');uc.emu_stop()
  if a==0x8025ab4:stopped.append('heap overflow');uc.emu_stop()
  if a in (0x8025cc4,0x8023480,0x8041c2a,0x802490c,0x8046414,0x802b66c):uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
  if a==0x8023bb6:
   from unicorn.arm_const import UC_ARM_REG_R4
   ptr=struct.unpack('<I',uc.mem_read(uc.reg_read(UC_ARM_REG_R4)+0x30,4))[0]
   uc.mem_write(ptr,bytes([uc.mem_read(ptr,1)[0]&~2]))
  if a in (0x8023668,0x8024068):
   ticks[0]+=1000;uc.reg_write(UC_ARM_REG_R0,ticks[0]);uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
 def invalid(uc,access,a,n,v,c):
  events.append(('access',hex(a),n,hex(uc.reg_read(UC_ARM_REG_PC))))
  if 0x40000000<=a<0x50000000 or 0xa0000000<=a<0xa0010000:
   page=a&~4095;uc.mem_map(page,4096);events.append(hex(page));return True
  return False
 def intr(uc,i,c):stopped.append('interrupt '+str(i));uc.emu_stop()
 cpu.hook_add(UC_HOOK_CODE,code);cpu.hook_add(UC_HOOK_MEM_UNMAPPED,invalid);cpu.hook_add(UC_HOOK_INTR,intr)
 try:cpu.emu_start(0x8025689,0,count=500000,timeout=5000000)
 except Exception as e:stopped.append(str(e))
 print('PRE',label,stopped,hex(cpu.reg_read(UC_ARM_REG_PC)),list(map(hex,struct.unpack('<II',cpu.mem_read(0x20002b54,8)))),'alloc',len(allocs),'minsp',hex(minimum[0]),flush=True)
 stopped.clear();counts.clear();events.clear()
 def main_hook(uc,a,n,c):
  if a==0x8025864:stopped.append('main loop reached');uc.emu_stop()
  if a==0x8026596:
   uc.reg_write(UC_ARM_REG_R0,1);uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
  if a in (0x8031110,0x80311a6,0x8024138,0x8024150):uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
 cpu.hook_add(UC_HOOK_CODE,main_hook)
 try:cpu.reg_write(UC_ARM_REG_LR,0x8010201);cpu.emu_start(0x8027981,0x8010200,count=1500000,timeout=10000000)
 except Exception as e:stopped.append(str(e))
 print(json.dumps({'case':label,'stop':stopped,'PC':hex(cpu.reg_read(UC_ARM_REG_PC)),'lr':hex(cpu.reg_read(UC_ARM_REG_LR)),'heap':list(map(hex,struct.unpack('<II',cpu.mem_read(0x20002b54,8)))),'lowest_sp':hex(minimum[0]),'allocations':len(allocs),'last_allocs':allocs[-8:],'hot':[(hex(a),n) for a,n in counts.most_common(5)],'mapped_peripherals':events},indent=2),flush=True)
