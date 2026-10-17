# Circuit Tracks firmware on Circuit Rhythm: initial investigation

**Historical scope:** The user subsequently clarified that the target is custom firmware based on Rhythm, with individual Tracks-inspired features such as per-track synth/sample engine selection. The conversion plan below is superseded by [DESIGN-BRIEF.md](DESIGN-BRIEF.md). Hardware and image-format observations remain relevant.

Date: 2026-10-15. No firmware, project, pack, bootloader, option-byte, or device storage writes were performed. No update or diagnostic messages were sent to the device.

This document records the first pass. Subsequent live identity and offline startup results are in [PLATFORM-FINDINGS.md](PLATFORM-FINDINGS.md); the connected unit's build is now confirmed as 5706.

## Findings

The target is plausible, but cross-model compatibility remains unproven. The cited project modifies the Tracks application through its existing SysEx updater; it does not replace or unlock the bootloader. Its bootloader recovery analysis applies to the Tracks dump it inspected, not automatically to Rhythm.

The repository contains binary patch builders, a small Thumb assembler, DSP tools, ARM/DSP emulation harnesses, an official-image patcher, uploaders, manifests and test plans. It is not a source release of Novation's application. New work must map existing binary interfaces and preserve assumptions about RAM, DSP memory, storage and execution timing. Many recent builds are explicitly marked pending hardware testing. The latest RC is not a validated Rhythm starting point.

The upstream technical notes already inspect Rhythm as a DSP-code donor. They do not demonstrate Tracks firmware running on Rhythm. Their earlier user's decision to exclude Rhythm development is historical context for that project, not a constraint on this project.

## Direct offline verification

Downloaded Novation's catalog and official firmware IDs 168 and 169. Both decode with upstream `ct_fw.py`, pass its CRC check and re-encode byte for byte. Tracks matches the upstream stock SHA-256. Full hashes and inspection results are in `firmware-inspection.json`.

| Property | Tracks | Rhythm |
| --- | --- | --- |
| Official catalog version/build | 1.2.1 / 4486 | 2.0.1 / 5706 |
| SysEx product byte | `0x64` | `0x63` |
| Protocol version field | `0x572` | `0x731` |
| Application image size | 314,844 bytes | 370,032 bytes |
| Initial stack pointer | `0x20050000` | `0x20050000` |
| Embedded version string | `GB1.1.5143.4486` | `DR1.1.5143.5706` |
| Version-block product bytes | `01 64` | `01 63` |

The shared container structure is verified; the differing protocol fields do not by themselves establish differing transfer behavior. The common version-string component suggests shared code ancestry, not hardware identity.

Upstream Tracks documentation identifies an STM32F745VEH6 MCU, DSP56724, 32 MB NOR, 32 MB SDRAM and AK4556 codec. Its notes report identical alternate-mode DSP programs in the two firmware images and different main DSP workloads. Those are upstream findings; this investigation has not independently established Rhythm board wiring or compared every peripheral initialization routine.

The Tracks bootloader analysis reports product checking, a changed-version-block requirement, staged NOR installation and CRC checking. Therefore simply relabeling a `.syx` wrapper is insufficient even under the Tracks model. Whether the Rhythm loader uses equivalent checks, staging boundaries and recovery behavior requires its own evidence. A full hardware dump is distinct from the official application update: these downloads do not supply the Rhythm bootloader, settings, external flash or runtime RAM.

## Connected device

macOS IORegistry reports an active USB device named `Circuit Rhythm`, vendor `0x1235`, product `0x0138`, owned by MIDIServer. This confirms USB enumeration. The installed application build and bootloader version have not yet been queried. USB `bcdDevice` is not treated as an application firmware version.

Normal operation mode is sufficient for the next step: a standard MIDI identity request on the Rhythm's USB endpoint, recording raw response bytes and installed build. No mode change is needed for offline application analysis. This investigation found no established firmware-readback command in the reviewed tools; bootloader mode should not be assumed to expose flash dumping.

## Next steps

1. Add a narrowly scoped MIDI identity probe, targeting Rhythm only, to establish installed firmware and distinguish the normal and bootloader endpoints.
2. Verify the Rhythm-specific documented bootloader entry/recovery procedure before requesting a mode change. Capture its identity without initiating an update.
3. Compare reset paths, clocks, GPIO/button/LED mapping, codec and DSP initialization, external-memory geometry, and storage paths/formats in both applications. Establish whether synth factory assets are embedded or stored separately; a Tracks application alone may not supply a usable Tracks pack.
4. Obtain or read the Rhythm bootloader using a proven non-destructive method. Upstream used SWD/ST-Link on Tracks. If Rhythm read protection prevents access, stop before any unlock operation that could mass-erase flash; this is not equivalent to loading a modified application.
5. Back up internal content through Components and the microSD independently. Prepare a separate Tracks-format test pack/card and keep known-good Rhythm recovery firmware locally.
6. Only after compatibility and recovery are established, build a minimal port with explicit model changes, inspect its diff, and validate offline. Do not combine the first port with the RC's full feature set. The first hardware test should establish boot, controls, storage and audio before iterative features.

## Local research sources

- `research/CIRCUITS-HACKS-RC`: commit `3d6d25211a0fee1282df9303be07f27891dce815`; inspect `tools/ct_fw.py`, `tools/patch_official.py`, `tools/ct_uploader.html`, `LEEME.md`, `docs/LOCAL.md`, `docs/NOTAS.md`, `docs/RECURSOS.md`.
- `research/CircuitTracksReverseEngineering`: commit `d6aec9fc9d975d65f54ed5ca16c096227e6974ab`; hardware documentation, original Tracks dumps and bootloader Ghidra project.
- `research/novation-firmwares.json`: official catalog snapshot.
- `firmware/official/`: private local official SysEx files and decoded applications. Ignored to avoid accidental redistribution.

Run `python3 tools/inspect_firmware.py` to reproduce the offline container checks without communicating with hardware.

## Primary references

- https://github.com/Ghillermo/CIRCUITS-HACKS-RC
- https://github.com/Ghillermo/CIRCUITS-HACKS-RC/blob/main/docs/NOTAS.md
- https://github.com/userx14/CircuitTracksReverseEngineering
- https://components.novationmusic.com/api/v2/firmwares
- https://components.novationmusic.com/api/v2/firmwares/168/file
- https://components.novationmusic.com/api/v2/firmwares/169/file
