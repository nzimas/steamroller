# Changelog

## 0.3.2

Changed

- The firmware is now called SteamRoller (formerly Rhythm FM). Nothing else changed from 0.3.1.
- A one-page installer in the browser: find the Rhythm, install, done. Only the SteamRoller firmware is served.

## 0.3.1

Fixed

- The bit crusher was barely audible at any setting (its presets reached only about 10-bit resolution). Its eight pads now go from about 6.5 bits to 5 bits, with sample-rate reduction down to about 1.5 kHz, alternating grit and aliasing.

Changed

- The Grid FX rows use the colours of the Rhythm's own Grid FX view.

## 0.3.0

Added

- Grid FX locks per step, on single tracks. Below reverb in the step sliders come seven rows: bit crusher, stutter, looper, auto-filter, vinyl, reverser and phaser, each with eight settings from gentle to extreme. While a locked hit plays, only its track goes through the Grid FX (a new FX bus in the sound chip's mixer); the effect lasts until the track's next hit without Grid FX locks. When two tracks hold Grid FX locks at once, the newest takes over.

Changed

- With no Grid FX locks playing, the Grid FX process the tracks only, not the reverb and delay returns or the audio input.

## 0.2.12

Fixed

- A locked pan or level still let the first ~100 ms of its sound play at the old setting (a hard-right kick clicked on the left, even at level 0). The sound chip glides every level and pan change over about 100 ms; that glide now settles in about 5 ms, still smoothly. Mixer moves follow the hand more tightly too.

## 0.2.11

Changed

- Every lock is also written straight to the sound chip, the way a note is, rather than waiting for the main loop.

## 0.2.10

Changed

- A step's locks are sent a moment before the step, so they are in place for its transient.
- Locks belong to the step's content: deleting a step deletes its locks.

## 0.2.9

Changed

- A lock lasts for the whole sound of its step: steps that trigger nothing keep it, and the next hit without a lock returns to the knob's value.

## 0.2.8

Added

- Step sliders: with Record on, holding a step turns the two bottom pad rows into sliders for that step's level and pan, then delay and reverb with the arrows.

Fixed

- Level and pan locks now move the mixer (they go the same way as a live Mixer-view change).

## 0.2.7

Added

- Spice: Shift + a track button adds random locks to the track's pattern after reading its structure (backbone, fills, accents, ghost notes, room after each hit, the other tracks). Pressing again gives another take.

## 0.2.6

Changed

- Kit pages beyond the first each focus on the three or four kinds of sound the pack has least of, so pages differ from one another.

## 0.2.5

Added

- Kits grow page by page: each new page is planned from every generated sound already in the pack, up to 128 sounds. Generated samples carry a tag so this works across power-offs.

## 0.2.4

Fixed

- A sound stored on its own could carry crackle from the audition still playing.

## 0.2.3

Added

- The kit pad: one tap fills the Sample Rec page with a balanced kit of 16 sounds; holding it clears the page.

## 0.2.2

Changed

- Generate and commit modes on two pads of the Sample Rec view; storing a sound is hold a track button + press a slot.

## 0.2.1

Added

- Generated sounds can be stored as samples in the current pack.

## 0.2.0

Changed

- A new direction: the stock sampler on all eight tracks, with the drum voice as a sound generator in the Sample Rec view (eight families). The FM synth tracks of the 0.1 versions are gone; parameter locks stay.

## 0.1

Added

- The 0.1 versions (as Rhythm FM) put a six-operator FM synth on tracks 1–3 and FM drum voices on the others, and introduced Elektron-style parameter locks: a lock plays on its step and the next unlocked step returns to the knob.
