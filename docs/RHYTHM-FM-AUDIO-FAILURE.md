# 0.1.4 boots with FM state initialized but produces no FM audio

The user confirmed normal-mode identity 15712 and no audio on tracks 1–2 in
Note mode. This establishes successful application boot and initialized ITCM
state. It does not establish pad note delivery, DSP streaming or audible output.
The exact preserved input image is in `dist/rhythm-fm-0.1.4.zip`, image SHA-256
`ed2f2371a910f23ffbc21c2acfca22de849a18c3ee943035220fdcb68e60804b`.

## Reproduced scheduling defect

0.1.4 reads the DSP's expected sequence and publishes a block bearing that
same tag, after rendering and uploading 64 payload words. The DSP runs during
that upload. If it advances before commit, the block is already expired.
Its consumer also treats every tag mismatch as stale, so an early/future block
would be released rather than retained until due.

`tools/probe_fm_delayed_transfer.py` executes the actual old/new ARM producer
and replays its writes through the actual old/new DSP consumer. An explicit
clock advances every 40, 64 or 100 single-word writes. This is a latency
fixture, not a measurement of this unit's bus timing. It preserves partial
publication and uses the real consumer's sequence, payload and ownership
results to check the ARM readback model.

At 40 and 64 writes per DSP tick, 0.1.4 produces **zero nonzero output frames**.
Under the same fixtures, 0.1.5 produces respectively one and two. At 100 writes
per tick, both produce two nonzero frames. This proves a scheduling defect
and its correction in the model, not that the user's silence has this sole
cause or that throughput is adequate for continuous real-time audio.

0.1.5 schedules one block ahead, fills at most the next two frames, and
resynchronizes after falling behind. The consumer distinguishes future from
stale using a signed modular 24-bit difference. Future slots remain committed;
expired slots are released. Tests cover sequence wrap as well as ordinary
future slots. Missing frames remain silent and never block DSP execution.

## Runtime evidence for the next test

0.1.5 adds counters in ITCM; total state is 1,696 bytes, inside the same 4 KB
region and without native heap changes. Its identity is **15714**, or **15713**
if FM memory initialization is unavailable.

On standard identity inquiry, it first sends a diagnostic SysEx:
`F0 7D 52 46 4D 01`, twenty unsigned values encoded as five little-endian
7-bit groups each, then F7. The noncommercial ID avoids claiming a new official
Novation command. The twenty fields are:

notes1, notes2, services, ticks, blocks, nonzeroBlocks, peak1, peak2, dspMagic,
dspSequence, dspStatus, dspFrames, dspHeartbeats, dspUnderruns, dspDrops,
resyncs, pending1, pending2, nextSequence, busy.

The producer caches DSP status every 32 calls. The identity callback only
snapshots ARM memory; it performs no DSP bus operation. The report buffer has
permanent ITCM lifetime. Native 0x080298B8 copies its payload into the native
SRAM output buffer at 0x0802996E..0x08029982 before dispatching transport,
so USB/DMA does not need to access ITCM directly. Report length 107 is below
the native 360-byte path limit.

The DSP hook now publishes native frame count and a heartbeat even before the
bridge is enabled. Status 1 means the enabled consumer accepted the native
32-sample block size; status 2 means unsupported block size. A growing
heartbeat/sequence verifies execution; peaks and nonzeroBlocks establish
ARM rendering, not audible output. Underruns/drops and two snapshots distinguish
stalled execution from a progressing but underfed stream.

The new uploader logs `FM runtime status: {...}` before the subsequent identity
reply closes the read-only identity check. After playing both FM tracks,
check the normal-mode device and Save log. A second snapshot several seconds
later permits a rate comparison. If audio remains silent, use this evidence
to distinguish input gating, absent rendering, bus/memory visibility, consumer
progress and downstream mixing. None of those remaining paths is declared
working merely because identity succeeds.

All new tests are offline. No installation on the laptop and no automatic
firmware transfer occurs. The image still needs a hardware audio test.

## 0.1.5 hardware result and 0.1.6 correction

The user confirmed application build 15714. After playing track 1, the report
at 2026-10-16T09:27:39.646Z recorded 59 notes, 30,396 published blocks,
16,959 nonzero rendered blocks, track-1 peak 1,600,827, DSP status 1 and
32 samples/frame. DSP sequence was 91,262, underruns 90,460 and drops 29,616.
The user reports silence and occasional clicks. Track 2 received no notes in
this snapshot; this does not establish a track-2 input defect.

These counters establish successful input/render execution and severe stream
starvation. Roughly 97% of published blocks were dropped. Snapshot fields are
cached at slightly different instants; their differences are approximate.
Clicks are consistent with isolated accepted frames, but this does not prove
that any accepted frame contained nonzero audio or reached the output mixer.

Each previous payload required 64 separate native single-word SPI operations,
each with lock/wait/configuration/DMA overhead. 0.1.6 uses the existing native
SRAM TX buffer and one 192-byte raw transfer under the same SPI lock. Address
setup happens before packing because it overwrites the first three TX bytes.
The native transfer completes before release and READY publication; the cached
single-word address is invalidated. ITCM is CPU-read, never DMA-read.

The producer refreshes expected sequence after FM rendering and reschedules an
expired deadline to a future sequence of the same free-slot parity. The buffer
remains uncommitted while rendering/transferring. The consumer preserves future
slots and records copied block count, first copied samples and nonzero copied
block count. A failed SRAM staging-pointer check records bulkErrors and does
not commit a slot.

Nine new tests execute the compiled C burst and native address/raw transfer
helpers with only DMA/GPIO/mutex endpoints stubbed, then feed their captured
bytes through both real DSP receiver handlers in both memory modes. They check
ITCM source copying, byte order, lock arguments, completion-before-unlock,
address-cache invalidation, 0/65 rejection and 1/32/64-word bounds/guards.
Ten delayed-transfer cases preserve the immutable 0.1.4/0.1.5 comparison and
add the buffered producer plus a three-frame rendering delay. These compare
explicit scheduling/transaction fixtures; they do not measure hardware SPI
throughput or prove continuous audio.

The version-2 runtime message uses `F0 7D 52 46 4D 02`, the original twenty
fields plus dspCopies, dspSample1, dspSample2, bulkErrors, dspNonzeroCopies,
and F7. It is 132 bytes, within the native output-copy path. The browser parser
accepts both version 1 and 2. Identity **15715** identifies initialized 0.1.6;
**15713** retains the memory failure meaning. Hardware boot/audio and downstream
routing still require a user test. No hardware write is performed by these tools.

## 0.1.6 audible but degraded; 0.1.7 FIFO correction

The user reports a sharp/plucky initial attack followed by sustain, with strong
degradation and background crackling. This establishes audible output but not
clean FM. No 0.1.6 runtime status was supplied for this report, so the hardware
cause remains unconfirmed. The native patch itself has a fast attack and decay;
sustain means immediate note release alone does not explain the whole result.

The 0.1.6 queue has only two slots (1.33 ms at 48 kHz). Its consumer advances
the expected sequence even when empty, so audio rendered during a service gap
can expire. The producer can schedule future frames but cannot protect an
uninterrupted sample stream across an empty queue.

0.1.7 uses eight slots at 0x37800 + 128*i (i=0..7). Each holds READY, sequence,
32-frame length, then 32 samples per track. The FIFO read sequence advances
only after a complete matching block has been copied. An underrun clears the
active FM output bank and increments the miss count, but leaves the read
sequence unchanged so a pending block remains usable. READY is released after
copy, followed by read-sequence increment. The producer compares its next write
sequence to the read sequence to enforce eight-slot backpressure. It performs
at most four renders per service call and never overwrites a ready slot.

The sequence/length and 64 samples now travel together in one 198-byte transfer;
READY remains a separate final write. One read-sequence poll serves a whole
service invocation. Magic/status polling is every 128 calls, reducing bus load.
The sample stream remains continuous across stalls shorter than the current
queue depth. A longer stall still produces gaps; this is buffering, not an
unlimited-time correction or proof of sufficient average hardware throughput.

`probe_fm_fifo_jitter.py` executes the immutable 0.1.6 and linked 0.1.7 ARM
producers and replays all writes through each image's actual DSP consumer.
With service paused for 2, 4 or 6 audio frames, 0.1.6 has respectively 2, 4 or
6 output gaps after warmup. 0.1.7 has zero under those same fixtures. A 10-frame
pause gives 10 versus 3 gaps, exposing the new FIFO's finite limit. Additional
cases exercise 24-bit wrap and a DSP tick halfway through payload publication;
the consumer never copies the partial block. Accepted FIFO sequences are
contiguous and all copied payload samples match bit for bit. Twelve cases run.
These are explicit fixtures, not physical latency measurements.

Track 1 keeps its patch. Track 2 uses FM6 algorithm 32 with one audible carrier,
no feedback and equal attack/decay/sustain levels, giving a sustained sine
reference. A four-second host render checks middle-C pitch, stable half-second
RMS and no clipping. This isolates listening for interruptions from the complex
FM timbre; it does not bypass the FM engine on hardware.

Application identity is 15716, or 15713 on memory initialization failure. The
132-byte version-2 report format is unchanged. Its dspSequence now counts
accepted FIFO blocks rather than elapsed audio frames; dspHeartbeats remains
the elapsed-frame counter. Hardware output improvement and long-run workload
behavior still require testing. No software installation or automated device
transfer is part of these changes.
