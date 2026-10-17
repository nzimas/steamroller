# First hardware attempt: updater receipt without FM behavior

The user transferred the original 0.1 candidate and reported updater success,
but tracks 1 and 2 continued playing samples. FM operation was not established.

Inspection of the delivered binary confirmed that its entire 38-byte version
block at image offset `0x1C8` matched stock Rhythm 5706 exactly. A build marker
at the end of the image did not change that block. This was a build defect missed
by the offline integration checks.

The local reference project's `docs/NOTAS.md`, section “Bloque de versión”,
documents a Tracks bootloader comparison at `0x08003BE4`–`0x08003BF6`:
equal version blocks cause a staged image to be skipped after successful
receipt. Its `tools/build_shared_channels.py` explicitly stamps the padding to
avoid this. The Rhythm bootloader has not been dumped or emulated, so the same
gate on Rhythm is an inference consistent with the observation, not a confirmed
readback or proof that it is the only defect.

Version 0.1.1 makes these specific changes:

- Write `RF0101` into the six padding bytes at image offset `0x1E8`.
- Preserve all original product and official version fields in that block.
- Change the stock standard MIDI identity response literal at `0x080270D8`
  to report diagnostic application build 15706 instead of 5706. The actual
  identity handler at `0x08026F0C` is emulated with a standard identity inquiry
  and a captured MIDI send; it returns the patched reply. No new device command
  or arbitrary memory-read facility is introduced.
- Require the changed version block, marker and identity in offline validation
  and firmware approval. Test startup with RAM produced by the real reset path,
  rather than relying solely on a hand-initialized heap fixture.

The FM engine, note routing and DSP integration remain the same as 0.1. The
0.1.1 package is for a hardware retest. After restart, normal-mode application
identity 15706 distinguishes the new image from stock 5706; bootloader identity
4799 does not identify the application. Identification does not attest that the
FM engine initialized or that audio works. Those require separate hardware
observation. No firmware transfer or device access was performed while preparing
this correction.
