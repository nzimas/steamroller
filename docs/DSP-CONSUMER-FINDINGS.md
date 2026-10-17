# Rhythm DSP consumer and per-track output boundary

Date: 2026-10-16. All work in this pass was offline. No device messages, firmware uploads, or storage writes were performed.

## Correct reconstruction of the second-core image

The core-1 main download is not a flat P-memory image. The ARM loader first uploads a 64-word loader from application address `0x080662CC`, then sends the main stream selected from table entry `0x08031660`.

The small loader discards the outer length/address pair, then interprets records as `space, length, destination, data...`, where space 0/1/2 selects P/X/Y memory. Its end condition is a space value greater than 2, followed by a fixed jump to P:$0100. This was established by disassembling that loader, not by assuming all words in the main payload are executable.

The main stream has 20 non-overlapping records:

| Space | Populated words | Last populated address |
| --- | ---: | --- |
| P | 3,124 | `$0D31` |
| X | 10,512 | `$2ACE` |
| Y | 485 | `$1405` |

Its outer payload length is 14,187 words, including record headers, initialized data and end markers. The first end marker is `$0100`; five further `$0100` words follow it. Thus the previous 14,187-word figure must not be described as the core's executable program size. The sparse populated counts do not establish free memory: other memory can be reserved or initialized at runtime.

The actual consumer address is **P:$04CD**. A flat disassembly of the download incorrectly labels the same bytes around `$2D0A`. The probe reconstructs P/X/Y before executing anything and rejects unexpected image hashes, overlapping records and malformed lengths.

## Command consumer verified in the interpreter

The routine reads these core-1 Y-memory locations:

- `$307B5`: producer cursor.
- `$307B6`: consumer cursor.
- `$30410 + 2*i`: first command word for cursor `i`.
- `$30411 + 2*i`: second word.

When cursors differ, it advances the consumer modulo 16 and interprets the command. The isolated probe verifies:

| Command family | Observed effect |
| --- | --- |
| `$9xxxxx` trigger | Copies both words to per-track mailbox `Y:$307B7 + 2*t`; sets `X:$004A + 16*t` to 4 |
| `$8xxxxx` release | Sets `X:$004A + 16*t` to 2 |
| `$Bxxxxx` parameter | Writes the payload to a per-track parameter field; command `$B20102` writes `Y:$30809` |

Triggers were tested for all eight tracks using the unmodified words prepared by the ARM path, including the `$0C0000` payload for note 72. There is no byte-reversal assumption between the two probes. The DSP's short immediate-move encodings must be interpreted correctly: a displayed `move #$90,x0` loads the high-byte command mask used here; treating it as integer `$000090` gives an incorrect decoding.

The release-family consumer does not read the second word on that path. Therefore the ARM's release/stop payload values 2 and 3 must not, by themselves, be assigned distinct DSP meanings without tracing the later voice state machine.

The routine returns after one recognized event. With two trigger events queued, one call advances the reader to 1 and fills only the first mailbox; two calls advance it to 2 and fill both. The main loop visibly calls P:$04CD at P:$011C. It can skip unrecognized entries before returning, so "one event per call" specifically describes recognized events.

Thirteen isolated cases pass: empty queue, triggers on tracks 0–7, a release, a parameter write, and one-call/two-call processing of two queued triggers. These complement the earlier ARM-side probes, rather than emulating the complete ARM-to-DSP link or real-time execution.

The report includes interpreter instruction/cycle counts for these isolated calls plus the call stub. Those counts are not a measured audio-block CPU budget, and are not evidence of spare capacity on hardware.

## Voice and mixing boundaries from disassembly

The reconstructed core-1 main loop at P:$011C–$0126 calls the consumer, then several processing routines. Relevant boundaries include:

- **P:$0221**: clears per-track output buffers and traverses eight voice records. Active paths call P:$01E5, P:$0511, P:$025C, P:$0244 and P:$06AB. Exact meanings of all stages are still under investigation.
- **P:$02E4**: visits eight pending-command mailboxes and calls P:$02FD for nonzero commands. The trigger consumer and this later setup stage are distinct.
- **P:$06AB**: accumulates processed scratch samples from X:$2A02 into a per-track output buffer in Y memory, using a gain value from the current voice record. It advances the track counter and continues the voice traversal afterward.
- **Output-buffer address**: the accumulation routine computes a base equivalent to `Y:$304F0 + 64*track + 32*bank`, with bank read from peripheral Y:$FFFFD0. The active block length is taken from X:$1. This address calculation and its gain/multiply-accumulate path are static findings; no audio has yet been rendered through this stage.
- **Core-0 counterpart**: the flat core-0 main program accesses the same Y:$304F0 region and accumulates into X buffers at `$30430`, `$30470` and `$304B0`. This is evidence of the inter-core audio path, not yet a fully validated routing map for main audio and effects sends.

These boundaries suggest an investigation path for an oscillator: produce a block for a selected track before its output accumulation, retain a deliberate gain/mixing path, and bypass the sampler-specific voice processing for that track. Whether this should be a new branch inside the core-1 voice loop or a separate generator remains undecided.

Do not insert code only at the ARM trigger wrapper: that could select an engine without providing its audio generation, parameter mapping, queue lifecycle or output routing. Similarly, injecting a waveform after the master mix would not establish a per-track synth with Rhythm's track controls.

## Remaining experiments

1. Characterize P:$0221's traversal, voice state transitions and gain pointers with controlled synthetic state.
2. Controlled and Tonitor FM samples now pass through the isolated P:$06AB accumulation stage; see [FM-OUTPUT-BRIDGE.md](FM-OUTPUT-BRIDGE.md). Confirm the complete core-0 handoff, buffer-bank timing and effects sends next.
3. Establish P/X/Y memory usage beyond static segments and measure complete block costs under representative sampler/effects loads. Do not treat unpopulated addresses as free space.
4. Only then prototype a single-track oscillator in offline DSP execution, with note, release and engine-switch behavior. A hardware patch is not prepared by this pass.

## Tools and reproduction

`tools/rhythm_dsp_probe.py` extracts the segmented stream from the hash-checked official application, reconstructs memory, emits ignored local probe inputs under `research/analysis/dsp`, runs the DSP interpreter and checks the expected memory effects. It writes `docs/rhythm-dsp-consumer-probe.json`.

The local DSP tools were built from:

- `dsp56300/dsp56300` commit `44353be2f0156cac970c6ab4f467ea1998b40605`.
- Its `source/asmjit` submodule commit `3577608cab0bc509f856ebf6e41b2f9d9f71acc4`.
- The reference project's `tools/dsptool/dsp56kEmu-memtrace.patch` and `dsptool.cpp`.

The existing clone is `research/dsp56300`. On this Mac, configure and build there with:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDSP56K_FORCE_INTERPRETER=ON
cmake --build build --target dsp56kEmu -j 4
clang++ -O2 -std=c++17 -DDSP56K_FORCE_INTERPRETER=1 -Isource -Isource/asmjit/src ../CIRCUITS-HACKS-RC/tools/dsptool/dsptool.cpp build/source/dsp56kEmu/libdsp56kEmu.a build/source/dsp56kBase/libdsp56kBase.a build/source/asmjit/libasmjit.a -lpthread -ldl -o ../analysis/dsptool
```

From the project root:

```
.venv/bin/python tools/rhythm_dsp_probe.py
research/analysis/dsptool dis research/analysis/dsp/rhythm-core1-P.bin
```

The DSP interpreter executed successfully within the sandbox. The ARM Unicorn JIT restriction documented earlier does not apply to this interpreter-only probe. No operating-system settings were changed.
