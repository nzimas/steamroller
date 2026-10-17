# 0.1.3 late startup failure — investigation and D1 isolation test

The user reports that 0.1.3 reaches approximately pad 31 of the 32-pad startup
animation and then fails. This observation has not been mapped to an exact
instruction or initialization stage. Stock firmware was restored successfully;
a blank project is saved and the unit is currently in bootloader mode.

The exact failed image SHA-256 is
`58b80030461eaa2c3650ce117dcb70e728ba051948fa2eede5a8ce0c18c896d8`.
Its original ZIP is preserved in `dist/withdrawn/rhythm-fm-0.1.3.zip`.
The regular uploader is now recovery-only. A successful download/CRC check did
not establish successful startup, audio, or hardware integration.

## Offline findings and limits

- Stock and custom sampler DSP programs both reach P:0x111 from their actual
  P:0x100 startup code in the interpreter with the physical core-1 RAM bounds
  enforced. Both execute 4,176 instructions in that fixture. This does not
  model simultaneous DSP cores, real SHI interrupts or the ARM downloader.
- The extended ARM probe executes native reset, the boot trampoline and
  pre-scheduler constructors. Both images reach the call to the scheduler at
  0x08026134 after 117 allocation calls, with heap current 0x2000B1BC and
  minimum simulated SP 0x2004FF20. The custom heap limit is 0x2004E760 versus
  stock 0x2004F760. Peripheral initialization, clock setup and asynchronous
  hardware completion require explicit fixtures. This is not a complete boot.
- A separate execution of the application constructor at 0x08027980 reaches
  408 total allocation calls and heap current 0x200374A0 in both images. Both
  then encounter the same null interface at 0x08046008, because the real
  filesystem/worker initialization was not executed. The probe deliberately
  stops there. It does not bypass this boundary and claim success. This
  standalone constructor probe does not measure the real task's stack use.
- No allocation failure or FM-state overwrite was reproduced in these
  covered paths. Remaining allocations, scheduling, DSP shared-memory
  ownership, locks, bus access and hardware timing are still unverified.

## D1: separate the DSP change from the ARM FM extension

`tools/build_boot_diagnostic.py` creates
`dist/rhythm-boot-diagnostic-dsp-only/`, including a separate Mac launcher,
local uploader and official recovery firmware. It sends nothing automatically.

D1 starts from the pinned official image. It changes only the installation
marker, application identity and DSP download pointer within the original
application. It appends the exact modified DSP stream from the failed 0.1.3
image. All five original ARM hook sites are checked byte-for-byte against
stock. No ARM FM code or FM RAM reservation is included.

The expected behavior is **native sample playback on all eight tracks**, with
application identity **15709**. This is explicitly a diagnostic, not a corrected
FM release. Its only requested hardware result is whether startup reaches the
normal playable layout, restarts or freezes, plus the observed identity/log.

- If it boots, investigate the ARM extension/producer and its interaction with
  the DSP next. This alone does not identify which ARM hook or bus call fails.
- If it fails, investigate the DSP change/download path next. This alone does
  not identify the precise faulting instruction or reset source.

D1 passes image-difference assertions, physical DSP layout checks, SysEx
round-trip, the actual reference Tracks bootloader CRC loop and all 25 existing
sender tests adapted to D1's exact image and allowlist. These are offline checks;
hardware boot is unverified. Details and hashes are in its
`diagnostic-validation.json`. Reference bootloader CRC behavior remains an
inference for Rhythm; its bootloader was not dumped.

## Hardware follow-up: D1 completes startup

The user reports: “The startup completed without incident.” This follows the
D1 diagnostic transfer. Application identity 15709 has been requested but has
not yet been confirmed. Treat the reported startup result as evidence with
that identity limitation, rather than an independently captured hardware trace.

This narrows the failing combination to the ARM FM extension or its interaction
with the DSP. It does not establish that the DSP streaming protocol, enabled
consumer, output audio or timing are correct: D1 never enables FM staging.

### Next isolation image: D2

Build with `.venv/bin/python tools/build_boot_diagnostic.py --arm-init`.
The output is `dist/rhythm-boot-diagnostic-arm-init/`, with a separate uploader
and official recovery image. The image SHA-256 is
`73b7a4ee163111529a7a7b4ccb4c01637a1df5938d6e8faeadc9c4da34cc1b0f`.
Its installation marker is RFD002 and application identity is **15710**.

D2 uses the exact failed 0.1.3 ARM extension but activates only its boot hook.
The native note/release/stop hooks and service tail remain stock. The DSP image
is identical to D1 and 0.1.3. The boot hook initializes the FM state and lowers
the native heap limit by 4096 bytes. No producer or FM note handling runs.
All eight tracks intentionally remain samplers.

The native continuation, saved stack words and initialized FM magic are checked
by execution of the real boot trampoline with the board setup call stubbed.
Image differences, physical DSP layout, reference CRC and all 25 simulated
sender tests pass. D2 hardware startup has not yet been tested.

A D2 startup failure would focus investigation on ARM initialization and its
RAM reservation. A D2 boot success would leave the disabled runtime hooks and
producer interaction as the next target. Neither result alone proves an exact
crash instruction or reset mechanism. No firmware was sent automatically.

## Hardware follow-up: D2 fails startup

The user reports: “The boot fails now.” D2 has no active FM producer or note
hooks. Its additional behavior versus D1 is the FM boot initialization and
4096-byte native heap reservation. The exact hardware allocation/fault has not
been captured; application identity during the failing boot is unavailable.

The native bump allocator at 0x08025A98 compares its prospective end pointer
against the limit at 0x20002B54. An out-of-range request executes BKPT #1 at
0x08025AB4. Lowering this limit can therefore prevent native startup even if
the FM state itself fits its reserved range. The previous offline constructor
probe did not cover all native allocations and could not exclude this failure.

### D3: change only the heap-limit write

Build with `.venv/bin/python tools/build_boot_diagnostic.py --stock-heap`.
The output is `dist/rhythm-boot-diagnostic-stock-heap/`, identity **15711**,
installation marker RFD003. The image SHA-256 is
`c220c561c79770b997144f39c5780d0b4c898c7156c4b0de40202da0e7d305f6`.

Compared with D2, the sole changed executable instruction is
0x0806B024: `str r4,[r0]` (04 60) becomes NOP (00 BF). The installer marker and
application identity also change. An explicit binary assertion rejects any
other difference between D2 and D3. The boot trampoline, state initialization
and DSP program remain identical. Execution confirms the initialized FM magic,
unchanged native continuation stack, and original heap limit 0x2004F760.

The temporary state may subsequently be overwritten by native allocations.
This is intentional only in this diagnostic: no FM runtime hook ever uses it.
**It must not be used as a heap fix with the FM producer enabled.** All eight
tracks remain samplers. If D3 boots where D2 fails, the heap-limit write is the
causal executable difference in this test pair; heap exhaustion remains an
inference until the faulting allocation is observed.

Physical DSP geometry, SysEx round-trip, reference CRC and all 25 simulated
transfer tests pass. Hardware startup of D3 is pending. The D2 uploader has
been made recovery-only following its reported failure. No device access or
automatic transfer was performed.

## Hardware follow-up: D3 completes startup; 0.1.4 allocation change

The user reports: “It booted successfully.” D2 and D3 have identical executable
initialization except that D3 replaces the heap-limit write at 0x0806B024 with
NOP. The reported test pair isolates that write as the change preventing boot.
The precise failing allocation/reset mechanism was not captured. The diagnostic
application identities have been requested but not separately confirmed.

The fix in the full 0.1.4 candidate leaves the heap limit at 0x2004F760 and
moves the 1,532-byte FM state to ITCM 0x00003000..0x000035FB. ST AN4667 maps
16 KB of ITCM RAM at 0..0x3FFF for STM32F745/746; Arm DDI0489 explicitly permits
data-transfer instructions through enabled ITCM. The hardware matches the
upstream STM32F745 profile and the stock 320 KB SRAM map, but no independent
chip-ID readback was taken during this investigation.

FM initialization checks the implemented TCM size, keeps exception vectors in
the application flash table, enables ITCM with DSB/ISB barriers, and checks a
write/read/restore before initializing the state. The whole native SRAM heap
is byte-compared before/after initialization in the offline test, excluding
the ordinary call-stack area. A case with only 96 bytes of native heap space
remaining still initializes FM without changing the native heap pointers.
These are register/memory fixtures, not proof of physical ITCM operation.

The backup-SRAM alternative was rejected after auditing stock USB code:
0x08045B50 selects up to 0x1000-byte transfers through the buffer at 0x40024000
for READ(10); WRITE(10) likewise uses up to 0x1000 bytes at 0x08045C66 onward.
The two direct base-address literals are at 0x0802F17C and 0x08045DDC.
Its apparently empty tail is therefore not available for FM state.

0.1.4 restores all FM runtime hooks. Its normal identity send call now chooses
static payloads based on the initialized state: **15712** means FM state is
initialized, while **15713** means initialization is unavailable. Static payloads
avoid a temporary-data lifetime problem in the native MIDI transport. The
original device family/product and message shape are retained.

Offline validation covers 18 ARM integration cases, 16 DSP consumer cases,
native sampler composition, bit-identical ARM/host FM output, physical DSP
geometry, reference bootloader CRC and the complete simulated transfer tests.
The full firmware has not yet booted or produced FM audio on hardware. Full
ITCM ownership across workloads, DSP staging ownership and timing remain
hardware-validation limits. Nothing was installed on the laptop or sent to
the instrument automatically.

Primary documentation:
- [ST AN4667 memory map](https://www.st.com.cn/resource/en/application_note/an4667-stm32f7-series-system-architecture-and-performance-stmicroelectronics.pdf)
- [Arm Cortex-M7 technical reference manual](https://documentation-service.arm.com/static/5e906d72c8052b1608760d63)
