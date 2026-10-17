# Rhythm-native engine integration: first mapped note interface

Investigation date: 2026-10-16. Target: official Rhythm 5706, image SHA-256 `d5b9ff5dadf2e284f80d69ecb7087486cf0df315a697bed8272af9de0e90df15`. All new experiments in this pass were offline. The unit remains in normal operation; no device messages or writes were performed in this pass.

## What is established

The Rhythm application has a common ARM-side note interface taking a track ID, MIDI-style note value and velocity. Downstream functions turn those into sampler commands for eight slots. This is a candidate boundary for routing a track to either a sampler or synth. Its suitability for the entire sequencer and live MIDI paths still needs tracing; we have not proved that every sound-producing route passes through this boundary.

| Rhythm address | Observed role | Evidence |
| --- | --- | --- |
| `0x08031A18` | Object-method adapter: forwards arguments r1/r2/r3 as track/note/velocity | Disassembly; tail call to `0x08046780` |
| `0x08046780` | Acquires the DSP service lock, calls sampler note dispatch, releases lock | Disassembly; calls `0x08029C8C`, `0x08040874`, `0x08030EB8` |
| `0x080467AA` | Corresponding locked release wrapper | Disassembly; calls `0x0804083C` |
| `0x08040874` | Sampler note dispatch; special handling for track `0xFF`, otherwise per-slot parameter write then trigger | Disassembly and isolated emulation for tracks 0–7 |
| `0x08040778` | Prepares trigger; tracks previous note and slot state; stops previous event when retriggering | Disassembly and command capture |
| `0x0804083C` | Releases only the currently tracked note and only when a per-slot flag is set | Isolated emulation with flag 0/1 and matching/mismatching notes |
| `0x08040814` | Unconditional slot stop | Isolated emulation |
| `0x0804070E` | Updates cached per-slot parameters and writes DSP parameter memory | Disassembly only |
| `0x080406A8` | Writes a pair into a 16-entry DSP command ring | Executed in emulation with DSP-write calls intercepted |

The stock Tracks locking wrapper at `0x0803BD58` helped identify Rhythm's equivalent by instruction structure. Rhythm's actual downstream dispatch differs; Tracks' track classes and synth handling have not been substituted into Rhythm.

## Command preparation observed in emulation

For track `t` in 0–7, the sampler uses `t & 7` as its slot. The note-state arrays start at ARM RAM `0x20006D90`: the first eight bytes track notes and the next eight track slot ownership/active state. Per-slot cached configuration starts at `0x20006860`, with a 16-byte stride. Meanings for all cached fields are not yet mapped.

The outer note dispatch first calls the parameter-write API `0x08031200` with `(0, 0xFF00, 0x95 + t)`. The capture establishes these arguments, not the semantic meaning of that parameter.

The trigger routine calls `0x0803081C` before preparing its event. Disassembly shows that routine drains dirty parameter bits and forwards parameter values through `0x0804070E`. It is stubbed in the isolated note probe; a future engine router must preserve or deliberately replace that synchronization behavior.

The pair passed to the command-ring producer for a fresh note is:

```
word 0 = 0x900000 | (t << 16) | (velocity << 8) | t
word 1 = ((note - 60) << 16) modulo 2^32
```

For track 2, note 72, velocity 80, after a previous note on that slot, the captured pairs are:

```
0x820002, 0x000003  (stop previous event)
0x925002, 0x0C0000  (new event)
```

These names describe ARM behavior; the DSP interpretation of the opcodes is still unverified. Negative pitch payloads are passed through a left shift by eight before DSP transmission, so only the low 24 bits are retained in that write path.

The note-off routine checks both the stored note and byte `1` of the slot's cached configuration. With this flag zero, note-off produces no pair. With it set, a matching note produces `(0x800000 | t<<16 | t, 2)` and clears the tracked note. A forced stop uses the same first word with payload `3`. This is consistent with distinct gated/one-shot behavior, but the flag's user-facing meaning has not been verified against UI descriptors.

The fresh trigger path checks `note < 111` using a signed comparison. Inputs above that boundary were not included in the probe; do not assume the routine accepts the entire MIDI note range.

## DSP command ring

The producer at `0x080406A8` uses ARM cursor `0x20006F38`. For cursor `i`, it calls the DSP memory-write API `0x08031228` with:

1. `(0, word0 << 8, 0x30410 + 2*i)`
2. `(0, word1 << 8, 0x30411 + 2*i)`
3. `(0, ((i+1)&15) << 8, 0x307B5)`

The producer then retains `(i+1)&15` in ARM RAM. The probe confirms wrap after 16 pairs. These are API-level DSP addresses; identifying the DSP memory-space selection, consumer routine, synchronization and overflow handling is the next step. No DSP processing or audio was emulated.

## Implications for a first native synth experiment

A useful first milestone would be one monophonic diagnostic oscillator assigned to one Rhythm track, with the other tracks retaining their sampler path. This is a proposed integration experiment, not a selected final synth engine or a verified resource allocation.

The candidate design would add per-track engine state around the common note dispatch, preserve its lock discipline, route release and forced-stop events consistently, and keep sampler parameter synchronization off the synth path unless it is needed. Engine changes must stop the previous engine's voices and handle queued events. Merely appending oscillator code or redirecting note-on is insufficient.

The existing sampler bookkeeping is monophonic per slot at this boundary. It does not prove that the sequencer can store chords, or that a synth must be monophonic. Polyphonic sequencing would require a separate investigation of stored events, held notes, voice allocation and note-off bookkeeping. Start by understanding one voice before extending those structures.

Do not reuse the Tracks mod's claims of spare DSP core 1 capacity: Rhythm has substantial code on both cores. Neither DSP cycle headroom nor free program/data memory has been established.

## Project and asset storage

Official example archives were downloaded from Novation's manual pack-setup articles and inspected without extracting them to any device:

- Tracks: `Tracks.zip`, attachment `7043146414738`.
- Rhythm: `Rhythm.zip`, attachment `4402780979986`.

In these example archives, Tracks projects have magic `USER` and length 160,780 bytes; Rhythm projects have magic `RUSE` and length 169,092 bytes. Tracks has `.cpb` synth banks; the Rhythm archive has `.ncg` Grid FX data and no `.cpb` banks. These observations apply to the downloaded examples, not every firmware revision or saved project.

For this project, engine selection and synth patch state must be designed within Rhythm's storage model. Do not replace Rhythm folders with Tracks folders or treat the common `.ncs` extension as evidence of project compatibility. No extension field, unused padding area or backward-compatible save method has yet been validated.

References:

- https://support.novationmusic.com/hc/en-gb/articles/360019663719-Manually-setting-up-Circuit-Tracks-SD-card-packs
- https://support.novationmusic.com/hc/en-gb/articles/4402771846930-Manually-setting-up-Circuit-Rhythm-SD-card-packs

## Update-container follow-up

The previously interrupted update probe is now resolved. The application's header helper accumulates version digits using powers of ten: Tracks `000572` becomes 572; Rhythm `000731` becomes 731. The upstream container helper reads those same bytes as hexadecimal nibbles. Both representations preserve the original official files when encoded as upstream expects, but they should not be conflated when comparing parsed fields. Size and CRC fields agree directly. The application-only product-gate and header-parser checks now pass for both images; this does not validate the Rhythm bootloader.

## Reproduction and limitations

`tools/rhythm_note_probe.py` verifies 13 note scenarios plus ring writes and cursor wrap. It hash-checks the official Rhythm image, uses synthetic RAM, and replaces selected firmware dependencies with logging stubs. It verifies ARM command preparation, not full initialization, queued timing, audio output, DSP execution, sequencer behavior or hardware safety.

`tools/investigate_update.py` verifies isolated application gates and official header parsing. `tools/inspect_packs.py` inventories the local official ZIP downloads. Reports are `rhythm-note-probe.json`, `update-receiver-emulation.json` and `example-pack-inspection.json`.

The DSP ring consumer has now been reconstructed and tested separately; candidate per-track output boundaries are mapped in [DSP-CONSUMER-FINDINGS.md](DSP-CONSUMER-FINDINGS.md). Full block processing, routing and resource usage remain to be measured before writing an experimental engine patch.
