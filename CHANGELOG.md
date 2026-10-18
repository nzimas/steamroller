# Changelog

## 0.6.4

Changed

- BAYAREA sounds no longer all carry the same metallic ring. Its resonator bank was fixed (three resonators at 508, 270 and 1153 Hz); now every sound draws resonators of its own (120 Hz to 3 kHz, inharmonic, ringing 0.1 to 1.2 s), and most families use none.
- BAYAREA turns toward textures and strange noises. Its families are now clunk (a short, folded knock), scrape (noise-like FM held under a sweeping filter), hiss (high FM static), rumble (a low folded drone, slowly moving), whir (a held tone whose shape an LFO churns), clang (inharmonic metal), chop (a held fold whose timbre a square LFO jumps) and gnarly. None moves in pitch.
- BAYAREA kit pages fill impacts with clunks, metal with clangs and chops, clicks with blips, and four texture categories (scrape, rumble, whir, hiss). On the pattern generator's pack path, scrapes and hisses play as noise; rumbles and whirs stay out of drum parts.

## 0.6.3

Changed

- Generated patterns are sparser. The pack path plays kick, snare and hats with at most one or two of percussion, noise and clicks, thinned to a share of the steps per style (38 to 60 %); the one-sound path uses fewer hits too.
- Generated patterns are more deliberate. Every decision for a step (its velocity, which follows the metre, the cell's one ratchet spot, swing, nudges, gate, ghost notes, the sample it flips to, a recurring Grid FX spot) is made per position, so the groove repeats exactly; only the last cell, the fill, adds hits, ratchets and effects. IDM patterns of 20, 28 and 32 steps now repeat in halves.

## 0.6.2

Fixed

- Generated patterns played about an octave below the samples' own pitch. The Rhythm's step notes run from 36 to 108 around 72; generated steps now use 72.
- The pack path flipped no samples: it relied on reading every slot's tag, which found nothing on the Rhythm. It now takes the occupied slots from the Sample Rec view's own map, and the tags (where they can be read) only refine each sample's role. Flipped steps show pink, as when you place them by hand.

## 0.6.1

Fixed

- Generated patterns sounded like farts: 0.6.0 set every step's note to 12, four octaves below the Rhythm's reference note 60 (and kept it on later presses). Every generated step now plays at 60, the sample's own pitch, which also repairs patterns made by 0.5.0 and 0.6.0.
- The path pads in the second Sample Rec view did nothing (the Rhythm keeps the Sample Rec view's id there), so the pack path never ran. The view is now recognized by what is on screen; the Sample Rec view's own pads stay out of it.
- Generated hits are no longer cut to a blip: gates are at least half a step (except within ratchets), and the filter and distortion moves are as strong as spice's, not stronger.

## 0.6.0

Fixed

- The pattern generator played every hit an octave down (it wrote the lowest note into each step), which made it unusable. It now never touches the pitch: steps keep their note and new ones play the sample untransposed.

Added

- Two pattern generator paths, chosen in the second view of Sample Rec (press Sample Rec again) with the first two pads of the bottom row: **one sound** (the track's own sample) or **the pack**, a whole drum part on one track built from the pack's samples by role (kick, snare, hats, percussion, noise, clicks) through sample flips. Generated samples are recognized by their recipe; your own samples by where they sit on the page.

## 0.5.0

Added

- Pattern generator. With Record on, Shift + a track button writes a whole new pattern on that track, in one of four styles: industrial techno, power noise, IDM or broken. It sets the pattern's length (up to 32 steps, odd lengths for IDM), velocities, gates, micro-steps (ratchets, flams, nudges), probabilities, parameter locks and Grid FX locks. With Record off, Shift + a track button still spices the pattern.

Changed

- BAYAREA no longer makes kicks: poundhard keeps that job. Its families are now bongo, clunk (a short, folded knock), pluck, chime, clang (inharmonic metal), folded, sustain (a swell or a drone) and gnarly.
- No BAYAREA sound moves in pitch: no vibrato from slow modulators, no LFO on the FM depth.
- BAYAREA kit pages fill impacts, metal (including the gnarly kind), blips, plucks, folded tones, swells and drones, never kicks.

## 0.4.0

Added

- A second sound engine, BAYAREA: a West Coast voice ported from Tonitor (a carrier morphing from sine to saw, two FM sines, a wavefolder, a low-pass gate or a sustaining envelope, a saturating output and three resonators). In the Sample Rec view, with generate or commit mode on, the leftmost pad of the third row picks poundhard and the next one BAYAREA. The track buttons then draw kick, bongo, pluck, chime, folded, swell, drone and gnarly sounds from BAYAREA.
- Kit pages from either engine. Every page is planned against all generated sounds in the pack, whichever engine made them, and fills only the kinds of sound its engine plays.

Changed

- Generated sounds are tagged with the engine that made them. Sounds made by earlier versions count as poundhard.

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
