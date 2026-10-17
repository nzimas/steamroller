# ARM write packets reaching Rhythm's DSP buffers

Date: 2026-10-16. Offline investigation; no device access or firmware installation.

## Result

The ARM write packing and DSP receive handlers now connect in a reproducible experiment. A command captured from the real ARM `0x08031228` path, followed by 32 captured data transactions, writes an FM reference block to the expected shared output-buffer address through either DSP core's unchanged receive handler. All eight tracks and both banks are covered, with both retained X/Y mode settings.

This establishes the **packet-to-buffer addressing path under explicit interface fixtures**. The output bank still needs a real-time ownership protocol: the native DSP processing pass clears its active bank before accumulating tracks. Sending into that bank before the clear would lose the FM block.

## Shared-memory basis

The [NXP DSP56724 reference manual, §3.1, printed page 3-1](https://www.nxp.com/docs/en/reference-manual/DSP56724RM.pdf#page=75) states that addresses `0x030000..0x037FFF` refer to shared RAM, accessible by both cores, and that P, X and Y accesses map to the same physical location. This is relevant because the native accumulator uses Y space, while the receive handler can write X or Y according to retained mode.

The candidate region `0x304F0..0x306EF` is within that shared range. The experiment models its P/X/Y aliasing explicitly. Previous isolated probes used independent emulator spaces; their individual routines did not require this alias, but that model was insufficient to connect an X-space receive write to a Y-space accumulator read.

## Receive handlers

The official programs install these receive-handler calls at vector P:`0x44`:

| Core | Handler | Saved state area (Y) | Pointer / mode words (X) |
| --- | --- | --- | --- |
| 0 | P:`0x150` | `0x2000..0x2005` | `0x10`, `0x11` |
| 1 | P:`0x6D0` | `0x1406..0x140B` | `0x6`, `0x7` |

Both read the received word from X:`0xFFFF94` and inspect GPIO input bit 8 at X:`0xFFFFBD`. In the fixture, high selects command decoding and low selects data storage. Data storage writes the full 24-bit word at the retained pointer, increments that pointer, and selects X or Y according to mode bit 15.

For the ARM-generated address command `0x0B04F0`, command bit 19 selects the extended-address branch. Masking with `0x03FFFF` yields `0x304F0`. This branch updates the pointer and retains the existing memory-space mode. The subsequent data words therefore reach X:`0x304F0...` or Y:`0x304F0...`; shared-memory aliasing makes those the same storage.

Separate local-memory controls verify that short-address commands `0x000800` and `0x008800` set X and Y modes respectively: the selected local word changes and the other space remains untouched. Unlike the shared region, these local addresses must not be treated as aliases.

Assembly bit numbers are printed in hexadecimal: for example `#$13` means bit 19, while `#$f` means bit 15. Interpreting these as decimal would misidentify the command families.

The protocol contains further command branches for readback, shifted addresses and deferred interface reconfiguration. This experiment exercises the write branches above; it does not establish a complete specification for every command family.

## Executed checks

`tools/rhythm_receiver_probe.py` checks the official application hash, extracts both programs from that image and captures real ARM instructions using the earlier transfer probe. Each stream is then delivered to the appropriate unchanged DSP handler using the local interpreter's SHI word-input fixture.

The result is **70 passing cases**:

- 64 block streams: two DSP handlers × two retained modes × eight tracks × two banks. Each transmits one captured address transaction and 32 captured data words. Together they cover the first diagnostic block of all 32 Tonitor FM algorithms, using the established `Q24 >> 4` headroom conversion.
- Four local X/Y controls: two handlers × two modes.
- Two bank-clear checks: both values of the bank fixture.

For every block stream, the complete 512-word output region is checked in P, X and Y. The selected 32 words equal the captured FM payload exactly, all neighboring words keep their sentinel, the address pointer advances by 32 and the retained mode is unchanged. There are 2,112 manually entered receive-handler invocations across those 64 streams.

These blocks are diagnostic Tonitor core output, not complete Tonitor sounds or real-time playback. No DSP gain stage is applied by the receiver itself.

The existing accumulator regression also passes with the expanded interpreter wrapper: 48 gain/isolation cases and all 16,384 FM blocks remain within one DSP least-significant bit of the scalar reference.

## Bank clearing and scheduling consequence

The core-1 main loop calls P:`0x221`. After an initial helper call, P:`0x223..0x233` starts from `0x304F0`, applies `32*bank`, and clears the active sample span for eight tracks before entering voice processing.

Executing just that clear region with the explicit 32-sample length fixture confirms that it clears **256 words**, the selected 32-word bank for each of eight tracks, while preserving the other bank. This is a buffer-lifecycle constraint, not a measured hardware cadence.

A direct MCU upload into final per-track output memory therefore requires coordination with the native clear, sampler accumulation and downstream consumption. Packet delivery alone does not make it safe to stream asynchronously. A future implementation may need a separate staging buffer and a DSP-side copy/accumulate hook at a defined point in the processing pass. No staging location has been identified as free, and no ready/consumed handshake has been implemented.

## Fixture boundaries

The analysis wrapper now supports `SHI`, `HOSTC` and `BRIDGE`, in addition to `IOX`/`IOY`. It uses the interpreter's supported 56362/56367 peripheral classes; these do not simulate the full DSP56724 board.

- SHI receive interrupts are disabled in the fixture. Each packet is injected as a complete word, then the handler is entered manually and stopped immediately before RTI. Interrupt entry, byte clocks, chip-select timing, word-length mode and FIFO service deadlines are not tested.
- GPIO command/data state is supplied explicitly. The ARM capture still stubs its GPIO dependencies; the physical wiring and edge timing are not exercised end to end.
- `BRIDGE 30000` redirects emulator accesses at and above that address to common storage. Tests use this alias only within the documented shared range; it is not a full chip memory map.
- Each core's handler is tested separately. This is not simultaneous dual-core execution, shared-bus arbitration, or an end-to-end rendering/mixing run.
- Ready state, locks and transfer completion remain immediate ARM fixtures. CPU usage, bus throughput, sample rate, bank ownership and installation/recovery are unverified.

## Reproduction

```
.venv/bin/python tools/build_dsp_analysis.py
.venv/bin/python tools/rhythm_receiver_probe.py
.venv/bin/python tools/rhythm_mix_probe.py
```

The ARM portion needs JIT execution permission on this Mac. The receiver probe writes its scripts under ignored `research/analysis/dsp/receiver` and its report to `docs/rhythm-receiver-probe.json`. Source checkouts and firmware images remain unchanged.

## Handoff follow-up

The [buffer handoff probe](DSP-BUFFER-HANDOFF.md) subsequently resolves opposite-bank rendering/consumption and the inter-core poll acknowledgment. It also validates FM reference blocks through the native core-0 three-bus stereo mixer with controlled coefficients. This narrows a candidate insertion point but does not implement staging or real-time firmware.
