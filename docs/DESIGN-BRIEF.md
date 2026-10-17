# Rhythm-based engine selection: research brief

## User direction

The base is the Circuit Rhythm firmware. Introduce features taken from or inspired by Circuit Tracks at the level of a new custom Rhythm firmware. The concrete example is allowing a track to run either a synth engine or the sample player. Implement features iteratively.

The user subsequently specified the end target: port **Tonitor's DX7-style FM synthesizer** from the local `/Users/nzimas/development/mvave-fm1` project into this Rhythm-based firmware. Engine selection should therefore be investigated with Tonitor FM as the target synth, rather than treating the stock Tracks synth as the intended engine. See [TONITOR-FM-PORT.md](TONITOR-FM-PORT.md).

## First firmware version

User direction on 2026-10-16: implement the firmware, with tracks 1 and 2 reserved for FM and tracks 3–8 retaining the sample player. General per-track engine assignment is deferred. Rhythm FM 0.1 implements that fixed split; see [firmware notes](../firmware/rhythm_fm/README.md). The user subsequently directed that the work end with artifacts and validation, without installing anything on the machine.

## Architectural implications

- Keep the Rhythm application and its existing sampler as the starting point.
- Identify where Rhythm note events become sample-engine commands. An engine selector needs a dispatch point and per-track state; changing the visible track label alone cannot supply a synth.
- Investigate synth voice allocation, DSP command translation, audio mixing, parameter automation and engine-state reset. A shared DSP chip does not imply interchangeable DSP applications.
- Account for Rhythm's existing workload on both DSP cores. The previous comparison showed substantial Rhythm programs on both cores, whereas stock Tracks has a much smaller secondary-core program. Tracks-mod claims of spare secondary-core capacity must not be applied to Rhythm without measurement.
- Adapt the UI and parameter model to the chosen engine, including patch selection and automation semantics. Define what happens to notes and parameter state when switching engines.
- Extend Rhythm's project and patch storage deliberately. Existing sample slots and projects should remain the baseline; do not assume that Tracks packs or projects can be loaded directly.

## Initial research questions

1. Can Rhythm's track event representation express the desired synth behavior, including multiple notes and note lengths, or would that require sequencer changes?
2. Where are per-track sound parameters and automation stored and sent to the DSP?
3. What CPU, DSP, RAM and external-memory headroom is available with the existing sampler and effects active?
4. Which execution architecture can host Tonitor FM within Rhythm's measured budget: the Cortex-M7 C core with a DSP audio bridge, or a DSP56724 translation? A small diagnostic oscillator or controlled buffer can validate the output bridge before the complete engine.
5. How should engine choice and synth parameters persist in Rhythm projects, and what is the behavior when older firmware reads them?

No particular synth-track count, polyphony, live-switching behavior or preservation of every effect at maximum load has been established. These require design decisions and measurements.

## Reusable findings from the earlier investigation

The verified image container, Rhythm application and bootloader identities, common low-level hardware configuration, and alternate DSP block matches remain useful. Whole-firmware cross-flash acceptance and Tracks-format storage migration are no longer project goals.

The application update-parser experiment is now resolved: the firmware helper reads the version digits in decimal, while the upstream container helper interprets those bytes as hexadecimal nibbles. The isolated emulation checks pass with that distinction. No such probe was sent to hardware.

The first mapped Rhythm-native note interface, command-ring captures, storage observations and next steps are in [RHYTHM-ENGINE-INTEGRATION.md](RHYTHM-ENGINE-INTEGRATION.md).
