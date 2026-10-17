# Target engine: Tonitor's DX7-style FM

Date: 2026-10-16. User direction: the eventual synth engine in the custom **Rhythm-based** firmware is the DX7-style FM engine from Tonitor for M-VAVE FM-1. Tracks remains a reference for investigation, not the target synth implementation or firmware base.

## Located source

- Source directory: `/Users/nzimas/development/mvave-fm1`.
- Inspected commit: `d3e4e36ec37e4cab151a4ee7fa6aacd419a08f00`.
- Every reference input used by the new probe has a SHA-256 recorded in `tonitor-fm-reference.json`. This matters if Tonitor development continues independently.

Important source files:

| File within Tonitor | Role |
| --- | --- |
| `firmware/src/fm6_core.c` | Integer C port of msfa/Dexed six-operator FM: rendering, envelopes, scaling, operator pitch, LFO and pitch envelope |
| `firmware/src/fm.c` | Tonitor voice layer, sound-object patch generator, macro mapping, patch measurement/gain, output treatment |
| `tools/gen_tables.py` | Constant sine/FM tables and sample-rate-derived constants |
| `firmware/src/layers.c` | Tonitor's shared voice allocation and application mixing; must be adapted to Rhythm rather than transplanted wholesale |
| `firmware/src/dsp.h` | Fixed-point helpers used by the surrounding Tonitor sound layer |

Tonitor declares the core Apache-2.0 and the surrounding firmware/wrapper GPL-3.0-only in `LICENSING.md`; keep those source distinctions and notices in any future implementation. This pass reads the local sources without copying them into the distributable project tree.

## Engine properties from source

- Six operators and all 32 algorithm tables.
- Four-rate/four-level operator envelopes, pitch envelope, feedback, keyboard scaling, velocity response, ratio/fixed operator frequencies and LFO.
- 155-byte unpacked single-voice patch layout, with operator 0 representing DX7 operator 6.
- Integer arithmetic: mainly 32-bit state, Q24 signal/control values, Q15 interpolated sine and 64-bit intermediate products.
- 32-sample render blocks at 44,100 Hz in the inspected Tonitor configuration.
- Two temporary 32-sample modulation buses on the render call's stack. The surrounding Tonitor renderer also has temporary output and macro state.

The existing FM-1 firmware's voice count is an application/resource decision, not an established Rhythm capability. Likewise, six FM operators should not be confused with six simultaneous voices or tracks.

## Independent reference built in this project

`tools/tonitor_fm_reference.c` includes the unmodified local core and provides a small adapter. `tools/investigate_tonitor.py` generates only the tables required by that core, builds a host executable, renders synthetic diagnostic patches for all 32 algorithms, and compiles the adapter/core for Cortex-M7 Thumb instructions.

This reference does **not** yet include Tonitor's patch generator, macros, loudness measurement, DC blocking/output gain treatment, or application voice allocator. Its synthetic patches are diagnostic material, not a reproduction of Tonitor's generated sound families.

Results:

| Measurement | Result |
| --- | ---: |
| Algorithms rendering nonzero output | 32 / 32 |
| Samples per algorithm | 16,384, with release at sample 8,192 |
| Host O0 versus O2 output | Bit-identical for these cases |
| `sizeof(fm6_note_t)` | 296 bytes |
| `sizeof(fm6_lfo_t)` | 24 bytes |
| Reference voice, including its own patch and pitch | 480 bytes |
| Cortex-M7 adapter/core `.text` | 5,546 bytes |
| Cortex-M7 constant sections combined | 8,114 bytes |
| Cortex-M7 unwind metadata | 48 bytes |
| Total allocated sections in this object | 13,708 bytes |
| Detected `__aeabi_*` symbols in generated assembly | None |

The note structure's old source comment says 244 bytes; the compiled structure is 296 bytes. Use measured layout rather than that comment. The patch can be shared by voices in a future design; the reference deliberately embeds one patch per voice for isolation.

Clang's Cortex-M7 stack reports are 368 bytes for `reference_render`'s own frame, with callees including `fm6_op_run` (112 bytes), `fm6_op_fb` (68 bytes) and `fm6_env_advance` (16 bytes). These are individual static frames, not a verified worst-case task/interrupt stack requirement. Existing Rhythm stacks and nesting must be measured before integration.

The raw Q24 reference output and generated files are under ignored `research/tonitor-reference/`. The committed report records input hashes, per-algorithm output hashes, sample counts and object-section sizes. The adapter uses `-fwrapv`; signed shifts and other integer-width semantics still need explicit attention in a different implementation. The host optimization comparison is useful regression evidence, not proof of equivalence on every target or patch.

## Two implementation paths to investigate

### A. Run the C engine on Rhythm's Cortex-M7

This is the first path to evaluate because the unmodified arithmetic and structure compile for that instruction set. It offers a direct reference comparison and avoids immediately rewriting the entire FM renderer in DSP assembly.

It requires a real-time render schedule on the MCU, adequate RAM/stack, and a reliable block transfer into Rhythm's existing per-track DSP output path. Existing ARM-to-DSP control writes do not establish an audio-streaming transport. Throughput, locks, buffer ownership and deadlines must be measured under ordinary Rhythm workloads.

The compile result alone does not establish real-time feasibility, voice count, ability to preserve every effect, or permission to use a particular flash/RAM address. Physical flash headroom is not the same as a bootloader-verified image-size allowance.

### B. Translate the engine to DSP56724 code

This could place synthesis beside the mapped sampler/output stages, but it is a substantive arithmetic port. The DSP uses 24-bit words and wider accumulators rather than the C core's 32/64-bit operations. Phase wrapping, Q24 gains, interpolation, feedback, exponent tables and envelope calculations require deliberate representation choices and reference comparisons.

Do not treat Tonitor's integer-only C as directly runnable DSP code, and do not choose this path merely because the hardware has a DSP. The reconstructed core-1 segment map is the basis for resource analysis; its unpopulated addresses are not assumed available.

## Sample-rate and block-boundary work

The isolated Tonitor core currently runs at its original 44.1 kHz. Rhythm's actual render clock/block timing must be established; 48 kHz sample-file requirements alone do not prove every DSP stage's rate.

If the engine must run at another rate, changing `FS` alone is insufficient. The generated frequency/LFO/pitch-envelope constants depend on rate, while the operator-envelope code also contains explicit 44.1 kHz static-duration values and block-step calculations. The wrapper has additional rate-dependent constants. Either adapt and validate these together or use a deliberate resampling bridge with a measured cost and latency.

The 32-sample Tonitor block is a useful starting point, but it must be reconciled with Rhythm's actual block length and double-buffer schedule rather than presumed identical.

## Revised milestones

1. **Reference core: complete for this pass.** Local unmodified FM6 rendering, hashes and Cortex-M7 compilation are available.
2. **Rhythm output bridge: isolated accumulation verified.** Controlled samples and the full 32-algorithm diagnostic corpus pass through the unchanged per-track accumulation instructions; see [FM-OUTPUT-BRIDGE.md](FM-OUTPUT-BRIDGE.md). Full handoff, pan/effects routing, buffer ownership and timing remain unverified. No new synth engine code on hardware yet.
3. **Execution-budget comparison:** measure the Cortex-M7 path and transport; compare with a scoped DSP prototype if necessary. Select the execution architecture using results.
4. **One-track FM integration in offline tests:** note-on, retrigger, release, stop and engine switching, initially one voice. This is a validation milestone, not a final polyphony limit.
5. **Tonitor sound layer:** adapt the desired patch generation, macros and output treatment from `fm.c`, then define Rhythm controls, automation and persistent engine/patch state. The target is Tonitor's FM behavior, not merely any generic six-operator synth.
6. **Hardware experiment:** only after Rhythm-specific build validation, resource mapping and recovery work are sufficient. Retain Rhythm as the base and the normal sample path on other tracks.

No modified Rhythm image has been produced or installed by this pass. No FM-1 or Rhythm device access was performed, and the Tonitor source directory was left unchanged.

## Reproduction

With the local Tonitor checkout still at the intended source revision:

```
.venv/bin/python tools/investigate_tonitor.py
```

The script builds and renders only under this project's ignored research folder and writes `docs/tonitor-fm-reference.json`. It does not invoke Tonitor's build/install scripts or host suite, which write into that other checkout.
