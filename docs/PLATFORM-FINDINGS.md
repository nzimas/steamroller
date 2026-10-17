# Second investigation pass: hardware compatibility evidence

**Scope correction:** The project now targets feature integration into the Rhythm application, not replacement with the Tracks application. See [DESIGN-BRIEF.md](DESIGN-BRIEF.md). Cross-flash and Tracks-format storage suggestions in this historical report are not the current plan; the verified low-level similarities remain useful research evidence.

Date: 2026-10-15. This pass sent one standard MIDI identity inquiry in normal mode and one in bootloader mode to the connected Rhythm. It sent no firmware-update commands, storage commands, or diagnostic payloads. All application execution described below happened in a local emulator.

## Connected unit

The normal-operation identity response is:

`F0 7E 00 06 02 00 20 29 01 63 00 00 01 01 2C 4A F7`

Novation manufacturer ID is `00 20 29`; family bytes are `01 63`; model bytes are `00 00`. The build decodes as `0x2C * 128 + 0x4A = 5706`, matching the official catalog's Rhythm 2.0.1. The complete capture and its interpretation are saved in `device-identity-normal.txt`.

The upstream uploader's `d[9] === 1/17` mode test is incorrect for this response: byte 9 is `0x63`, part of the family field. Its manufacturer-only `isTracks` variable also does not distinguish Tracks from Rhythm. We must not use that UI's device/mode classification as a compatibility check. Our probe displays the raw bytes and does not classify operating mode from that field.

## Directly verified similarities

The comparison uses official, hash-checked Tracks 4486 and Rhythm 5706 applications. The script rejects other images before applying these version-specific addresses.

| Evidence | Tracks address | Rhythm address | Verification |
| --- | --- | --- | --- |
| Early CPU `SystemInit` function | `0x0801BC78` | `0x08023F50` | All 60 instruction bytes identical |
| Static GPIO-record table in reconstructed RAM | `0x20002B6C` | `0x20002B7C` | 47 consecutive 32-byte candidate records identical, 1,504 bytes total |
| GPIO configuration routine | `0x0801BEB0` | `0x08024188` | Unique identical 32-byte entry anchor; relocation differences later in routine |
| GPIO port-clock routine | `0x0801C1EE` | `0x080244C6` | Unique identical 32-byte entry anchor |
| GPIO alternate-function routine | `0x0801C270` | `0x08024548` | Unique identical 32-byte entry anchor |
| Alternate DSP core 0 bootstrap | `0x08054EFC` | `0x08062130` | Entire 2,430-byte block identical |
| Alternate DSP core 1 bootstrap | `0x0805587C` | `0x08062F20` | Entire 1,086-byte block identical |

The SystemInit literal references also resolve to the same FPU register `0xE000ED88`, RCC base `0x40023800`, RCC masks/defaults, vector-table base `0x08010000`, and VTOR register `0xE000ED08`. Other startup data pointers relocate.

The static records cover GPIOA through GPIOE, including matching port addresses, pin masks and remaining configuration words. We compare the eight raw words per record rather than assigning unverified meanings to every field. They were obtained by running reset and C-runtime initialization with Unicorn's Cortex-M7 model, stopping at the application entry (`0x0801D3C8` Tracks, `0x08025688` Rhythm). Both reached that point successfully. This execution does not emulate the bootloader, initialized storage, external RAM, audio hardware, or the complete application.

Five direct BL callsites to the GPIO configuration routine were found in each application, with matching surrounding instruction structure. This supports common drivers but does not prove that every record is used identically at runtime. Physical board wiring and hardware revisions remain unverified.

DSP bootstrap tables begin at `0x08029700` (Tracks) and `0x08031658` (Rhythm). The latter matters: `0x08031660`, mentioned in upstream notes, points to the second pair, not the start of the four-entry table.

| DSP download payload | Tracks words | Rhythm words |
| --- | ---: | ---: |
| Main core 0 | 10,846 | 8,173 |
| Alternate core 0 | 808 | 808 |
| Main core 1 | 364 | 14,187 |
| Alternate core 1 | 360 | 360 |

Both main payloads differ. The complete block hashes and all static GPIO records are in `platform-comparison.json`. Subsequent reconstruction established that Rhythm's core-1 main payload contains segmented P/X/Y records, including 3,124 populated P words; its 14,187-word download length is not its executable program size. See [DSP-CONSUMER-FINDINGS.md](DSP-CONSUMER-FINDINGS.md).

## Interpretation

The evidence now extends beyond similar chip specifications: both applications carry identical static pin configurations and identical alternate DSP programs, alongside shared low-level initialization. A port is increasingly plausible. This does not establish that changing the model bytes alone is sufficient or that cross-flashing is recoverable on this unit.

The original reverse-engineering repository also contains `Tools/circuitToTracksPatcher.py`. Despite its ambiguous name, it opens the Rhythm image, changes application offset `0x1CD` from Rhythm's `0x63` to Tracks' `0x64`, and packages for Tracks. It does the opposite direction of our intended port and is not proof of a successful conversion; the reviewed material provides no accompanying hardware test result. It has not been run.

## Recovery procedure and remaining work

Novation's official support page explicitly documents **Sample Rec + Samples + Note while powering on** for Rhythm bootloader mode:

https://support.novationmusic.com/hc/en-gb/articles/360002211360-Updating-firmware-using-Novation-Components

The user entered that mode and a second standard identity inquiry succeeded. The endpoint is `Circuit Rhythm Boot MIDI` and its raw reply is `F0 7E 00 06 02 00 20 29 11 63 00 00 00 03 25 3F F7`. The advertised version bytes are `00 03 25 3F`, with build `4799`. The first family byte changes from `01` to `11`, while the second remains `63`. This establishes a measured normal/bootloader identity distinction for this unit and confirms that manual bootloader entry works. It does not extract the loader or prove update acceptance, recovery after a bad application, or memory-write boundaries. Full capture: `device-identity-bootloader.txt`.

The catalog snapshot contains only application releases for Rhythm and Tracks, with no separate bootloader download. Rhythm application 1.0 also carries build 4799; that does not imply its update file contains this bootloader. For comparison, the original Tracks full-flash dump carries bootloader version block `GB0.4.2920.3556` at `0x080001C8` and application block `GB1.1.5143.4486` at `0x080101C8`, illustrating the separate versioning and binary regions.

Further work should establish:

1. Rhythm update validation/recovery behavior from Rhythm-specific evidence. Bootloader identity and manual entry are now measured.
2. Higher-level peripheral setup, pad/button mappings, audio routing and any board-variant selection.
3. Storage layout and the boundary between the internal factory pack, global settings and update staging. Tracks data and synth patch assets must be prepared independently of Rhythm content.
4. Whether a non-destructive bootloader readback is possible. No arbitrary SysEx probes, debug unlocks, or option-byte changes have been attempted.

## Reproduction

Install the pinned dependencies into the project `.venv`, then run:

```
.venv/bin/python tools/compare_platform.py
```

On this Mac, Unicorn raises a host illegal-instruction fault when its JIT runs inside the restricted sandbox, even for one trivial emulated instruction. The same smoke check and full startup comparison succeed in an approved JIT-capable execution context. This was an execution-environment restriction, not evidence that either application failed startup.

The identity utility can be built with:

```
clang tools/midi_identity.c -framework CoreMIDI -framework CoreFoundation -o /tmp/rhythm-midi-identity
```

Running it requires access to the local CoreMIDI service. It selects exactly one source and destination whose display names contain `Circuit Rhythm`, sends the fixed six-byte universal identity inquiry once, reads for three seconds, and exits. It has no arbitrary-message or update options.
