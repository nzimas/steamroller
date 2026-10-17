# Custom firmware for Circuit Rhythm

## Objective

Develop custom firmware based on the Novation Circuit Rhythm application, introducing selected capabilities inspired by Circuit Tracks and its community modifications. Develop and validate features iteratively on the Rhythm.

The user clarified this scope on 2026-10-15: the project is not a whole-application Tracks-to-Rhythm conversion. Rhythm is the firmware base, instrument model and development target. Tracks firmware and CIRCUITS-HACKS-RC are research references and potential sources of individual engine or feature implementations.

The target synth engine is **Tonitor's DX7-style six-operator FM engine**, from the local M-VAVE FM-1 firmware project at `/Users/nzimas/development/mvave-fm1` (user direction, 2026-10-16). An initial feature to investigate is choosing a track's sound engine: the Rhythm sample player or Tonitor FM. The number of simultaneous synth tracks, polyphony, engine-switch behavior and controls are open design questions constrained by CPU/DSP time, memory and the existing sequencer.

## Investigation priorities

1. Map Rhythm's track objects, sequencer-to-sound dispatch, note/parameter commands and DSP program loading.
2. Locate the Rhythm sampler's DSP entry points, voice state, inter-core communication and processing budget.
3. Investigate Tonitor's FM core and sound layer as the target implementation. Use Tracks/community work as supporting references for integration patterns, while adapting state, command interfaces, scheduling, assets and UI to Rhythm.
4. Design engine selection in Rhythm's existing track controls and project storage. Preserve ordinary Rhythm operation as the baseline; determine how new project data can coexist with current data.
5. Establish Rhythm-specific image rebuilding, offline validation and recovery before any modified firmware is installed.

Do not change model identifiers to impersonate Tracks, convert the unit's storage to Tracks format, or use a complete Tracks application as the first firmware experiment. A native Rhythm synth feature may need new patch assets, but its storage design must be defined within the Rhythm-based firmware.

## Evidence collected

- Connected unit: Rhythm application build 5706 (official 2.0.1).
- Bootloader: manually entered and queried; advertised build 4799.
- Official applications downloaded and checked offline.
- Shared static GPIO configuration and alternate DSP programs independently verified.
- No firmware or device-storage writes performed.

See [platform findings](PLATFORM-FINDINGS.md) and the [current design brief](DESIGN-BRIEF.md). Earlier investigation notes contain historical conversion assumptions that are superseded by this scope.

The [Rhythm engine integration investigation](RHYTHM-ENGINE-INTEGRATION.md) maps a candidate track/note/velocity interface and the sampler's ARM-side command ring, with reproducible offline probes.

The [DSP consumer investigation](DSP-CONSUMER-FINDINGS.md) reconstructs the second core's segmented program/data download, verifies its command consumer, and identifies candidate per-track output boundaries.

The [Tonitor FM port investigation](TONITOR-FM-PORT.md) identifies the target source, establishes an isolated 32-algorithm reference renderer, and records Cortex-M7 compilation and resource estimates.

The [FM output bridge experiment](FM-OUTPUT-BRIDGE.md) verifies controlled gain and track/bank isolation, plus Tonitor FM reference output through Rhythm's unchanged per-track DSP accumulation instructions. MCU streaming and real-time integration remain unverified.

The [ARM↔DSP transfer investigation](ARM-DSP-TRANSFER.md) resolves the earlier bulk-transfer candidate as 16-bit readback and verifies existing 24-bit word packing and raw transmit helpers offline. Destination decoding, scheduling and hardware throughput remain open.

The [DSP receiver bridge experiment](DSP-RECEIVER-BRIDGE.md) connects captured ARM writes to both DSP receive handlers and verifies every track/bank destination under shared-memory fixtures. It also confirms that the native processing pass clears the active output bank, making buffer scheduling the next integration requirement.

The [DSP buffer handoff investigation](DSP-BUFFER-HANDOFF.md) verifies the poll-driven bank pipeline, full/half block-length propagation and native three-bus stereo mixing of FM reference blocks. It identifies a candidate post-sampler insertion boundary; MCU staging, runtime timing and engine dispatch remain unimplemented.

The [FM staging prototype](FM-STAGING-PROTOTYPE.md) adds an executable two-slot ownership protocol and a DSP copy kernel. Adversarial publication checks and isolated copies pass; the protocol state machine still runs on the host and has not been inserted into firmware.
