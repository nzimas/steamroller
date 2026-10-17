# SPDX-License-Identifier: GPL-3.0-only
"""DSP core-0 "FX bus" patch (Rhythm FM 0.3.0): per-track Grid FX.

Stock core 0 mixes every track into the main bus Y:$30430 (P:$367, per-track
loop P:$37F), builds the master X:$4AD8 from it (P:$1A11, + input monitor,
audition, reverb/delay returns), and runs the Grid FX chain on that master
(looper P:$166B on $4AD8, copy to $4B18, reverser … vinyl P:$1302 on $4B18).
The patch (56 words in the unused interrupt vectors P:$80..$B7):
  FXSEL (P:$38A): a track whose route word Y:$3770A+t is 0 mixes into the FX
    bus $37900 instead of the main bus (0 = default: every track in the FX bus,
    so the stock Grid FX pads keep working on all tracks);
  SWAP  (P:$239, before the looper): dry master -> DRY $37940, FX bus -> master,
    FX bus cleared;
  ADD   (P:$27A, after vinyl): master += DRY, before the master dynamics.
All hand-encoded (the analysis assembler has no long branches); verified by
disassembly and by tools/validate_rhythm_fm.py in dsptool-io."""
FXB, DRY, ROUTE = 0x37900, 0x37940, 0x3770A
def build():
    p=[]; base=0x80
    def at(): return base+len(p)
    # FXSEL: r1 = the bus for track y:$307b3 (route word 0 -> FX bus, else main)
    fxsel=at()
    p += [0x69f000,0x0307b3, 0x000000, 0x0b71ce,ROUTE, 0x200003, 0x61f400,FXB]
    jeq=len(p); p += [0x0af0aa,0]          # jeq end
    p += [0x61f400,0x030430]
    p[jeq+1]=at(); p += [0x00000c]          # end: rts
    # SWAP: dry master ($4ad8) -> DRY, FX bus -> $4ad8, FX bus cleared; then the stock looper
    swap=at()
    p += [0x0461a0,0x0462a0,0x0463a0, 0x61f400,0x004ad8, 0x62f400,DRY, 0x63f400,FXB, 0x20001b]
    do=len(p); p += [0x063a00,0]
    body=[0x56e100,0x565a00,0x56e300,0x565900,0x575b00]*2
    p += body; p[do+1]=at()-1
    p += [0x0af080,0x00166b]                # jmp $166b
    # ADD: vinyl (stock), then the master ($4b18) += DRY
    add=at()
    p += [0x0bf080,0x001302, 0x0461a0,0x0462a0, 0x61f400,0x004b18, 0x62f400,DRY]
    do=len(p); p += [0x063a00,0]
    body=[0x56e100,0x44da00,0x200040,0x565900]*2
    p += body; p[do+1]=at()-1
    p += [0x00000c]
    return base,p,dict(fxsel=fxsel,swap=swap,add=add)
# stock call sites: (address, stock words, new words)
def sites(labels):
    return [(0x38a,[0x61f400,0x030430],[0x0bf080,labels['fxsel']]),
            (0x239,[0x0bf080,0x00166b],[0x0bf080,labels['swap']]),
            (0x27a,[0x0bf080,0x001302],[0x0bf080,labels['add']])]
