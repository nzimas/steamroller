# Boot failure investigation: withdrawn 0.1.2

Stock Rhythm firmware was restored successfully. The user confirms normal
operation. No device access or firmware transfer was performed in this audit.

## Confirmed defect

The exact distributed image was read from
`dist/withdrawn/rhythm-fm-0.1.2.zip`, not rebuilt for the investigation. Its
SHA-256 is `9bba793bda28baa0194097448608f0b43f42ec00de64561b67d7f2367c6f90e9`.

The sampler DSP code was placed at **P:0x1000–0x117E** (383 words), outside
the sampler core's configured program RAM, **P:0x0000–0x0FFF**. Being absent
from the original segment map did not make this address range usable RAM.

The evidence is reproducible:

1. The stock ARM DSP download routine at `0x08031110` sends the bootstrap from
   `0x080662CC` to core 1 (`0x08031148` onward), then sends the mode-0 image
   selected from table slot `0x08031660` to that same core (`0x08031168` onward).
2. That 64-word bootstrap sets OMR bits **7, 21 and 22** at DSP P:2, P:3 and
   P:4. These are MS, MSW0 and MSW1. The reconstructed main sampler program
   contains no subsequent OMR writes.
3. [NXP's DSP56724 product brief](https://www.nxp.com/docs/en/product-brief/DSP56724PB.pdf),
   page 5, Table 2, specifies **4K program / 12K X / 8K Y words** for core 1
   with MS=MSW0=MSW1=1. Shared RAM is separately mapped at 0x30000–0x37FFF;
   it does not fill the low-address program hole beginning at 0x1000.
   [The reference manual](https://www.nxp.com/docs/en/reference-manual/DSP56724RM.pdf),
   Figure 5-1, identifies the OMR bit positions.
4. Every original sampler download segment fits the physical map. The custom
   segment fails it. The audio loop's patched call at P:0x011D unconditionally
   targets P:0x1000, even with no FM note held. This is therefore a startup/audio
   execution defect, not an FM patch or note-input problem.

The DSP cannot rely on code at that placement. Loss of DSP execution or its
startup/audio handshake is consistent with the observed reboot loop. **The
precise reset source (ARM watchdog, DSP watchdog, bus fault or other recovery
path) was not captured on hardware.** This audit establishes a fatal layout
defect, not an instruction trace proving it was the only failure.

## Why the earlier checks passed

The interpreter wrapper allocated **0x80000 words per memory space** and used
`DefaultMemoryValidator`, whose access check always returns true. Its backing
array therefore accepted P:0x1000. It did not model the DSP56724's selected
RAM partition. Synthetic test stubs also occupied addresses beyond physical
program RAM; those were useful component fixtures, not a boot model.

The ARM test replaced the entire `rhythm_boot_original` trampoline with a
successful return. DSP bus calls likewise returned from fixtures. The check
names overstated what these component tests established.

The new ARM audit executes reset, FM initialization and the **actual trampoline**
at `0x0806CCE8`, replacing only the board/peripheral setup call. It reaches the
native continuation with the same stack pointer and saved stack words as stock,
and with FM state initialized. This rules out a simple stack/prologue mistake
in that narrow path. It does **not** validate full peripheral startup, task
scheduling, locks, audio timing or worst-case stack use.

## Earlier transfer failures are separate

- 0.1 retained the stock installation-version block. The reference loader skips
  a matching block after successful receipt; this fits the unchanged sampler
  behavior. The Rhythm loader itself was not read back.
- 0.1.1 had a 432,110-byte, non-word-aligned image. Reference-loader execution
  reproduced its CRC mismatch. 0.1.2 corrected the length and then exposed the
  runtime boot failure. A valid transfer CRC does not validate DSP addresses.

## Changes made during the investigation

- Added `tools/rhythm_dsp_layout.py`, deriving the audited configuration from
  the stock bootstrap and checking physical RAM boundaries.
- The production builder now rejects P:0x1000 **before compilation or output
  replacement**. The old validation path also rejects the withdrawn image
  before entering the permissive DSP emulator.
- Added `tools/investigate_fm_boot.py`, which checks the immutable withdrawn
  image, runs boundary regressions, compares ARM boot continuation stacks and
  confirms the production build is blocked. Machine-readable evidence is in
  `docs/rhythm-fm-boot-investigation.json`.
- Stock program RAM has a static gap that could hold these 383 words at
  **P:0x0D40–0x0EBE**. Only its bounds and lack of static overlap were checked.
  No relocated firmware or transfer file was generated. This is not a tested
  replacement allocation or a claim that relocating code fixes every issue.

At the time of this investigation, custom transfers and release packaging were disabled. Before another hardware
candidate, startup/handshake behavior, DSP shared-memory ownership and the MCU
audio producer's locks, throughput and execution context still need validation.

Subsequent user instruction requested a corrected version. Version 0.1.3 uses
P:0x0D40–0x0EBE. Its offline tests now execute with program-fetch and data-access
bounds enforced and with test stubs inside physical program RAM. The six negative
boundary tests reject invalid loads, fetches and data reads. The actual ARM boot
trampoline also reaches the native continuation. This revision is packaged for
the requested hardware test; the remaining hardware limits above still apply.
