.syntax unified
.thumb
.section .text.trampolines,"ax",%progbits
.global rhythm_note_original
.thumb_func
rhythm_note_original:
 push.w {r4-r9,r11,lr}
 b.w stock_note_continue
.global rhythm_release_original
.thumb_func
rhythm_release_original:
 push {r2-r6,lr}
 and r0,r0,#7
 b.w stock_release_continue
.global rhythm_stop_original
.thumb_func
rhythm_stop_original:
 push {r2-r6,lr}
 and r2,r0,#7
 b.w stock_stop_continue
.global rhythm_boot_original
.thumb_func
rhythm_boot_original:
 push {r7,lr}
 bl stock_boot_setup
 b.w stock_boot_continue
/* Stock button handler 0x0803D8B8 (r4 = controller, r5 = button id), at its
   Shift-held branch 0x0803D956: the displaced `ldr.w r1,=0x07DFFF00` mask
   discards ids 0..7 (the track buttons), so Shift + track does nothing in
   stock. Ids 0..7 (tracks 1-8) request a new generated sound, then leave through the handler's
   own epilogue 0x0803DC94 (pop {r0,r4-r7,pc}); every other id continues at
   the stock mask test 0x0803D95A unchanged. */
.global fm_shift_button
.thumb_func
fm_shift_button:
 cmp r5,#8
 bhs 1f
 mov r0,r5
 mov r1,r4
 bl fm_randomize_request
 b.w stock_button_done
1:
 movw r1,#0xff00
 movt r1,#0x07df
 b.w stock_shift_mask_test
/* Stock controller pad press 0x0803D7FE (controller, pad, velocity):
   displaced push {r4-r6,lr}; mov r6,r0, then the rest of the stock handler. */
.global rhythm_pad_original
.thumb_func
rhythm_pad_original:
 push {r4-r6,lr}
 mov r6,r0
 b.w stock_pad_continue
/* Parameter locks (plock.c): the displaced stock entries. */
.global plock_lanes_original
.thumb_func
plock_lanes_original:
 push.w {r3-r11,lr}
 b.w stock_lanes_continue
.global plock_knob_original
.thumb_func
plock_knob_original:
 push {r3-r7,lr}
 mov r4,r0
 b.w stock_knob_continue
/* Player reset 0x080552F6: push {r4,lr}; mov r4,r0 displaced. Revert the
   player's locks first (r4 = player survives the call), then stock clears. */
.global plock_reset_hook
.thumb_func
plock_reset_hook:
 push {r4,lr}
 mov r4,r0
 bl plock_revert_all
 mov r0,r4
 b.w stock_reset_continue
/* Player transport 0x0805518C, inside a function that saved r4,r5,lr:
   displaced movs r0,#0; add.w r1,r4,#0x3c. r4 = player. */
.global plock_stop_hook
.thumb_func
plock_stop_hook:
 mov r0,r4
 bl plock_revert_all
 movs r0,#0
 add.w r1,r4,#0x3c
 b.w stock_stop_clear_continue
/* After the knob router's constructor returned (0x0802854E): displaced
   mov r8,r0; b 0x08028556. Save the router, keep every register. */
.global plock_router_capture
.thumb_func
plock_router_capture:
 push {r0-r3,r12,lr}
 bl plock_router_set
 pop {r0-r3,r12,lr}
 mov r8,r0
 b.w stock_router_continue
.global plock_preview_original
.thumb_func
plock_preview_original:
 push {r3-r7,lr}
 mov r4,r0
 b.w stock_preview_continue
/* Stock track-button case 0x0803D9A4 (r4 = router, r5 = id 0..7), displaced
   subs r6,r5,#1; cmp r6,#6 (the flags feed the next bhi). fm_track_press
   returns 1 to keep the Sample Rec view (a sound waits on that track): leave
   through the handler's epilogue 0x0803DC94; else stock continues. */
.global fm_track_hook
.thumb_func
fm_track_hook:
 push {r0-r3,r12,lr}
 mov r0,r4
 mov r1,r5
 bl fm_track_press
 cmp r0,#0
 pop {r0-r3,r12,lr}
 bne 1f
 subs r6,r5,#1
 cmp r6,#6
 b.w stock_track_continue
1:
 b.w stock_button_done
