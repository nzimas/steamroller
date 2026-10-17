# SteamRoller 0.3.2 — transfer instructions

**SteamRoller** is the new name of this firmware (formerly Rhythm FM).
0.3.2 is 0.3.1 under the new name: nothing else has changed.


**Since 0.3.1:**
- **Bit crusher** presets are much stronger. 0.3.0 only reached about
  10-bit resolution, so it was barely audible. The pads now go from about 6.5
  bits down to 5 bits, with sample-rate reduction down to about 1.5 kHz. They
  alternate between grit (bit depth) and aliasing (sample rate), and get
  heavier from left to right.
- **FX rows use the stock Grid FX colours**, taken from the firmware's own
  Grid FX view.


**Since 0.3.0 — Grid FX locks per step, on that track only.**
With Record engaged, hold a step and scroll down with the arrow past level,
pan, delay and reverb. Seven more rows follow, two at a time. Each row's 8
pads are 8 settings of one Grid FX, growing in intensity (or rate) from left
to right:

| Row | Effect | Left → right |
|---|---|---|
| light blue | bit crusher | gritty → destroyed (fewer bits, lower sample rate) |
| pale yellow | stutter (the Grid FX gater) | slow → fast, tempo-synced |
| red | looper | long loop → short loop, tempo-synced |
| indigo | auto-filter | slow, shallow → fast, deep |
| magenta | vinyl | light → heavy |
| orange | reverser | long → short (the engine has only four lengths, so pads come in pairs) |
| green | phaser | slow, light → fast, deep |

The lit pad is the step's setting. Press it again to remove it. A step can
lock several effects at once.

**Only the track that holds the lock is affected.** The Rhythm normally runs
the Grid FX on the whole mix. 0.3.0 adds an FX bus to the sound chip's mixer:
- **While a locked hit plays:** only its track goes through the Grid FX, and
  every other track bypasses them.
- **How long:** the effect lasts until that track's next hit without FX
  locks (or until you stop).
- **When two tracks hold FX locks at once:** the newest lock takes the bus,
  and the earlier track goes back to dry.
- **With no FX locks playing:** every track goes through the Grid FX as
  before, so the Grid FX pads still work on the whole mix (they then skip
  the reverb/delay returns and the audio input).

FX locks belong to the step: deleting the step deletes them, and copying it
copies them.


**New in 0.2.12 — the real cause of the click on the wrong side.** The
Rhythm's sound chip never switches a track's level or pan instantly. It
glides to the new value, and the stock glide takes about 100 ms to finish
(I measured it by running the stock chip program). A lock that changes pan
or level on a hit therefore let the kick's first ~100 ms play partly at the
old setting. That was the faint attack you heard on the left even at level
0. The Mixer view never shows this because its value is set long before the
note. The glide now settles in about 5 ms (it still ramps smoothly sample by
sample, so there is no zipper noise). Each lock goes out a tick before its
hit, so it is in place when the kick starts. Mixer-view level and pan moves
now follow your hand more tightly too.


**Since 0.2.11 — locks are written straight to the sound chip.** The click on
the wrong side came from how the Rhythm passes a parameter change to its
sound chip. A change (a lock, or a Mixer-view pan) is only noted down at
first, and the main loop sends it to the chip a moment later. A note, on the
other hand, goes to the chip immediately. So a lock always arrived just
after its note's attack. A Mixer pan, set long before, was never affected.
Every lock is now also written straight to the chip, the same immediate way
a note is. It also goes out one sequencer tick before its hit.


**Since 0.2.10:**
- **Locks catch the transient.** A lock's change reached the sound a few
  milliseconds after the note started, so a hard-right kick still clicked on
  the left. Each hit's locks (and the return to the knob value before an
  unlocked hit) are sent one sequencer tick early (0.2.11: and written
  straight to the sound chip).
- **Locks belong to the step's content.** When you remove a step (or its
  sample/trigger), its locks are erased too. They don't come back if you add
  the step again. A lock can only live on a step that plays something.


**Since 0.2.9 — a lock lasts the whole sound.** A step's lock now carries
on through the steps that don't trigger anything, so the kick you pan right
on step 1 stays right for its whole tail. The parameter goes back to the
knob's value only when the next hit *without* a lock starts. This applies
to every lock: the sliders, the knobs and spice.


**Since 0.2.8 — step sliders for level, pan, delay and reverb.**
The eight macro knobs keep their usual jobs. To lock a step's mix:
1. Engage **Record**.
2. In a track's normal view, **hold a step**. The two bottom pad rows stop
   showing samples and become sliders for that step:
   - **third row: level** (green), from left (minimum) to right (maximum);
   - **fourth row: pan** (cyan), a bipolar slider: pads 1–4 pan left, pads
     5–8 pan right. Press the middle two pads together for centre.
3. Still holding the step, press the **sample-page arrow** that normally
   goes to the next sample page. The rows move down one parameter: pan/delay,
   then **delay send** (yellow) / **reverb send** (violet). The other arrow
   moves back up.

A **bright** bar means the step has a lock. A **dim** bar shows the knob's
own value (no lock). Press any pad to set the value. Press the pad at the end
of the bright bar to remove that lock. When you let go of the step, the rows
return to level/pan. With Record off, holding a step and pressing a sample
pad is the stock sample flip, as before.


**New in 0.2.7 — spice:** hold **Shift** and press a **track button** to add
random parameter locks to that track's current pattern. The Sample Rec view's
generate mode keeps Shift + track for replaying a sound. Before adding
anything, it reads the pattern's structure: its length, which hits repeat in
every bar (the backbone), which ones change (fills), accents and ghost notes,
rolls, how much room each hit has, and what the other tracks play on the
same step. It then picks 3–4 parameters (always pan or level, plus a
distortion or filter parameter) and moves them by role:
- **Backbone downbeats** keep their place and level.
- **Ghost notes** get quieter, darker, thinner or panned.
- **Fills** build: a crescendo, or the filter opening.
- **Delay and reverb throws** go on hits with room after them, such as the
  pattern's last hit.

Repeating hits get the same move every bar, so the groove keeps its shape. A
lock lasts until the track's next hit. Pitch, decay, attack and start/end are
never touched, and your own locks are kept. **Press it again** for a
different take: the pattern first goes back to how it was before spice.
Locks save with the project like any other. To remove them, hold Clear and
turn the knob (this clears that knob's whole lane). Press **Play** once after
loading a project, before the first spice: the track's sequencer must have
run once.

**Fixed in 0.2.4:** a single sound stored in commit mode could carry crackle or
stutter from the replay that was still playing. Stored sounds are now clean,
like kits. (Samples stored with 0.2.3 keep their artefacts: store them again.)


**A new direction.** The Rhythm is back to its stock sampler on all eight
tracks. The FM engine and the live drum tracks are gone. What stays from
SteamRoller:

- **Sound generator** — in the **Sample Rec view**, with two pads of the third
  row (pads counted left to right, top to bottom):

  | Pad (third row) | Function | LED |
  |---|---|---|
  | third from the right | **kit** (momentary) | dim purple; blinks while working |
  | second from the right | **generate** mode | dim green; bright while on |
  | rightmost | **commit** mode | dim red; bright while on (yellow while storing) |

  Press a mode pad again to turn it off. Only one mode is on at a time.

  **Kit:** tap the kit pad to fill the **current Sample Rec page** with
  **16** new sounds (overwriting that page).
  - On an empty pack that is a balanced starter kit: kicks, snares, a clap,
    an impact, hats, a cymbal, metal, noise, a click and a destroyed sound,
    grouped by rows.
  - **To expand the kit, go to another page and tap again.** The new 16 are
    planned from every generated sound already in the pack (all pages).
    Each expansion has its own **focus**: three or four kinds of sound the
    pack is shortest of (e.g. destroyed sounds, snares and metal; or hats,
    clicks and noise), which fill most of the page and lead its first row,
    plus a few other sounds. Within each kind it uses the characters and
    variants that are rarer so far. Over all 8 pages the kit still evens out
    (about a quarter kicks, then snares, hats, metals, noise, clicks,
    impacts …).
  - Every generated sample carries a small tag in its WAV header, so this
    works across power-offs. Samples you recorded yourself are not counted.
  - **Hold** the kit pad for 1.5 s to **clear** the current page (16 slots).

  1. **Generate mode:** press a **track button 1–8** for a new sound of that
     family, played on that track. Press again for another one. **Shift +
     track button** plays the current sound again. Track buttons don't leave
     the Sample Rec view in this mode.

     | Button | Family |
     |---|---|
     | 1 | kick (widest variety) |
     | 2 | snare (sometimes clap-built) |
     | 3 | hi-hat / cymbal |
     | 4 | metal (clang, metal, anvil) |
     | 5 | noise |
     | 6 | glitch / click |
     | 7 | impact (industrial hits, always driven) |
     | 8 | gnarly (any family, always destroyed) |

  2. **Commit mode:** a track button plays that track's sound again. To store
     it, **hold the track button and press a slot pad** (rows 1–2, the current
     page). The sound is written into that slot of the current pack (it
     overwrites what was there, in every project using this pack) and plays
     once from the slot. Record does not need to be engaged.
     - If the store can't start (no sound on that track, already storing,
       recording), the commit pad flashes orange and the pad acts as stock.
  3. Sounds last until they fade out (at most 2 s), are stored at −1 dBFS,
     and the stored sample is exactly what you auditioned.

- **Parameter locks:** with Record engaged, hold a step and turn a knob. The
  value plays on that step only, and the next unlocked step returns to the
  knob. Macros are stock again (pitch, decay, distortion, filters …). Level and
  pan locks come from the Mixer lanes.

1. Save and back up your work. Close Components, DAWs and other MIDI apps.
2. Open `Start Transfer Tool.command` in this extracted bundle. Keep Terminal
   open. It starts a local server and opens the existing Chrome or Edge.
   If no browser tab appears, paste the exact local URL printed in Terminal
   into Chrome. Make sure the page header says **STEAMROLLER 0.3.2**.
3. Select **SteamRoller 0.3.2 — custom Circuit Rhythm firmware**, then **Load packaged file**.
   Wait for **Verified**.
4. Connect the Rhythm directly by USB with reliable power. Power off, then hold
   **Sample Rec + Samples + Note** while powering it on for bootloader mode.
5. Choose **Allow MIDI and check device**, grant SysEx permission and select
   the Rhythm bootloader ports if needed. Confirm **bootloader mode** (build 4799).
6. Check the acknowledgment, choose **Transfer firmware…** and confirm the
   selected 0.3.2 file.
7. Keep power and USB connected until **Device reported successful receipt**
   and restart.
8. After restart, select the normal-mode ports and check the device again.
   This revision must report **application mode · build 15746**. Build
   **15713** means FM memory initialization failed; save the log and report it.
9. Test, at low volume, in a disposable project:
   - **Stock behaviour:** samples on all tracks play, record and load as on
     the official firmware.
   - **Generator:** in the Sample Rec view, turn generate mode on (third row,
     second pad from the right: green) and press each track button several
     times. Note families that sound too plain or too wild.
   - **Store:** use a disposable pack or a slot you don't need. Turn commit
     mode on (rightmost pad of the third row: red), hold a track button and
     press a slot pad. The pad should turn yellow briefly, then the slot
     plays. Play the slot from a track, then power-cycle: the sound must
     still be there. The log shows `commits`, `commitErrors`, `lastStored`,
     `storeRefusals` and `modes`.
   - **Kit:** in a disposable pack, tap the kit pad on page 1 and wait until
     it stops blinking. Go to page 2 and tap again: the expansion should
     complement page 1. Hold the kit pad to clear a page. The log shows
     `kits`, `kitClears`, `kitProgress`, `kitKnown` (how many generated
     sounds the last expansion found in the pack: 16 per filled page) and
     `kitFocus` (the kinds it focused on).
   - **Locks:** hold a step and turn a knob (Record engaged). Lock level and
     pan from the Mixer view too: they now go the way 0.1.19 proved on the
     hardware.
   - **Lock timing:** lock a kick to level 0 and pan hard right: it must be
     silent on both speakers. Then pan a kick hard right with a step slider and compare it
     with a hard-right pan in the Mixer view: no click should come from the
     left. Then delete a locked step, add it again: it should have no lock.
     The log shows `locksEarly`, `locksSwept` and `locksDirect`
     (values written straight to the chip).
   - **Grid FX locks:** on a kick step, lock the stutter and play: only the
     kick should stutter. Add a looper lock on a hi-hat step and check that
     the newest lock takes over. Try every row and tell me which settings feel
     wrong (the presets are my first guess at each effect's range). Also play
     the Grid FX pads by hand with no locks: they should still affect
     everything. The log shows `fxHits`, `fxClaims`, `fxReleases` and
     `fxState`.
   - **Mixer:** turn level and pan in the Mixer view while a loop plays and
     tell me if you hear any zipper or crackle (the glide is now much faster).
   - **Step sliders:** press Record, hold a step: the bottom rows should show
     level (green) and pan (cyan). Set a few steps, scroll with the arrows to
     delay/reverb, then play and listen. Tell me if the arrows scroll the wrong
     way (the arrow for the next sample page should move down). The log shows
     `stepLocks`, `stepUnlocks`, `lastStepLock` and `stepState` (whether the
     Record state was found).
   - **Spice:** make a pattern with a few hits (accents, ghosts, a fill),
     press Play, then Shift + its track button. Listen, press again for
     other takes, and try a busy hi-hat track and a sparse one. The log shows
     `spices`, `lastSpice` (track, pattern, number of locks, the parameters
     used, the period it found), `spiceRefusals` and `spicePlayers`.
10. Click **Check selected device again** once *before* playing (this resets
    the "since previous check" figures), play the busy pattern for a minute or
    two, click **Check selected device again** with the normal-mode ports, then
    **Save log** and send it. Most useful fields:
    - `avgRenderCycles`, `avgTransferCycles`, `maxTransferCycles`,
      `avgDrumCycles`: processor cycles per 0.67 ms audio block (about 64,000
      cycles is the whole processor). These measure the real cost.
    - `timerBacklogA` / `timerBacklogB`: how far the Rhythm's internal timer
      counters fell behind. Values near 254 mean ticks were lost, which slows the
      tempo.
    - `services` (main-loop rate), `minQueued`, `maxCallCycles`,
      `dspUnderruns` (dropouts), `voiceSheds`, `drumRandoms`, `lastPadPress`.
    Report any crackling and whether the tempo stayed right.

Random patches are still not saved: power-off or loading a project returns to
the boot defaults. If a patch sounds wrong, note its track's seed from the log.

If it fails to boot, return to bootloader mode using the same button combination,
select **Official Rhythm 5706 — recovery**, load it and transfer it. The original
file is included in `recovery/`. Novation Components **Reinstall Firmware** is
another recovery route:
https://support.novationmusic.com/hc/en-gb/articles/360002211360-Updating-firmware-using-Novation-Components

The sender stops on unexpected ordering, device errors or timeouts and never
retries automatically. Save the log on failure. **Stop sending** cannot undo data
already sent; leave power connected while the device finishes any activity.

Close the browser and press Ctrl+C in Terminal when finished. Starting the
launcher, loading a file, building and offline tests do not transfer firmware.
