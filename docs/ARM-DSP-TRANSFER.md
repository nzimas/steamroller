# Rhythm ARM↔DSP transfer investigation

## Result

The previously identified bulk routine **`0x08031430` is a 16-bit readback path**. It receives data from the interface, swaps each received byte pair, and writes the resulting halfwords into the ARM destination buffer. It is not an upload routine for rendered FM samples.

Two existing transmit paths were also captured offline:

- `0x08031228` sends a left-aligned 24-bit word through the application's memory-write API. Consecutive writes reuse a cached next-address value.
- `0x080315E6` transmits a caller-supplied byte buffer unchanged, under one chip-select interval. It does not establish a DSP destination or acquire the service lock itself.

These are transport building blocks, not yet a complete FM audio bridge. The probes execute official ARM instructions with simulated hardware dependencies. They do not exercise the DSP endpoint, prove which DSP core/memory space is selected by a given command, measure throughput, or establish safe ownership of an output bank.

## Evidence and reproduction

`tools/rhythm_transfer_probe.py` checks the official Rhythm 5706 image hash before executing the relevant functions. It uses synthetic RAM objects and stubs SPI/DMA operations, ready-pin reads, locks and completion waits. Ready reads always return zero and waits complete immediately. No MIDI or USB API is imported.

Run:

```
.venv/bin/python tools/rhythm_transfer_probe.py
```

On this Mac, Unicorn requires permission for executable JIT memory. The probe writes `docs/rhythm-transfer-probe.json` and has passed:

- 20 readback cases across selectors 0 and 1: counts 0, 1, 32, 511, 512, 513, 1025, nonzero address offsets, and a diagnostic count of 65536.
- Eight 24-bit single-word writes across both selectors, including zero and signed boundary bit patterns.
- A contiguous 32-word write sequence, verifying address-cache reuse.
- Eight raw byte transfers across both selectors, with lengths 0, 3, 96 and 510 bytes.

The selectors are exercised as ARM arguments; the fixture does not establish physical bus routing.

## Readback path

Observed signature:

```
0x08031430(selector, encoded_address, ARM_destination, count)
```

The routine locks the service object, waits, selects an interface and emits three-byte transactions. For diagnostic address `0x123400 + offset`, captures are:

```
10 12 34
20 01 00
40 00 40 00
```

The last four bytes are the preamble issued by helper `0x0804644C`. If `offset` is nonzero, a receive operation discards `2*offset` bytes before that helper runs. The helper receives the requested data in chunks of at most 512 halfwords, while transmitting repeated byte `0x40`. Its ARM loop uses `LDRSH`, `REVSH`, then `STRH` to copy incoming big-endian byte pairs into little-endian destination halfwords. Both positive and negative bit patterns are included in the fixture.

The finish helper `0x0803157E` transmits `20 00`, changes selection state, and polls the selected ready input. The outer function invalidates the cached address with `0xFFFF`, unlocks and returns the original count argument.

Only the low 16 bits of `count` reach the receive helper. Passing 65536 copies zero samples but returns 65536 in this fixture. This is an argument-width observation, not an API usage recommendation. The function does not provide the bounds validation a new caller would need.

Guards around the ARM destination remain unchanged in all cases. This proves the tested copy behavior with supplied RX bytes; no actual DSP read command was executed.

## 24-bit word writes

The existing queue producer already calls:

```
0x08031228(selector, word24 << 8, encoded_address)
```

Executing this API through its real command-packing helpers confirms the transaction bytes. For example, address `0x304F0` and word `0x123456` produce:

```
0B 04 F0    address setup
12 34 56    data
```

The low eight bits of the ARM data argument are not transmitted. The API stores `encoded_address + 1` in its address cache at ARM `0x20003684`. With a fresh fixture, 32 sequential writes from `0x304F0` produce one address command plus 32 data transactions: **33 transactions, 99 transmitted bytes**. Every word call still enters the lock and wait machinery; fewer address commands do not eliminate its CPU or synchronization overhead.

The cache is shared state. This test has one caller and does not exercise competing operations or switching selectors within a cached sequence. New integration code must respect the application's service ownership and cache invalidation rules.

The chosen numerical address resembles the candidate DSP accumulator address from the DSP investigation. That numerical match is not proof that this ARM command reaches that memory space on the intended core. The endpoint decoder and addressing mode remain to be traced.

## Raw transmit helper

Observed signature:

```
0x080315E6(selector, ARM_source, byte_count)
```

This helper waits for the prior operation, selects the interface, calls `0x080464E6` to transmit the source bytes and wait for completion, then deselects. Captures preserve all supplied bytes, including a 96-byte buffer large enough for 32 packed 24-bit samples.

It performs no sample conversion, destination setup, block framing, address-cache maintenance, or service locking. A correct DSP transaction would require an independently established protocol and caller context. The test does not establish that arbitrary 96-byte writes are accepted by the DSP, or that the command/data stream may be batched this way.

## Implication for the FM port

The previous bulk-transfer candidate cannot carry rendered FM blocks in the required direction. The validated word-packing path gives a conservative starting point for further endpoint research. The raw byte helper could support a more efficient implementation if the DSP protocol permits it.

For one mono stream, 32 samples require 96 payload bytes. At hypothetical rates of 44.1 or 48 kHz, a 32-sample block has approximately 725.6 or 666.7 microseconds available. The cached word-write capture would add three address bytes per uninterrupted block if address setup is required each block. These are arithmetic quantities, not measured transfer time or verified Rhythm rates. More FM tracks multiply the payload, and CPU rendering, waiting and existing sampler/effects work share the deadline.

The next integration questions are the DSP-side command decoder, actual audio sample rate and block cadence, bank ownership, and MCU execution budget. No instrument mode change is needed to continue these offline checks. No modified firmware has been built for installation or sent to the device in this experiment.

## Follow-up receiver experiment

The [DSP receiver bridge experiment](DSP-RECEIVER-BRIDGE.md) now connects captured ARM packets to both unchanged receive handlers with explicit GPIO, SHI and shared-memory fixtures. All track/bank destinations pass. The remaining integration constraint includes the verified native bank clear; packet delivery does not establish safe asynchronous streaming.
