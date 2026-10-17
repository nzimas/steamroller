# Tonitor FM reference through Rhythm's per-track accumulator

Date: 2026-10-16. This is an offline DSP experiment. It does not load firmware, send device commands, stream from the MCU, or reproduce the complete Rhythm audio pipeline.

## Result

The unmodified Rhythm core-1 accumulation instructions at **P:$06AB through P:$06C5** accept controlled sample blocks and Tonitor FM reference samples with the expected gain and destination addressing.

- 48 controlled cases: all eight tracks × both buffer banks × three gains.
- Each case seeds all 512 words of the eight-track/two-bank output region with sentinels, seeds the selected destination with a known bias, and checks that only its 32-word span changes.
- Gains tested are zero, `$400000` (0.5 in Q23), and `$7FFFFF` (maximum positive Q23, just below unity).
- Controlled output matches a scalar fractional-multiply/add reference to within one 24-bit DSP least-significant bit.
- All 32 Tonitor diagnostic FM algorithms, including their release halves, pass through the same stage: **16,384 blocks / 524,288 samples**.
- Those FM outputs match the expected converted/attenuated reference to within one DSP least-significant bit.

The source is the hash-recorded `all-algorithms-q24le.pcm` produced by the unmodified local Tonitor FM6 core. These are the synthetic diagnostic patches from the reference harness; they do not include Tonitor's generated sound families, macro layer, output filtering or loudness normalization.

## What was executed

The probe reconstructs the Rhythm core-1 P/X/Y segments from the official, hash-checked application. A test-only stub sets R0 to the fixture's gain pointer, then jumps into the existing accumulation routine. Execution stops at P:$06C6, before the code advances to another track and resumes the voice-processing loop.

Inputs are supplied in the ordinary scratch-buffer location **X:$2A02**. The routine accumulates into:

```
Y:$304F0 + 64*track + 32*bank
```

The test supplies a 32-sample length through X:$1 and explicitly sets the bank-read register Y:$FFFFD0 to 0 or 1 in the emulator. It does not establish that this register changes at the right time on hardware, or that 32 is Rhythm's actual block length under every operating mode.

The gain pointer and entry stub addresses are **synthetic test fixtures**, not identified free locations in the real instrument. Memory and program use outside this routine have not been validated by the test.

For the FM corpus, the Q24 core samples are shifted right by four before insertion into signed 24-bit DSP memory. Relative to a normal Q24→Q23 conversion, that adds a factor of 1/8 for headroom; the stage then applies a further factor of 1/2. The combined nominal attenuation is approximately 24 dB. This deliberate diagnostic gain must not be mistaken for the final engine's output calibration.

The one-LSB comparison tolerance accommodates fractional rounding at this stage. It is not a perceptual comparison against hardware or proof of bit-exactness for a future full engine implementation.

## Diagnostic audio

The probe writes a mono 24-bit WAV at:

`research/analysis/dsp/mix/tonitor-fm-through-rhythm-accumulator.wav`

It contains 32 consecutive algorithm excerpts, 16,384 samples each, using the original reference's 44,100 Hz rate: approximately 11.89 seconds total. Algorithm changes are abrupt; each excerpt includes the diagnostic patch's held and released portions. The file rate describes the offline reference only, not a verified Rhythm hardware rate.

This is **Tonitor's core reference rendered through one Rhythm DSP stage offline**. It is not a recording of either instrument, a complete Tonitor sound, or evidence that a modified Rhythm firmware is installed.

## Boundary established, remaining work

We now have independently checked pieces on either side of an engine bridge:

1. The Tonitor C core produces repeatable blocks and compiles for Cortex-M7.
2. Rhythm's native per-track accumulation stage processes converted blocks and preserves neighboring buffers in the isolated fixture.

The missing part is still substantial: scheduling the C renderer in Rhythm's MCU, transferring blocks to the DSP, and coordinating the sampler bypass, buffer ownership and full effects/mixing chain without missing deadlines.

The subsequent [ARM↔DSP transfer probe](ARM-DSP-TRANSFER.md) resolves the earlier candidate **`0x08031430` as a 16-bit readback routine**, not an FM upload API. It also verifies 24-bit word-write packing through `0x08031228` and verbatim byte transmission through `0x080315E6`, with hardware dependencies stubbed. Neither establishes a complete block-streaming protocol or safe DSP destination ownership.

For planning, the payload alone for one mono 24-bit stream is 132,300 bytes/s at 44.1 kHz, or 144,000 bytes/s at 48 kHz. A 32-sample block gives approximately 725.6 µs or 666.7 µs respectively. These are arithmetic lower bounds/deadlines for candidate rates, not measured bus throughput or confirmed Rhythm clock settings. Command overhead, locks, task scheduling and sampler/effects work must all be accounted for.

The [receiver bridge experiment](DSP-RECEIVER-BRIDGE.md) subsequently verifies packet delivery into these shared buffer addresses and the native active-bank clear. Next experiments should establish the actual bank/block schedule and MCU execution budget, and follow the per-track handoff into the full core-0 routing. Audio timing and effects routing remain unverified.

## Reproduction

```
.venv/bin/python tools/build_dsp_analysis.py
.venv/bin/python tools/rhythm_mix_probe.py
```

The builder creates `research/analysis/dsptool-io`, a local variant of the reference project's DSP wrapper. It exposes `IOX`/`IOY` register writes using the interpreter's supported 56362/56367 peripheral classes. These serve as controllable fixtures; they are not a complete model of Rhythm's DSP56724 board, clocks or inter-core bus. Neither source checkout is modified by the builder.

The probe saves its inputs under ignored `research/analysis/dsp/mix`, plus the concise report `docs/rhythm-mix-probe.json`. No firmware image is patched. The DSP core code is unchanged, and the Tonitor source checkout is not used for writes.
