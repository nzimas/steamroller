; Research-only DSP56300 copy kernel, not installed on Rhythm.
; Input: R1=source Y address, R5=destination Y address, linear M1/M5.
; Exactly 32 encoded 24-bit words; clobbers R1, R5 and Y0.
; Caller must validate ownership, metadata and destinations, preserve live
; registers, and release the staging slot only after all selected tracks copy.
; Explicit pairs avoid dependence on caller loop registers LA/LC.
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
move y:(r1)+,y0
move y0,y:(r5)+
rts
