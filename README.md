# SteamRoller — custom Circuit Rhythm firmware

SteamRoller (formerly Rhythm FM) is custom firmware for the Novation Circuit Rhythm, built on the official firmware
5706. It keeps the stock sampler and sequencer, and adds:

- **A poundhard sound generator** in the Sample Rec view: eight families, store to slots, and kits that grow page by
  page.
- **Elektron-style parameter locks.** A lock lasts for the whole sound and is in place for the transient.
- **Spice:** Shift + track adds structure-aware random locks.
- **Per-step sliders** for level, pan, delay and reverb, plus **per-step Grid FX on that track only**: an FX bus is
  patched into the DSP mixer.

## Where to look

- [firmware/rhythm_fm/README.md](firmware/rhythm_fm/README.md): behaviour and design, release by release.
- [tools/rhythm_uploader/TRANSFER.md](tools/rhythm_uploader/TRANSFER.md): how to install and test.
- [NEXT-AGENT-HANDOFF.md](NEXT-AGENT-HANDOFF.md): current status.
- [docs/](docs/): the research maps.

## Building

`make transfer-bundle` builds, validates and packages `dist/steamroller-<version>/` plus its `.zip`. Releases up to 0.3.1
are named `rhythm-fm-<version>`.

The official firmware (`firmware/official/`) and the research material (`research/`) are not part of the repository.
