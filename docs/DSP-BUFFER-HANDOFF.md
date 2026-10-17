# Rhythm buffer handoff and native stereo routing

Date: 2026-10-16. Offline execution of official DSP instructions with explicit register and memory fixtures.

## Result

The native code uses opposite banks for core-0 track mixing and core-1 track rendering during a processing pass. Core 0 toggles its poll value to request work, core 1 detects the mismatch, renders using its previous poll value, and copies the request into its own poll value after processing. Core 0 waits for equality later in its pass.

A second experiment feeds Tonitor diagnostic blocks through the unchanged core-0 track mixer. The selected bank reaches three stereo output buses with controlled channel gains. Both banks and all eight tracks pass, including zero-gain controls and separate eight-track summation tests.

This makes a DSP-side insertion point after the sampler pass a concrete candidate. It does not establish a real-time MCU producer, a free staging buffer, a working engine selector, or complete audio output.

## Inter-core register meaning

[NXP's DSP56724 reference manual, §13.2.2.11–12 and §13.3](https://www.nxp.com/docs/en/reference-manual/DSP56724RM.pdf#page=258) defines Y:`0xFFFFD0` as each core's own writable poll register and Y:`0xFFFFD1` as a read-only view of the other core's poll value. These are inter-core communication registers, not an independent audio bank clock.

The earlier probes deliberately supplied these values as fixtures. Their bank-selection results remain valid, but this investigation identifies what the firmware is actually reading.

## Native sequence

| Code location | Observed operation |
| --- | --- |
| Core 0 P:`0x202..0x204` | Publishes X:`0x3A` into shared `0x307B4` |
| Core 0 P:`0x20D` | Toggles bit 0 of its own poll register |
| Core 0 P:`0x224` → P:`0x367` | Mixes tracks from the bank indexed by its new poll value |
| Core 1 P:`0x112..0x116` | Compares peer poll with own poll; equality returns to its idle helper |
| Core 1 P:`0x117..0x118` | Copies the published full length into X:`0x1`, then half the length into X:`0x2` |
| Core 1 P:`0x11D` → P:`0x221` | Clears and processes tracks using its old own-poll bank |
| Core 1 P:`0x127..0x128` | Copies peer poll into own poll after the remaining processing calls |
| Core 0 P:`0x228` → P:`0x1A11` | Waits until own and peer poll values are equal before continuing that routine |

The two cores are concurrent on hardware. The table describes each core's code order; it is not a single interleaved execution trace.

For an example pass starting with both poll values zero:

1. Core 0 changes its value to one and consumes track bank one, prepared in an earlier pass.
2. Core 1 sees peer=one and own=zero, then clears and fills track bank zero.
3. Core 1 sets its value to one after completing its pass.
4. Core 0's equality wait can pass. On the next toggle back to zero, its track mixer selects the newly prepared bank zero.

This is a pipeline: a block rendered into core 1's bank is consumed by the core-0 track mixer on the following request toggle. A future FM ready flag needs a sequence number or similarly explicit block identity; the current poll bit alone is not an MCU producer/consumer protocol.

## Block length

The length propagation was executed with diagnostic core-0 X:`0x3A` values 16 and 32. In each case the shared word and core-1 X:`0x1` equal that full value, while core-1 X:`0x2` holds half. The parallel move at P:`0x117` copies the original B1 to X:`0x1` while the shift changes B for the following instruction.

These tests resolve the full/half distinction. They do **not** identify the actual initialization of core-0 X:`0x3A`, establish that 16 is a supported operating block length, or measure the hardware block cadence. Audio routing tests use the existing 32-sample fixture, consistent with the 32-word banks and fixed 64-word per-track stride identified in the accumulator. The source of the runtime length and physical sample rate remain open.

## Core-0 native mixer

The tested region is P:`0x367..0x3AE`, stopping before its RTS at P:`0x3AF`. It clears three 64-word stereo buses and mixes eight mono track blocks. For the 32-sample fixture, source selection is:

```
shared[0x304F0 + 64*track + 32*core0_own_poll]
```

The output coefficient layout established by the instructions and tests is:

| Stereo output bus | Left/right coefficient pair for sample i of track t |
| --- | --- |
| X:`0x30430` | X:`0xDE + 64*t + 2*i` and the following word |
| X:`0x304B0` | X:`0x2DE + 64*t + 2*i` and the following word |
| X:`0x30470` | X:`0x4DE + 64*t + 2*i` and the following word |

These are per-sample gain buffers, not yet a mapping of user-facing pan/send controls. Their downstream effect roles have not been verified here. The mixer uses fractional multiply-accumulate operations and stores interleaved left/right results.

The three output buses occupy `0x30430..0x304EF`, immediately followed by the track-input region at `0x304F0`. With shared-memory aliasing, an X-space write just beyond those output buses would overwrite the first Y-space track sample. The probe checks the preceding output guard and the entire source region rather than placing an overlapping guard there.

### Passed checks

- Four publish/accept pairs: two request directions × two diagnostic lengths.
- Two idle checks: equal poll values branch back without changing block-length fields.
- Two completion acknowledgments: core 1 copies the request into its own poll register.
- Four core-0 wait checks: equality passes; mismatch remains in the bounded polling loop.
- 48 isolated mixer cases: eight tracks × two banks × three gain profiles. All 32 Tonitor algorithms contribute a diagnostic block across these cases. The opposite bank contains deliberately different data to expose incorrect bank selection.
- Two eight-track summation cases, one per bank, with exact scalar-reference agreement for the chosen controlled values.

All isolated mixer outputs agree with a Q23 scalar gain reference within one DSP least-significant bit. Source buffers and the preceding output guard remain unchanged. Native accumulation regression remains at 48 control cases and 16,384 FM blocks, within one LSB.

The Tonitor source conversion here is the established Q24 right shift by four. This mixer experiment does not run the earlier core-1 accumulator's additional half-gain stage, Tonitor's full sound wrapper, or the complete Rhythm effects/output chain.

## Candidate integration point

A candidate hook is the core-1 main-loop boundary **P:`0x11E`**, immediately after the sampler pass at P:`0x221` returns and before the next existing processing call. At that point, the current bank has been cleared and native sample voices have been accumulated; the completion acknowledgment has not yet been published.

A possible implementation would consume a fully published FM staging block there and insert it only for tracks assigned to FM. The next core-0 pass could then apply the ordinary track mixer. This requires:

- An owned staging allocation and a ready/consumed sequence protocol between MCU and DSP.
- A defined underrun policy that does not stall existing audio indefinitely.
- Suppression and cleanup of sampler voices for an FM-assigned track.
- Verification of subsequent core-1 processing, gain calibration, and the core-0 control-generation and downstream effect paths.
- Measured MCU render and transfer time under realistic sampler/effects load.

This is an architectural candidate, not an implemented patch. No free memory or safe instruction replacement has been established. Direct asynchronous writes into final track banks remain unsuitable without an ownership protocol.

## Reproduction and limits

```
.venv/bin/python tools/build_dsp_analysis.py
.venv/bin/python tools/rhythm_handoff_probe.py
.venv/bin/python tools/rhythm_mix_probe.py
```

The probe hashes the official image, extracts both programs, and writes scripts under ignored `research/analysis/dsp/handoff`. Its report is `docs/rhythm-handoff-probe.json`.

The wrapper's `READIOY` command reads the interpreter peripheral fixture; it is not device readback. Poll values are manually supplied to each isolated core invocation. No actual ICC link, simultaneous cores, arbitration, interrupt timing, physical sample rate, missed-deadline behavior or device storage is exercised. The tested firmware routines are unchanged, and no image is prepared for installation.

## Staging prototype follow-up

The [FM staging prototype](FM-STAGING-PROTOTYPE.md) now defines an executable host reference for two-slot ownership, epochs, sequence numbers and late-block handling, with an independently executed DSP copy kernel. Native consumer decisions, safe allocations and real-time integration remain pending.
