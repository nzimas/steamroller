ENTRY(fm_boot)
SECTIONS {
  /* The poundhard drum code and its tables run from the reclaimed flash of the
     stock core-1 DSP download (0x080101F0..0x0801A837, 42567 bytes): the
     builder relocates a patched copy of that stream to the end of the image,
     and the core-1 pointer at 0x08031660 is the stream's only reference. */
  /* The fold curves (read only when a drum is struck) stay in the main area. */
  .lowflash 0x080101F0 : { *(.text.ph_*) *(.text.plock_*) *(.rodata.plock_*) *(.rodata.ph_carrier) *(.rodata.ph_modulator) *(.rodata.ph_transient) *(.rodata.ph_archetype) *(.rodata.ph_mild) *(.rodata.ph_harsh)
                           *(.rodata.ph_decay_*) *(.rodata.ph_hold_*) *(.rodata.ph_sweep_*) *(.rodata.ph_fold_at) }
  __lowflash_end = .;
  . = 0x0806A580;   /* right after the stock image (ends 0x0806A570) */
  .text : { KEEP(*(.text.fm_*)) *(.text*) *(.rodata*) }
  .ARM.exidx : { *(.ARM.exidx*) }
  /* Fast path: linked to run at ITCM 0x100, stored after .text in flash and
     copied by fm_boot. Below the FM voice pool at 0x1980. */
  .itcm 0x100 : AT(ALIGN(LOADADDR(.ARM.exidx) + SIZEOF(.ARM.exidx), 4)) {
    __itcm_start = .;
    *(.itcm_text*) *(.itcm_rodata*)
    . = ALIGN(4);
    __itcm_end = .;
  }
  __itcm_load = LOADADDR(.itcm);
  .data : { *(.data*) }
  .bss (NOLOAD) : { *(.bss*) *(COMMON) }
  __extension_end = __itcm_load + SIZEOF(.itcm);
  /DISCARD/ : { *(.comment) *(.note*) }
}
ASSERT(SIZEOF(.data) == 0, "Unexpected initialized RAM data")
ASSERT(SIZEOF(.bss) == 0, "Unexpected unallocated RAM state")
ASSERT(__extension_end < 0x08075000, "Extension leaves insufficient flash for DSP stream (the builder checks the exact fit)")
ASSERT(__itcm_end <= 0x1980, "ITCM fast path overlaps the voice pool")
ASSERT(__lowflash_end <= 0x0801A838, "Drum code exceeds the reclaimed core-1 stream flash")

stock_note_continue = 0x0804077c;
stock_release_continue = 0x08040842;
stock_stop_continue = 0x0804081a;
stock_boot_setup = 0x08025cc4;
stock_boot_continue = 0x0802568e;
stock_button_done = 0x0803dc94;
stock_shift_mask_test = 0x0803d95a;
stock_lanes_continue = 0x08055328;
stock_knob_continue = 0x08038e94;
stock_reset_continue = 0x080552fa;
stock_stop_clear_continue = 0x08055192;
stock_router_continue = 0x08028556;
stock_preview_continue = 0x0805ce88;
stock_track_continue = 0x0803d9a8;
stock_pad_continue = 0x0803d802;
