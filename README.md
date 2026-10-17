# SteamRoller — custom Circuit Rhythm firmware

**Website, installer and manual: https://nzimas.github.io/steamroller/**

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

## Disclaimer

SteamRoller is an unofficial firmware for the Novation Circuit Rhythm. It is not made, endorsed or supported by
Novation; Novation and Circuit Rhythm are trademarks of their owner, used only to say which device SteamRoller runs
on. Installing it replaces the device's firmware and may void any warranty that applies to your device. An
interrupted update can leave the Rhythm in update mode until the firmware is installed again (SteamRoller's
installer or Novation Components). Back up your projects and packs first.

The software is provided "as is", without warranty of any kind, express or implied, including but not limited to the
warranties of merchantability, fitness for a particular purpose and noninfringement. In no event shall the authors be
liable for any claim, damages or other liability arising from, out of or in connection with the software or its use.
You install and use it entirely at your own risk.

## Licences

The firmware extension, tools and installer are GPL-3.0 (`firmware/rhythm_fm/LICENSE`). The website and logo are MIT.
