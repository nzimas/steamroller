# Circuit Rhythm FM firmware

**Current candidate: Rhythm FM 0.1.13**, based on official Rhythm build 5706:

- **Tracks 1–3:** Tonitor's six-operator FM engine, one voice each; the sample-track distortion and filter knobs act on them through the Rhythm's own DSP processing.
- **Tracks 4–6:** Vascular's poundhard FM drum voice; the Sample view's third-row pads choose kick, snare, hi-hat, clap, tom, metal, noise, glitch (the selected sample slot), Shift + pad draws a new drum.
- **Tracks 7–8:** native sample player.
- **Shift + track button 1–3:** draws a new random patch for that FM track — dark ambient or rhythmic noise, never with pitch glides (not saved; not loudness-normalized).
- Future versions will add assignable track engines; this version uses a fixed 3 FM / 3 drum / 2 sampler split.

0.1.8 worked on the unit. 0.1.9 added polyphony, native distortion/filters on FM (confirmed working) and the ambient/noise randomizer, but overlapping notes crackled: the ~96 MHz, cache-less MCU could not render two voices from flash in time. 0.1.10 ran the FM render path from zero-wait ITCM RAM (much better on the unit, occasional clicks/cutoffs remained). 0.1.11 deepened the FIFO and fixed the cycle-count telemetry. 0.1.12 added the drums (worked, but crackled). 0.1.13 moves to FM 1–3 / drums 4–6 / samples 7–8 with a faster drum renderer. Not yet tested on hardware. Verify normal-mode identity **15722**; **15713** means FM memory initialization failed.

Earlier history: 0.1 retained sampler behavior (installation block matched stock); 0.1.1 had a non-word-aligned CRC failure; 0.1.2 exceeded DSP program RAM; 0.1.3 failed startup, isolated by D2/D3 to a heap-limit reduction; 0.1.4+ use ITCM state without shrinking the heap. See [the boot-failure investigation](docs/RHYTHM-FM-BOOT-FAILURE.md), [the late-boot investigation](docs/RHYTHM-FM-LATE-BOOT.md) and [the audio investigation](docs/RHYTHM-FM-AUDIO-FAILURE.md).

Read [the firmware notes](firmware/rhythm_fm/README.md) for behavior, the randomizer design, build instructions and remaining hardware checks. Build with `make rhythm-fm`; run emulator validation with `make rhythm-fm-check`. Neither target installs firmware. Version identities live in `tools/rhythm_fm_version.py`.

Generated firmware files, source hashes, patch manifest, validation reports and FM audio preview are in `build/rhythm-fm/`. The underlying reverse-engineering reports remain in [the research index](docs/RESEARCH-INDEX.md).

The complete replacement bundle is `dist/rhythm-fm-0.1.13.zip`: firmware, local browser uploader, official recovery SysEx, transfer checklist, source and validation reports. Read [the transfer checklist](tools/rhythm_uploader/TRANSFER.md). Launch the extracted `Start Transfer Tool.command`, or run `python3 serve.py` in the extracted folder using the existing Python and Chrome/Edge. No software installation is required. The uploader requests MIDI access only when chosen and writes firmware only after explicit transfer confirmation and a fresh Rhythm bootloader identity check.

Rebuild and validate the complete bundle with `make transfer-bundle`. All checks are offline; none of the build, validation or packaging targets accesses the instrument.
