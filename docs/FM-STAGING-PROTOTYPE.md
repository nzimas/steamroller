# FM staging protocol prototype

Date: 2026-10-16. Host-side reference protocol plus an isolated DSP payload-copy kernel. No installable firmware image or device operations.

## What exists

- `prototype/fm_staging.py`: executable reference for one MCU producer and a post-sampler DSP consumer. The protocol state machine currently runs in Python.
- `prototype/fm_copy_block.asm`: new DSP56300 code that copies exactly 32 words from a staged track to its native destination. This code executes in the interpreter, separately from the host protocol decisions.
- `tools/fm_staging_probe.py`: adversarial publication/ownership checks and comparisons between the reference output and the DSP copy kernel.

This is a specification made executable, with a tested payload-copy building block. The state machine has not been translated into DSP instructions, connected to real transport, or inserted into the official main loop.

## Proposed ownership

Use two fixed-size shared-memory slots, selected by sequence parity. The producer owns only an empty slot. It writes metadata and payload, waits for those writes to complete, then commits by writing READY=1 last. Once committed, the producer cannot modify the slot until the consumer releases it with READY=0.

The consumer runs once per eligible 32-sample post-sampler pass. It inspects at most two slots and copies at most eight track blocks. It never waits for a producer. The slot stays committed until all selected track copies finish; release is the final operation on an accepted slot.

The model assumes ordered, complete, atomic 24-bit word accesses, one producer, one consumer and configuration changes only at consumer block boundaries. These are protocol requirements, not demonstrated hardware memory-ordering properties. Real ARM transfers must be serialized with the existing DSP service and explicitly completed before commit. A register barrier alone would not demonstrate that a pending SPI/DMA transfer reached shared memory.

The current ARM write investigation establishes packet packing and receive semantics. It does not yet implement publication, acknowledgment readback or producer scheduling.

## Slot layout

Offsets count 24-bit DSP words:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | READY | 0 = producer may reserve; 1 = committed, immutable payload |
| 1 | EPOCH | Nonzero configuration generation |
| 2 | SEQUENCE | 24-bit rendering-pass identifier |
| 3 | TRACK_MASK | Snapshot of the eight FM track selections |
| 4 | FRAME_COUNT | Must equal 32 for this prototype |
| 5..260 | PAYLOAD | Eight track-major blocks, 32 encoded signed Q23 words each |

Each slot is 261 words, or 783 packed bytes. Two slots require 522 words / 1,566 packed bytes, excluding global control state, padding and implementation scratch. An ARM-side array of 32-bit storage would use more bytes; packed transfer size is not the MCU allocation size.

The consumer owns the current epoch, expected sequence, track mask, underrun/drop counters and status. These are Python fields in the reference, not yet an allocated DSP control structure. A real producer needs a coherent way to learn the current epoch, upcoming sequence and released slots.

The DSP-copy test uses Y:`0x34010` and Y:`0x34120` as slot fixtures, reserving a 272-word stride. **Neither address has been established as free in Rhythm.** The program locations P:`0x7000` and P:`0x7100` are also fixtures.

## Sequence and failure behavior

At each supported block boundary, the consumer checks committed slots:

1. Validate READY, epoch, mask snapshot, frame count and sequence/slot parity.
2. Compute `(slot_sequence - expected_sequence) & 0xFFFFFF`.
3. Distance zero is the current block. Distance one is the one permitted look-ahead block and stays committed. Other distances, including late and farther-future blocks, are dropped and released.
4. Replace the active-bank spans of FM-assigned tracks with the current payload, or with zeros if no current block exists. Leave sample-assigned tracks and the opposite bank unchanged.
5. Release the accepted slot after every selected track has copied. Advance the expected sequence even on underrun.

This policy prevents late audio from being replayed at the wrong step. Silence on underrun is an initial engineering policy; abrupt boundaries can click, and any smoothing policy remains to be designed and measured. The model does not claim a finished musical behavior.

A valid sequence of zero is distinguishable from an empty slot because READY is separate. Sequence wrap from `0xFFFFFF` to zero works with one-block look-ahead. Configuration epochs do not silently wrap: reaching `0xFFFFFF` requires a quiescent reset before reuse.

An engine-selection change increments the epoch and resets the expected sequence at a consumer boundary. It does not forcibly free a slot while a producer may still be filling it. If that old transfer later commits, the consumer discards it because its epoch differs. Real configuration requests and acknowledgments still need an implementation.

An unsupported native block length returns a status without copying, releasing a slot or advancing the reference sequence. The hook is bounded and returns; the integration must handle this status and disable the bridge or reconfigure coherently. The prototype does not support variable block lengths.

No automatic cancellation is implemented for a producer halted halfway through publication. Its local reservation prevents reuse; recovery requires quiescing that producer before resetting the reservation. Committed slots can be discarded by the consumer normally.

## Relationship to Rhythm's banks

The native [handoff investigation](DSP-BUFFER-HANDOFF.md) identifies a candidate boundary at core-1 P:`0x11E`, after the sampler pass and before its later processing/acknowledgment. The intended destination is:

```
Y:0x304F0 + 64*track + 32*core1_own_poll
```

Core 0 consumes that bank on the following request toggle, preserving the existing pipeline. The staging sequence identifies a rendering pass; it is not merely the alternating bank bit.

Replacement rather than addition prevents residual sampler audio from mixing into an FM-assigned track in the reference. It does not stop old sampler voices or recover their CPU cost. Real engine dispatch still needs note routing, voice cleanup and parameter-state handling. Previously prepared audio in the opposite bank may remain in the native pipeline during a mode switch; the epoch mechanism prevents stale staged blocks, not every possible transition tail.

The native core-0 mixer was independently verified to apply controlled left/right coefficients and three output buses to blocks from these destinations. The prototype does not yet compose every native clear, sampler operation, staging decision and downstream effect into one simultaneous dual-core run.

## DSP copy kernel

The kernel uses 32 explicit pairs of:

```
move y:(r1)+,y0
move y0,y:(r5)+
```

followed by RTS: 65 program words. It copies encoded sample bits without gain conversion or addition. R1 and R5 must point to validated source/destination spans with linear addressing. It clobbers R1, R5 and Y0; a real hook must preserve any live caller state and account for setup/return overhead. The unrolled kernel avoids using the caller's loop registers, but does not constitute a complete calling-convention audit.

The fixture stops immediately before RTS. It verifies memory copying, not a patched caller's stack, register preservation, interrupt interaction or hardware cycle budget. No timing result is inferred from the instruction count.

## Verification

Run:

```
.venv/bin/python tools/build_dsp_analysis.py
.venv/bin/python tools/fm_staging_probe.py
```

The probe passes:

- **262 publication boundaries**, covering every possible prefix of the four metadata writes, 256 payload writes and final commit. Partial publications never become audio; a publication committed after a missed deadline is discarded later.
- **16 recorded protocol checks**, including look-ahead retention, seven malformed/stale metadata cases, generation change during publication, 24-bit sequence wrap, epoch exhaustion, all-sampler passthrough, unsupported block length and producer reservation. At every one of 256 copied sample positions, attempted producer reuse remains blocked until release.
- **12 DSP copy cases**: both banks × six masks (first track, last track, alternating subsets, all FM and no FM). Source data comes from the hash-checked Tonitor diagnostic reference. Every selected destination matches the model, unselected tracks/opposite banks remain unchanged and staging source words remain unchanged.

The protocol checks execute in Python; the copy cases execute the new DSP kernel. This distinction is deliberate and is recorded in `docs/fm-staging-probe.json`. Tests write scripts and generated listings only under ignored `research/analysis/dsp/staging`.

## Remaining work

Translate the bounded consumer decisions into DSP instructions and validate them against this model, preserve caller state at the candidate boundary, and establish safe allocations by tracing all relevant memory users. Then implement the ARM producer/readback protocol and measure rendering, transfer and hook costs under realistic workload. Runtime block configuration, physical sample rate, full effects compatibility, project storage and on-device recovery remain open.
