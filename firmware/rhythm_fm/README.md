# SteamRoller 0.6.4 (formerly Rhythm FM) — stock sampler, poundhard and BAYAREA sound generators, pattern generator, step locks and per-track Grid FX

Identity **15753**; **15713** = extension memory unavailable. Not yet tested on hardware.

## 0.6.4: BAYAREA textures; resonators per sound

- **The ring (0.6.3 hardware: every sound had the same metallic resonance).** Tonitor's bank is fixed: `BA_RES`,
  508/270/1153 Hz. `ba_design` now draws three resonators per sound: f0 120–3000 Hz, then ×1.3–2.7 and ×2.2–5.4,
  T60 0.1–1.2 s. The coefficients are computed at design time (`r = 2^30 − 154527000/T60ms`,
  `c1 = 2r·cos w`, `c2 = −r²`, Q30) into `d->res`. Only the amplitudes of `BA_RES` are still used.
- **Families:** one row of `BA_FAM` each (kind, note, decay, peak, minimum folds and FM depth, resonators, hold,
  LFO rate and targets, flags). The rows are clunk, scrape, hiss, rumble, whir, clang, chop and gnarly, plus the
  kits' blip. The held families are released after their hold.
- **Kits and the pack path:** categories 13–16 are now scrape, rumble, whir and hiss (weights 5/4/4/4). Chop fills
  metal-g. In `pg_role_of`, scrape and hiss are noise; rumble and whir are not drums.
- **Space:** 392 bytes of app flash left.

## 0.6.3 (hardware: BAYAREA all one ring): sparser, per-position patterns (0.6.2 hardware: everything works; too dense)

- **Pack path:** kick, snare and hats, plus up to `extras[style]` of percussion, noise and clicks. Layers are
  thinner, and the hits are strictly capped per cell (`cap_pct`), dropping clicks, hats, percussion and noise in
  that order. Ghost chance is halved.
- **Both paths:** decisions are `PH(salt, position)` (`sp_hash` on a per-pattern seed). They are taken per cell
  position, and per step only in the fill (`key = 64 + s`):
  - velocity by the metre (beats, eighths, sixteenths);
  - one ratchet spot per cell;
  - swing for the whole pattern, or none;
  - IDM nudges at fixed spots;
  - one gate per pattern (techno, power) or per position;
  - ghost spots (at most 1–2 per cell);
  - the sample alternative per position;
  - one recurring Grid FX spot (power, IDM), plus 1–3 in the fill.
- **Densities:** one-sound k is 25–45 % (techno), 35–60 % (power), 25–45 % (IDM), 18–33 % (broken). Fill extras
  are at 20 %. IDM at even lengths uses a half-length cell.

## 0.6.2 (hardware: works; too dense): note 72; pack path from the slot map

- **Pitch (0.6.1 hardware: about an octave low).** The octave shifts refuse to go below 48 or from 97 upwards
  (`0x08057870`, `0x080578D0`), so notes live in 36–108, centred on 72. Written steps now get 72.
- **Flips (0.6.1 hardware: none).** The pack path used only the tag reads (`kit_tags`), which found nothing in the
  Shift + track context; why is not known. Occupied slots now come from the Sample Rec view's map:
  `router[+0x58]` store `[+0x30]`, word `slot >> 5`, bit `slot & 31` (`0x0805985C`). An occupied slot without a
  readable tag is placed by its pad position. Byte 5 is the stock flip (the step lights pink).

## 0.6.1 (hardware: an octave low, no flips): pitch 60; secondary view by the object on screen

- **Pitch (0.6.0 hardware: farts).** The engine's reference note is 60: key tracking `0x080463AC` subtracts 60, and
  the note-on `0x0804AFE2` plays a 0 byte as 48. So 0.5.0 (0, played as 48) was an octave down, and 0.6.0 (12) four
  octaves down, then copied forward. Every written step now gets 60.
- **Secondary Sample Rec view.** `0x0804D6B6(pages, 0xA)` sets `+0x264`, but the view id the router keeps is not
  known (likely still 4). `pg_view` now tests the object on screen (`router[+0x7C]`, or `[+0x78]`, equal to
  `router[+0x3C]`). `in_sample_rec` (view id 4 and the track pages not on screen) replaces the bare view-id test
  wherever the Sample Rec pads, track presses and replays are handled.
- **Sound:** gates are at least 3/6 of a step (ratchets 1/6), and the generator's spice amounts are the normal
  60–100 %.

## 0.6.0 (hardware: farts, no flips): pattern generator pitch fix; one-sound and pack paths

- **Pitch (0.5.0 hardware: every hit far too low).** Step byte 0 is the note: stock `0x08057894`/`0x080578F6` add or
  subtract 12, and the step view creates a step with 12 or the track's note (`0x0805E606`). 0.5.0 wrote 0. Now a
  step that had a hit keeps its note, and a new step takes the pattern's first hit's note (12 when it has none).
- **Paths:** pads 25/26 (24/25 zero-based) of the secondary Sample Rec view set `S->pg_mode`. That view is the
  track view (router view 0, `router[+0x3C]`) in sub-mode 0xA (`+0x264`), which the router selects on a second
  Sample Rec press. Lights are drawn in `fm_track_frame`.
- **Pack path:** `kit_tags` now also returns each slot's category (0xFE: a file without a tag). Roles come from
  categories, or by place for untagged samples. Each style has a Euclidean layer per role, merged by priority
  (snare, kick, noise, percussion, hat, click). Step byte 5 (sample flip) is one of two slots drawn per role.
- **Space:** about 0.9 KB of app flash left.

## 0.5.0 (hardware: pitch far too low): pattern generator; BAYAREA for metal and industrial percussion

**Pattern generator** (`pattern.c`): Record on + Shift + track writes the track editor's current pattern.
- **Styles:** four (techno, power noise, IDM, broken). Euclidean hits per cell; later cells mutate and the last one
  fills; ghosts and fill hits play on a probability.
- **Steps:** written in the stock format: gate (6 = one step, here 1–24), micro-step mask (ratchets 0x09/0x15/0x3F,
  flam 0x03, nudges 0x02–0x08), velocity, probability 0–7, sample flip 0xFF. End/start go to +0x100/+0x101.
- **Lanes:** all cleared. Then spice's engine runs (`sp_apply`, boosted: two palettes and bolder amounts), followed
  by Grid FX bytes on a few hits.
- **Editor reload:** the editor's stores are written back over the pattern on every edit (`0x080482xx` listener),
  so after writing, the editor reloads with `0x08048932(editor)`, stock's own reload on a pattern change.
- **Finding the editor:** the editor is `*([router+0x34] + t·[router+0x2C]) − 0x1C` (the object steplock.c already
  reads on hardware). Its `[+4]` must equal the player's `[+0x50]` when the player has been seen, or the press is
  refused (`S->pg_refusals`).
- **Spice:** its lock pass moved into `sp_apply`, unchanged in behaviour. Lane ids now come from the stock table's
  formula instead of the player.

**BAYAREA:** kicks removed (hardware 0.4.0: too weak).
- Families are now bongo, clunk, pluck, chime, clang, folded, sustain and gnarly, plus blip, swell and drone for
  kits.
- No pitch movement: modulator 1 ratio is at least 1 and the LFO no longer moves the FM depths.
- Kit categories: impact, metal, metal-g, click, destroyed, pluck, folded, swell and drone.

## 0.4.0: a second engine, BAYAREA (hardware: "pretty good"; kicks too weak)

**Engine:** `bayarea.c` is Tonitor's BAYAREA (mvave-fm1 `firmware/src/bayarea.c`) at 48 kHz, all integer, bit-exact
on host and ARM.
- Rate-dependent constants are recomputed: 1500 control blocks/s, phase step 2^32/48000, smoothing lags.
- `tools/gen_bayarea_tables.py` generates `generated/bayarea_tables.h`: the tan table, the 12 kHz Butterworth and the
  resonators (same pitch and ring time). The sine is the shared `SINE`.
- Dropped: Tonitor's macros, envelope page and loudness probe (the generator normalises when storing).
- Added: a 10 Hz DC blocker at the output (the folder left up to 5 % offset).
- `ba_strike` maps nine families (`BAF_*`: eight generate families plus the kits' blip) to a kind, a pitch range and
  drum reshaping. Sustaining kinds are held until their attack is over, then released.

**Generator:** a generator voice (`D[i]`) holds a poundhard voice or, in the same memory, a `BaSlot` (464 bytes);
`S->voice_engine[i]` says which. `gen_start`/`gen_render`/`gen_active` dispatch. Cost (emulated M7, per 32-sample
block): BAYAREA about 15.2k instructions mean, 16.9k max; poundhard 9.9k / 14.3k.

**UI:** pads 16/17 (third row, leftmost two) pick the engine while generate or commit mode is on. Otherwise they stay
stock's threshold-recording pads. LEDs: yellow 0x6D/0x73 poundhard, cyan 0x18/0x19 BAYAREA. `Pending.engine` keeps
each sound's engine, so replays and stores use the engine that made it.

**Kits:**
- Categories are shared: poundhard plays the first 13; BAYAREA plays kick, impact, metal, click and destroyed plus
  pluck, folded, swell and drone.
- A page (`S->kit_engine`, fixed at its start) draws only its engine's categories. Shares are weight / (the
  engine's weights) of the sounds in the engine's categories, whoever made them, so a poundhard-only kit plans as
  before.
- The WAV tag byte 47 is engine + 1 (earlier files: 1 = poundhard). A BAYAREA tag has flavour 4, pad = category and
  kind = family.

**Space:** spice, Grid FX, UI, storing and kits are built `minsize`; `.lowflash` lists its `plock_*` sections
explicitly. About 4.7 KB of app flash remains.

## 0.3.2: renamed SteamRoller

Firmware identical to 0.3.1. The release name, file stem (`steamroller-<version>`), installation marker (`SR0302`) and
build marker string change. Internal paths and identifiers (`firmware/rhythm_fm`, `tools/*rhythm_fm*`, Makefile
targets, report header) keep their names.

## 0.3.1: bit crusher range, stock FX colours

On 0.3.0 hardware, every effect worked except the bit crusher, which was barely audible.

**DSP `P:$504`:**
- Index = param `0x19` / 2. The sample is scaled by the table `X:$4400[index]` (a copy of `X:$C00`), masked to 12
  bits, then scaled back by index + 1.
- That makes the step (value/2 + 1) · 2^-11, so the 0.3.0 values 3–10 meant only about 10 bits.
- The presets now use 40–127 bits together with 1–32 samples of hold (`P:$4E4`).

**Colours:** the FX rows use the stock Grid FX view's colours (`0x0805AB02`: palette `0x08062AB0`, `[0x39 + type]`
latched, `[0x40 + type]` idle).

## 0.3.0: per-step Grid FX locks on a per-track FX bus

See `docs/RHYTHM-GRID-FX-MAP.md` for the stock engine.

**UI:**
- `steplock.c` has rows 4–10 after the mixer rows: bit crusher, stutter (gater), looper, filter, vinyl, reverser,
  phaser.
- Each pad is preset 0–7; the lit pad = the step's preset, and pressing it again removes it.

**Storage:**
- One byte per effect type (`0x80 | preset`; `0xFF` = none) in micro-steps 1–5 of lane 10 and 1–2 of lane 11
  (`gfx_byte`).
- The step editor's five `0xFF` stores to micro-steps 1–5 (`0x08047E32…`) are `nop.w`, so nothing overwrites them.
- Mixer lock reads and clears use micro-step 0 only.
- `lock_sweep` erases the bytes with the step.

**Playback** (`gfx_hit`, from `plock_tick` at tick 0 of a hit, before its note):
- A hit with FX locks sets each locked type's params through the knob router's setter (ids `0xA9 + param − 0x0A`).
- It then sets the type's on param and calls the type's stock DSP writer at once.
- Unlocked types are switched off; the track becomes the bus owner and the route words are written.
- **Release:** the owner's hit without FX, or `plock_revert_all` on stop/reset.

**DSP** (`tools/rhythm_fm_fxbus.py`): 56 words in core-0's unused vectors `P:$80–$B7`:
- **FXSEL** at `P:$38A` mixes a track whose route word `Y:$3770A+t` is 0 into the FX bus `$37900`; otherwise into the
  main bus.
- **SWAP** at `P:$239`, before the looper: dry master `$4AD8` → `$37940`, FX bus → master, FX bus cleared.
- **ADD** at `P:$27A`, after vinyl: master += dry.
- The route words default to 0 (every track through the FX: stock), and the ARM zeroes them on DSP init.

**Validator:**
- `core0_fx_bus_routes_swaps_and_adds_back` (dsptool-io on the built core-0);
- `grid_fx_rows_store_presets_per_step`;
- `grid_fx_locks_take_the_bus_newest_wins_and_release`;
- `step_editor_keeps_micro_steps_1_to_5`.

**Not hardware-confirmed:**
- the preset ranges (phaser and filter param meanings are partly inferred);
- that calling the stock FX writers from the sequencer is timely enough for looper/stutter capture.

**Report v23** adds `fxHits` and `fxState`.

## 0.2.12: DSP core-0 gain glide 2^-6 -> 1/2 per block

On 0.2.11 hardware, a kick locked to level 0 + hard right still sounded its attack faintly on the left, which was the
previous hit's pan.

**Measured in dsptool-io with the stock core-0 program** (flat image at `[0x08031658]`):
- `P:$2D9` computes each track's L/R gains (`X:$9D+3t`) from `Y:$F9/$101`, which `P:$033E` derives from
  `Y:$307D8+32t` level/pan.
- It glides them once per 32-sample block with `a += c·(target − a)`, where `c` = the literal at `P:$2DA`
  (`0x020000` = 2^-6). That is a ~42 ms time constant and ~100–150 ms to settle.

So no ARM-side timing could put a lock on the transient.

**Fix:**
- **Patch:** the literal becomes `0x400000` (1/2 per block: under 1% left after 7 blocks ≈ 5 ms). The mixer still
  ramps per sample inside each block.
- **Early send:** locks go out 1 tick early (2 at rate 7).
- **Side effect:** Mixer-view level/pan moves also glide faster.

**Validator:** `dsp_track_gain_glide_settles_in_blocks` runs the built image's core 0.

## 0.2.11: lock values written straight to the DSP

On 0.2.10 hardware the transient still leaked. Cause:
- **Parameters are delayed.** The setter chain (`[router+8]` → `0x0803898C` → per-track sound params, table
  `0x08063708`) only caches the value and marks it dirty. The main loop's drain (`0x0803081C` → `0x0804070E`) writes
  `Y:$307C8 + 32t + param` later.
- **Notes are not.** Note triggers are written to the DSP at once from the sequencer.
- **The DSP adds no delay.** Core 0 applies level/pan every block (`P:$033E`, ramped within one 32-sample block).

**Fix:** `lock_dsp_now` (called by `plock_out` for every lock, revert and preview value) writes the value immediately
with the drain's own writer `0x0804070E(track, param, value)`. That ends in the locked DSP write `0x08031228`, which
the note trigger also uses from the sequencer. It covers sound params 6–31 only. The early send is now one tick.

**Report v22** adds `locksDirect`.

## 0.2.10: locks ahead of the transient; locks belong to step content

**Early locks.** On 0.2.9 hardware, a hard-right step pan still clicked on the left, while a hard-right Mixer-view
pan did not. Stock already sends the lanes before the note on the same tick: the player tick `0x0805526E` runs sample
flip, then the lanes, then the note triggers (vt+8). So the mixer change itself lands after the note's start. Now:
- On tick `n − max(1, n/6)` of a step (n = ticks per step for the sync rate: 2, 3, 4, 6, 8, 12, 16, 24), `plock_tick`
  runs the lane logic for the next step, if that step has a trigger. That covers its locks, and its revert when it has
  none.
- The next step wraps from the end to the start (`+0x100` / `+0x101`).
- Tick 0 then repeats it; `plock_send` dedups.

**Content-tied locks:**
- `plock_tick` ignores lane bytes on steps without a trigger.
- `lock_sweep` (in `fm_service`, one track-pattern per call, track base = player `[+0x50]`) erases the six micro-step
  bytes of every lane on steps without a trigger.

So deleting a step deletes its locks; trigless locks are gone.

**Report v21** adds `locksEarly` and `locksSwept`.

## 0.2.9: locks last the whole sound

On 0.2.8 hardware, a kick panned right on step 1 went back to the centre on step 2: the tail of the sound was not
locked.

**`plock_tick` now:**
- A step without a trigger (step byte 2 & 0x3F = 0; pattern base = lane 0 − 0x128) never reverts. The previous
  hit's lock carries on.
- A lock byte on such a step still sends (trigless).
- The next **hit** without a lock reverts to the base.

**Spice** writes locks on hits only (the hold fill is no longer needed).

**Validator:** `locks_hold_over_empty_steps_until_the_next_hit`.

## 0.2.8: step sliders for level, pan, delay and reverb (`steplock.c`)

**When active:** Record on, track view (router view 0), sub-mode byte `+0x264` = 0, and a step held on the current
track. The bottom rows (pads 16–31) then become two sliders out of level / pan / delay / reverb (lanes 10 / 11 / 9 / 8).
Buttons 0x13/0x14 (sample page ±1) scroll the window of two rows.

**Writing a value:** through the stock message `0x08047038(knobRouter[+0x3C], track, lane, value)`, which is the
held step + knob path (the editor stores it on its held step).

**Removing a lock:** the lock's own pad writes `0xFF` to the step's six micro-steps.

**Display:**
- Locked: a bright bar.
- Unlocked: the knob's live value, dim.

**Stock state used:**
- **Record:** the transport object's store `[+0x28]` index 1. Its Record message handler `0x08033CA8` sets it from
  the message value, and the live-record gate `0x08036B06` reads the same store. The object is captured in its
  listener: vtable words `0x08066B40` (direct) and `0x08066B6C` (thunk) → `fm_transport_message`.
- **Current track:** the track view object (`router[+0x3C]`, vt `0x080678DC`) holds it in store `[+0x25C]` index 0
  (`0x0805800E`). Pads ≥ 16 go to the per-track sample sub-view in mode 0 (`0x0804DBDC`).
- **Held step:** the knob router's per-track object `X = *([R+0x34] + t·[R+0x2C])`:
  - store `X[+0x24]` index 0 = a step is held;
  - store `X[+0xC]` index 1 = the step;
  - `*(X[0]+0x18)` = the lanes.

  This is exactly what the knob display `0x08038EF2` reads.

**Hooks (vtable words):**
- router button press `0x080672EC` → `fm_button`;
- track view frame `0x080678F8` → `fm_track_frame`, which overwrites pads 16–31 after stock composes the frame;
- the two transport listener words.

The pad press uses the existing `fm_pad` hook, and pad release is tracked for the pan centre pair.

**Not hardware-confirmed:**
- which physical arrow is 0x13;
- that `X[+0x24]` means "held" in every case;
- the transport store's index 1 = Record (static evidence only; the log's `stepState` shows it).

**Report v20** adds `stepLocks`, `stepLast` and `stepState`.

## 0.2.7: spice (structure-aware random locks)

**Gesture:** Shift + track button (stock discards this). It replays a sound only in the Sample Rec view's generate
mode; anywhere else it runs `spice_track` (`spice.c`) on that track's current pattern.

**Where the data comes from:**
- **Track players** are captured as they tick (`plock_seen` in `plock_tick` and `plock_revert_all`; macro 1's id = 16·track).
- **The pattern** is the player's playhead store index 0.
- **Its base** is the lane address − 0x128.
  - Steps: byte 2 = micro-step trigger mask, byte 3 = velocity.
  - +0x100 end, +0x101 start (stock init 15/0; `0x0804709E` callers).

**Analysis:**
- length; hits, accents and ghosts (velocity quartiles, or beats/offbeats when velocities are flat); rolls;
- the shortest period (4/8/16) with at least 7 of 8 steps agreeing;
- backbone vs fill hits; the gap to the next hit;
- other tracks' hits on the same step.

**Spice:**
- **Palette:** 3–4 of the lanes 4–11 (distortion, HPF, LPF, resonance, reverb send, delay send, level, pan), weighted
  by density. It always includes pan or level, and a tone parameter.
- **Moves by role:**
  - Backbone downbeats are left alone.
  - Ghosts move most.
  - Fills get a crescendo or a riser.
  - Throws go on hits with room after them.
- **Consistency:** decisions are hashed per position in the period, so every repeat gets the same move.
- **Holding:** a lock holds over the steps until the next hit.
- **Never touched:** user locks, and macros 1–4.
- **Re-roll:** a snapshot of lanes 4–11 at micro-step 0, keyed by the lane pointer and a checksum, restores the
  pre-spice lanes before each re-roll.

**Plock changes:**
- `plock_tick` reads every lane's byte, not only lanes in the editor's has-data mask: spice writes lanes without
  the editor.
- Mixer ids (0x80+) go through the knob router's real setter plus the mirror (the 0.1.19 path that hardware proved
  for level/pan). Before this, lanes 10/11 went through the player's setter, which 0.1.17 showed does not move
  level/pan.

**Report v19** adds `spices`, `spiceLast` and `spiceRefusals | players << 8`.

## 0.2.6: expansions with a focus

On 0.2.5 hardware, expansions looked like the same scheme every time (the first 3–4 pads always kicks). By design,
0.2.5 balanced every page to the whole-kit weights and sorted the picks by category, so on a balanced pack every page
came out alike.

**`kit_plan` now:**
- **Page 1** (no tags found) is the balanced starter.
- **An expansion** draws a focus of 3–4 categories, weighted by how far each is below its share of the grown kit
  (deficit + 64; caught-up categories keep a weight of 16).
  - Each focus category gets 2 slots; more up to 11–12 by deficit.
  - The rest are "spice" from non-focus categories at or below their share.
  - Rows are ordered focus-first.
- **Fix:** `kit_category` checks "destroyed" before "tom", so destroyed toms are no longer counted as impacts.

**Host check (8 pages):** every page has a different focus and layout, and the whole kit stays within ~2 of the target.

**Report v18** adds `kitKnown` (tagged sounds found when the last page was planned) and `kitFocus`, so hardware can
confirm the tags are being read.

The emulated store test now uses one short sound (a hi-hat) to keep validation quicker.

## 0.2.5: kits that grow page by page

The user asked for 16-sound kits that can be expanded one page at a time, up to 128 sounds, with each expansion
aware of what was seeded before.

- **Tagged WAVs.** Every stored sound (kit or single) has a 36-byte `fmt ` chunk: PCM fields, cbSize 18, then the tag:

  | Bytes | Content |
  |---|---|
  | 38–41 | `PHK1` |
  | 42 | pad (role, and whether gnarly) |
  | 43 | archetype (0xFF = drawn) |
  | 44 | flavour |
  | 45 | kind |
  | 46 | family |
  | 47 | version |
  | 48–51 | seed |

  `data` follows at 56, and the samples at 64. That is still inside the loader's 64-byte header read
  (docs/RHYTHM-SAMPLE-WRITE-MAP.md §1.5).
- **Tap.** `kit_make` reads the tags of the 112 other slots (synchronous `0x0804603A` reads of 64 bytes through the
  stock scratch, in the main task), then `kit_plan` plans the 16 sounds:
  - sounds fall into 13 categories (kick, gnarly kick, snare, gnarly snare, clap, impact, hat, cymbal, metal, gnarly
    metal, noise, click, destroyed) with weights per 64 (`kit_weight`);
  - each pick goes to the category furthest below its share of the grown kit;
  - within a category, the least used archetype comes first. Kind (metal: clang/metal/anvil; click: zap/glitch),
    impact body and destroyed role also go to the least used;
  - ties go to the kit's random walk;
  - the picks are sorted by category, so rows group.

  `kit_patch` builds each sound from the planned pad, archetype, flavour and kind (`ph_character_a` with a forced
  archetype); its seed is the kit seed ^ slot × golden ratio.
- **Host check.** Growing 8 pages gives exactly the target balance (kick 16, gnarly kick 16, snare 12, gnarly snare 8,
  clap 6, impact 10, hat 12, cymbal 4, metal 10, gnarly metal 6, noise 10, click 10, destroyed 8), with archetypes
  spread evenly. Examples: `build/generator-auditions/kit-example-page-1.wav` and `kit-example-page-2-expansion.wav`.
- **Clear and placement.** Clear covers the page's 16 slots, and the kit starts at 16 × page. The fixed 32-sound
  `ph_kit` is gone (the low flash had only 256 bytes left); the planner lives in extension.c.

## 0.2.4: clean single stores

On 0.2.3 hardware, kits came out clean, but a single sound stored in commit mode carried crackle or stutter.

**Cause:** the store read its 32-sample block from `S->mix`, keeping a position across storage callbacks. Meanwhile the
commit-mode replay audition (`fm_service` → `render_block`) rewrote `S->mix` on every block, so fragments of the audition
were written into the WAV. Kits play no audition, so they were unaffected.

**Fix:** the store and its dry measure render into `S->store_mix`. `store_cases` now runs the audition through `fm_tick`
between every storage step and requires that the replay was actually sounding. The test fails on the 0.2.3 image
("Host and ARM stored WAV differ") and passes on 0.2.4.

## 0.2.3: the kit pad

On hardware, 0.2.2 "works exactly as intended". The user then asked for dim idle LEDs and a kit generator.

- **LEDs.** `fm_samplerec_frame` always draws pads 21–23 in the Sample Rec view. Idle colours: 21 dim purple 0x38,
  22 dim green 0x24, 23 dim red 0x54 (RGB table at 0x20003974). Active colours: 22 green 0x22, 23 red 0x55, 21 purple
  0x37, blinking every ~0.35 s while a kit is made or cleared.
- **Pad 21 is momentary.** `fm_pad` consumes the press and records the press time (DWT cycles). The release arrives
  through router vtable word **0x080672E8** (stock 0x0803D897) → `fm_pad_release`, which then calls stock:
  - a short press calls `kit_make`;
  - a press held 1.5 s (`KIT_HOLD`, checked in the frame callback, which runs in the main task outside the DSP mutex)
    calls `kit_clear` while the pad is still held.
- **`ph_kit(patch, index, seed)`** (poundhard.c) is a fixed 32-entry layout of roles and flavours, by rows: kicks;
  snares, claps and impacts; hats, cymbals and metals; noise, glitches and destroyed. Repeats of a role are spread over
  archetypes (`ph_character_a` with a forced archetype, offset per kit).
- **`kit_make`** stores the sounds one at a time through the same commit path. The slots are `kit_start` + i, where
  `kit_start` = 16 × page, capped at 96. `pending[8]` holds the kit sound, and `commit_closed` chains to the next. There
  is no audition per sound; each one is measured with a dry render and normalised to −1 dBFS.
- **`kit_clear`** deletes the 32 slots one at a time (async delete, then the file-changed event, which frees the DSP
  pages and clears the "has sample" bit).
- Errors stop a kit or a clear, and pad 23 flashes orange. The stall guard covers clears too.
- **Report v17:** adds `kits`, `kitClears` and `kitProgress`.
- An example kit render is at `build/generator-auditions/kit-example-32-sounds.wav`.

## 0.2.2: two-pad mode UI

On 0.2.1 hardware, Shift + track auditioned correctly, but holding a track button and pressing a slot did nothing. The user
asked for a sturdier UI. 0.2.2 follows docs/RHYTHM-SAMPLE-WRITE-MAP.md section 8 (Sample Rec view only):

- **Mode pads.** In `fm_pad`, pad 22 toggles `gen_mode` and pad 23 toggles `commit_mode`. The two are exclusive and the
  presses are consumed. Stock gives pads 21–23 no state (§8.5).
- **Track press.** `fm_track_press` at 0x0803D9A4 swallows the press in either mode:
  - generate mode: a new sound (`requested`);
  - commit mode: replay (`replay`).
- **Shift + track** in generate mode replays the sound.
- **Store.** In commit mode, a slot pad pressed while track t is held (the `router+0xA0` bit, a confirmed plain bitset)
  calls `commit_start`. The pad is consumed **only if the store started**. A store is refused while busy, while the
  stock Sample Rec is recording (`view+0x48` state 2/3), or when t has no sound. A refused pad goes to stock, and pad 23
  flashes orange.
- **Stall guard.** A store with no storage progress for 30,000 services is abandoned (error 4), so it can never block
  later stores. This is the likely 0.2.1 failure: a stuck state that swallowed every later press silently.
- **LEDs.** Vtable word 0x08067A24 (the Sample Rec view's frame method, stock 0x0804FE9B) points at
  `fm_samplerec_frame`. After stock composes the frame, pad 22 is green in generate mode. Pad 23 is red in commit mode,
  yellow while storing and orange for ~0.3 s after a refusal or error.
- **Bit-exact renders.** `gen_strike` seeds poundhard's noise from the sound's seed, so the audition, a replay and the
  stored copy are bit-identical. Before this, the noise state carried over from the previous strike.
- **Report v16:** adds `storeRefusals` and `modes`.

## 0.2.1: storing a generated sound into a sample slot

This follows docs/RHYTHM-SAMPLE-WRITE-MAP.md.

- **Sample Rec view only.** Shift + track (`fm_randomize_request(track, router)`) generates only in the Sample Rec view (router view 4).
- **Waiting sounds.** `Pending[t]` holds each sound's seed, family, measured length and peak. The audition measures it; if the audition was cut short, the commit measures it with a dry render on voice 1.
- **Track press.** At 0x0803D9A4, `fm_track_hook` → `fm_track_press` swallows a track press (it leaves through 0x0803DC94) while that track has a waiting sound in view 4. The stock peek would otherwise send the next pad to the track view.
- **Store gesture.** In `fm_pad`, in view 4 with track bit t held (`router+0xA0`), a pad below 16 means slot = pad + 16·page (`router+0x58` → +0xC). The press is consumed and `commit_start` runs.
- **Commit** (main task only, never in `fm_service`, which runs inside the DSP mutex):
  1. acquire the scratch `0x0802EF6A`;
  2. async delete `0x08046098({4,slot,0xFE})`;
  3. in each callback, re-render 4 KB on voice 1 (bit-exact) and write it with `0x0804607A`: a 44-byte header, int16 at −1 dBFS (gain capped at 8×) and a 256-sample zero tail;
  4. release the scratch, then close with `0x080460C8`;
  5. raise the file-changed event `0x08035D62(*0x20006EAC, {4,slot,curPack})`: bitmap rescan, page free and reload;
  6. play the slot once with `0x080407F0(slot,0,0)`.
- **Report v15:** adds `commits`, `commitErrors` and `lastCommit`.
- **Validated:** emulator cases with stub storage, where the test drives the callbacks. The WAV is byte-identical between the ARM build and the host build, the loader's format is checked, and the reload and audition are confirmed.
- **Hardware check needed:** that the slot reloads at once, the physical pad numbering, commit time, and no button artefacts.

## 0.2.0: a new direction

The user's decision (2026-10-16):
- Keep the stock sampler on all eight tracks.
- Drop the FM engine.
- Use the poundhard engine as a **sound generator**, whose sounds are stored as samples.

| What | Detail |
|---|---|
| Baseline | 0.1.19's sampler path: the stock note hooks, the DSP sampler code (P:$0122/$0234/$02E4), the sampler tables and the loader are all stock again. |
| Removed | The FM engine (fm6 core, Tonitor generator, voices), the live drum tracks, the note/release/stop hooks and the pad-Shift drum logic. The main extension code shrank from 34 KB to 10 KB; about 33 KB of image flash is free. |
| Generator | Shift + track button 1–8 (`fm_shift_button`, now `cmp r5,#8`; stock ignores Shift + tracks) calls `fm_randomize_request(t)`. The service calls `generate(t)`: `ph_family` (poundhard.c) draws a sound of family t (kick, snare, hi-hat/cymbal, metal, noise, glitch/click, impact, gnarly) and strikes voice 0 at 48 kHz (`PH_SR` 48000). |
| Length | Until the sound is 60 dB below its peak for 21 ms, at most 2 s with a 50 ms fade (`GEN_*`). The stored sample will follow the same rule. |
| Audition stream | Blocks of 32 samples go to a 12-slot FIFO (35 words per slot) only while a sound plays. Idle costs one telemetry read every 128 service calls. |
| DSP consumer | 229 words. After the native sampler pass ($0221), it adds the block to track t's active span (`Y:$304F0+64t+32·bank`), saturating, so the audition goes through the track's level/pan and the master FX. |
| ITCM | Sine table 0x100, poundhard voices 0x1000 (two: audition and, later, rendering to a slot), state 0x2000. |
| Parameter locks | Unchanged, except that macros 1–4 are no longer remapped (no synth tracks); Mixer lanes 8–11 play as locks again. |
| Report v14 | 32 fields: generator count, last family/pad/kind and seed, audition peak, render/transfer cycles, FIFO, locks, pads. |

**Next:** store the generated sound into a sample slot (hold the track button and press a pad). The
plan is to write a 48 kHz mono WAV into the slot's file and reload the slot with the stock loader.
The stock functions are being mapped in docs/RHYTHM-SAMPLE-WRITE-MAP.md.

The sections below describe the 0.1.x FM/drum releases and are kept for history.

## 0.1.20: sampler off (stage B)

This follows docs/RHYTHM-SAMPLER-REMOVAL-MAP.md, steps 1–4.

- **ARM:** all 8 tracks are intercepted (`DRUM_TRACKS` 5: tracks 4–8).
  - The drum tracks share `NDRUMV` = 3 poundhard voices. A hit takes the track's
    own voice if it still sounds, else a free voice, else the oldest one
    (`drumSteals`). A stolen voice starts its new strike from silence.
  - `OUT_TRACKS` 8: the FIFO is 8 slots × 259 words (`MAX_AHEAD` 7, 4.7 ms) at
    0x37720; CTRL is 19 words (`CTRL_SAMPLE`+8, `CTRL_NONZERO`/`SNAP`/`READ_SLOT`
    derived from `OUT_TRACKS`).
  - ITCM: fast path ≤ 0x1980, FM voices 0x1980, drum voices 0x1F20, state
    0x3400 (3 KB).
- **DSP core 1:**
  - P:$0234 `rts`: the voice traversal is skipped, after $0221 clears the spans.
  - P:$02E4 `rts`: no mailbox scan or trigger setup; this also stops note-ons
    wiping the consumer's filter state Y:$46..$4D.
  - P:$0122 `nop`: the main loop no longer calls the pitch round-robin $0289.
  - The interpolation (X:$200, 10,240 words) and amp-envelope (Y:$122, 354
    words) records are dropped from the relocated stream: the image is
    30.7 KB smaller.
  - The consumer runs the native FX for all 8 tracks.
- **Loader:** `0x08039C7C` `bl 0x080460A8` becomes `movs r0,#0; nop`, so no
  samples are copied to DSP memory at boot or on pack change. Those bursts also
  held the SPI bus outside the mutex.
- **Report v12** (70 fields): notes7/8, peak7/8, dspSample7/8, the drum 8
  role/kind byte and drumSteals. Drum roles for tracks 4–7 use the old roles
  field, one byte each.
- **Not yet removed:** the recorder (and its idle SPI poll), Sample Rec,
  audition and delete controllers, and the DSP sampler code itself (stage 2).
- **Unverified:** whether the Sample view still selects pads when no sample is
  loaded.

## 0.1.19: level and pan locks, again

On 0.1.18 hardware, level and pan locks still did nothing. The user corrected the order: knob 1 = reverb send,
2 = delay send, 3 = level, 4 = pan. A live turn calls the router setter **and** writes the router's live store
(`0x08038A08`, which notifies the store's listeners). The likely cause is that the mixer re-sends its stored
level/pan and overwrites a lock sent through the setter alone. `plock_out` now does both for every remapped value
(locks, reverts and preview).

## 0.1.18: level and pan locks

On 0.1.17 hardware, locks on macros 1 and 2 (delay and reverb send) worked, but
locks on macros 3 and 4 (level and pan) did nothing. Live turns of all four
macros, and the Mixer view, did work. Live turns go through the knob router's
real setter `[router+8]` (`0x0803898C`, vtable `0x08069688`), while locks used
the player's setter `[player+0x2C]`, a different class with a vt1. Every
remapped macro value (locks, reverts, preview) now goes through `[router+8]`.

Corrected order:

| Macro | Function | Param |
|---|---|---|
| 1 | reverb send | 18 |
| 2 | delay send | 19 |
| 3 | level | 16 |
| 4 | pan | 17 |

Level and pan are read for all 8 tracks every block by DSP core 0 at `P:$033E`.

## 0.1.17: macros 1–4 are the mixer on tracks 1–6

On 0.1.16 hardware, Shift + knob on a held step did nothing: Shift switches the
stock UI into its Shift layer (the Record LED turns green), and knob turns
never reach the held-step path. The FM and drum engines ignore macros 1–4
(sound params 6–9), so `plock_map` now sends ids `16t+k` (t < 6, k < 4) as
`0x80/0x88/0x91/0x99 + t`. Stock setter `0x0803898C` turns those into
per-track params 18/19/16/17: reverb send, delay send, level, pan. The mapping applies to:

- **live turns:** router mode 0, sent remapped and mirrored into the live stores
  under both ids;
- **lock sends**, **reverts** and **previews**, whose bases are read from the
  mixer store.

The Shift remap and the saved controller pointer were removed.

## 0.1.16: parameter locks

`plock.c` makes the stock automation lanes behave as Elektron parameter locks.
Stock structures, addresses and evidence are in
[docs/RHYTHM-AUTOMATION-PLOCKS.md](../../docs/RHYTHM-AUTOMATION-PLOCKS.md).

- **Playback (`0x08055324` replaced):** on tick 0 of a step, every lane whose
  micro-step-0 byte is not `0xFF` is sent through the player's setter, with
  stock's companion flags. The value the knob had is read first from the knob
  router's live mirror `[router+0x24]` (the router is saved after its
  constructor at `0x0802854E`) and kept in the player's own look-ahead array
  `[player+0x48]`. The next step without a lock sends it back. A lane being
  turned by hand (not recording) is left alone, and its value becomes the base.
- **Revert:** on player reset (`0x080552F6`) and transport clear (`0x0805518C`).
- **Step preview (`0x0805CE84`):** micro 0 sends the step's locks, micro 5
  restores the knobs.
- **Shift + macros 1–4 on a held step (router vt0 `0x08038E90`):** become ids
  `0x80/0x88/0x91/0x99 + track` (lanes 8–11, mixer/FX). Shift is read through
  the controller pointer saved by `fm_pad`.
- **Recording:** the interpolation fill (`0x08049544`) is a `nop.w`. The writer
  at `0x0804982C` writes micro-step 0 only and never overwrites a lock with `0xFF`.
- **Cost:** about 1.5 KB of low flash (1.2 KB left), 52 B of state. The unused 48 kHz drum
  mix buffer (128 B) was removed.
- **Validation:** six emulator cases on the patched image with fabricated
  player/router/preview objects: lock, revert, touch override, reset and
  transport revert, Shift remap, preview.
- **Report v11** (317 bytes, 62 fields): `plockLocks`, `plockReverts` |
  mixer edits << 20, and `plockLast` (value, lane, step, router-found bit).
- **Unconfirmed on hardware:** the order of mixer lanes 8–11, and Shift + knob
  in the sequencer view having no stock meaning.

## 0.1.15: drum character and the gnarly row

`ph_character(patch, pad, seed)` runs after `ph_design`. The pad is the sample
slot & 15, or Shift + pads 16–31.

- **Slots 0–7:** the plain kick is left as poundhard draws it. Every other role
  picks one of eight archetypes (`ph_archetype`). Each archetype is a set of
  treatments (`ph_treat`): fold, drive, crush, decimate, comb, four-pole
  ladder, noise ring modulation, chaotic feedback, inharmonic ratios, grain,
  register shift, wave frames, peak EQ, short, long, gate, body, dark and
  bright.
  - 30% of draws add one mild treatment.
  - Draws are retuned within ±7 semitones.
- **Slots 8–15 (gnarly):** a non-plain archetype of the same role, plus one or
  two harsh treatments, retuned within ±12 semitones.
  - The kick's archetype row is used only here.
- **Safety:**
  - Dark and ladder filters keep a per-role cutoff floor (hats 62, claps and
    snares 50).
  - The bit reducer is capped at 64, or 48 under a dark or ladder filter.
  - 4,800 offline renders (300 seeds × 16 slots) contain no NaN and no silent
    drum.
- **Emulated cost:** up to ~8.1k instructions per sounding drum per block
  (0.1.14: ~7.2k).
- **Report v10:** as v9, but each drum byte is role (0–15) | kind << 4.
- Audition renders (16 seeds per slot, 24 kHz): `build/drum-auditions/`.

## 0.1.14: processor load and tempo

0.1.13 on the unit, with one "rust" drum playing on track 6:
- the FM service ran ~655 times/s, down from ~1026 in 0.1.11;
- one service call took up to 544k cycles (5.7 ms);
- the FIFO emptied, with 9357 DSP underruns (~2.6%);
- the user heard crackle, and the tempo at 137 BPM ran far too slow.

The service is called from the stock superloop (`0x08025898` → `0x08029F58` →
our hook at `0x08029F70`), so every cycle it spends is taken from the loop that
also scans buttons, drives the display and runs the sequencer. Changes:

- **Drums at 24 kHz** (`PH_SR`): 16 samples per block, interpolated to 32 in
  `render_block` (odd samples are the midpoint toward the previous one).
  Coefficients follow `PH_SR`. srr steps are scaled to 48 kHz samples, and the
  declick is 0.986 per sample. The comb's lowest pitch is now ~94 Hz.
- **Packed transfer:** the block's length word is `32 | mask << 6`, where mask
  names the sounding tracks. Only those tracks' samples are sent, in order. The
  DSP consumer fills the others with silence and still runs their native FX, so
  filter tails continue.
- **Per-call budget:** after ~0.5 ms (`CALL_BUDGET` 48,000 cycles) a call stops
  rendering unless fewer than 4 blocks are queued.

Emulated cost per block:

| Case | Instructions per block |
|---|---|
| Silent | 538 (was ~5,000) |
| One sounding drum | ~5.4–7.2k (was ~9–13k) |
| Drum strike block | ~19k (once per hit) |

- **Report v9** (302 bytes, 59 fields): v8 plus five fields, all since the
  previous report:
  - `avgRenderCycles`, `avgTransferCycles`, `maxTransferCycles` and
    `avgDrumCycles` (cycles per sounding drum per block);
  - `timerBacklog`: the largest count seen in the stock TIM5/TIM4
    output-compare tick objects (bytes `0x200001DA`/`0x200001E6`, limit 254).
    A count near 254 means the main loop is losing ticks.

- **FM, monophonic:** each FM track owns one voice; a new note retriggers it
  the msfa way (phases and output kept, no click), and only the held key's
  note-off releases it. Shift + track button 1–3 draws a new patch (button 4
  goes back to stock). The overload guard can still fade a releasing voice.
- **Drums on 4–6:** as described below for 0.1.12, now three tracks.
- **Faster drum renderer:** each 16-sample control period is processed stage
  by stage (operators; fold and level; noise/transient band; drive, filter,
  amplitude, crush, ceiling) so each loop keeps its values in registers; the
  three envelopes advance once per period (exact stage machine, decay by the
  multiplier to the 16th power) and ramp linearly inside it; loops are not
  unrolled, to keep the code within the reclaimed flash. Emulated cost per
  sounding drum ~9–13k instructions per block (was ~10–15k). Offline fidelity
  against Vascular's engine unchanged (within ±0.8 dB except rust).
- ITCM: fast path `0x100–0x1958`, FM voices `0x1B60`, drum voices `0x2100`,
  state `0x3600`. The fold-curve table moved to the main flash area.
- **Report v8** (277 bytes, 54 fields): notes1–6, peaks1–6, dspSample1–6,
  seeds and families for tracks 1–3, voices sounding (FM per track, drums as
  bits), drum role/kind per drum track, cycle and stall figures, pad presses.

## Drums (since 0.1.12; tracks 4–6 since 0.1.13)

- **Engine:** `poundhard.c`, a C port of Vascular's poundhard voice
  (`vascular-ios/Sources/CVascularOpcodes/src/PoundhardCore.h`, commit
  b79f71ed): three operators in seven algorithms with feedback, pitch sweep,
  body envelope and wavefolder, second carrier, noise with grain and ring,
  the measured transient, noise band and envelope, all six filters, amp
  envelope, overdrive, sample-rate and bit reduction, soft ceiling, and
  `design` for all eleven kinds, including the clap's burst LFO. Not ported:
  the delay/reverb/chorus sends (the Rhythm's own FX sends apply) and the
  tempo-synced random LFOs (patches use LFO seed 0, as Vascular's "no LFOs").
  Waveforms are every 4th of the 128 frames (plus the last) as 128-point int16
  cycles (`generated/poundhard_tables.h`, from `tools/gen_poundhard_tables.py`);
  each strike interpolates its own carrier and modulator cycle and fold curve
  into fast RAM. Offline, against Vascular's own C++ engine rendering the
  same kind and seed with its sends off: median level difference 0.0 dB, all
  within ±0.8 dB except a few "rust" sounds (up to +4.7 dB, from the 256-sample
  comb, lowest comb pitch ~188 Hz), identical decay lengths. Vascular has no
  license file in its checkout; the port is marked GPL-3.0-only like this
  firmware — change it if you intend otherwise.
- **Choosing a drum:** a drum track's kind is its **selected sample slot**
  (DSP parameter 0, the sample number — the core-1 voice setup indexes the
  sample descriptors with it) **mod 8**: kick, snare, hi-hat, clap, tom, metal
  (clang/metal/anvil), noise (rust), click (zap/glitch) — Vascular's eight drum
  roles. FM note-on drains the track parameters exactly as the stock note
  routine does and reads the slot from the stock cache `0x20006860 + 16×track`.
  So the Sample view's pads, the sequencer and per-step sample changes choose
  drums with no new UI code.
- **Randomizing:** the stock controller pad press `0x0803D7FE` (controller
  vtable `0x080672E4`, slot 0) is hooked: it records whether Shift was held
  (`[controller+0x0C]` store, as the stock button handler reads it) and the pad
  number, then runs the stock handler unchanged. The next drum note on track
  5/6 within ~64 service calls takes a new seed for the pad's role (pads 16–23,
  the third row assuming row-major 0–31, give roles 0–7; other pads use the
  selected slot's role). This relies on the stock Sample view auditioning a
  sample when its pad is pressed; the runtime log records the last pad press
  and Shift state to confirm the numbering on the unit.
- **Behaviour:** one voice per drum track; a new strike restarts it, with the
  previous output faded over ~3 ms. Note-offs are ignored (one-shot); stop
  silences. Note 60 strikes at 55 Hz before the kind's register. Each track has
  its own eight seeds; defaults are a fixed kit. Not saved across power-off.
- **Audio path:** the DSP FIFO now carries six tracks — 11 packed 195-word
  slots at shared `0x37720` (ending `0x37F81`), up to 10 queued (6.7 ms),
  sequences wrapping at 11×2¹⁹ with the DSP's read slot address in
  `CTRL+16`. Blocks go as two DMA chunks (≤170 words, the native TX buffer is
  512 bytes). The consumer runs the native distortion/filters on all six tracks.
- **CPU:** the drum voice runs from flash under the instruction cache, which
  is switched on only for the duration of `fm_tick` (the stock firmware keeps
  it off and has a busy-wait DSP handshake whose timing must not change); it is
  built `-mexecute-only` so constants never become uncached flash data reads,
  and its state, cycles and fold curve live in ITCM. Emulated cost: ~12–15k
  instructions per block per sounding drum, about twice an FM voice. This is
  the main hardware risk; `blockCycles`/`maxBlockCycles` measure it.
- **Memory:** the drum code and tables (~38 KB) occupy the stock core-1 DSP
  download's original flash (`0x080101F0–0x0801A837`), freed because the
  builder already relocates a patched copy of that stream and its pointer
  `0x08031660` is its only reference. ITCM: fast path `0x100–0x1958`, FM pool
  `0x1EB0`, drum voices `0x2800`, state `0x3600`. The main extension now starts
  at `0x0806A580`, right after the stock image.

A Circuit Rhythm 5706-based firmware candidate. No Tracks application or Tracks
storage format is installed.

## Performance (0.1.10, extended in 0.1.11)

- **ITCM fast path.** The per-block service loop (`fm_tick`, mixing, clamping,
  DSP block packing in `fm_write_block`), the voice resampler, the FM6 core's
  per-block/per-sample functions (operator loops, core render, envelope and
  pitch-envelope steps, LFO) and the 1025-point sine table are linked to run at
  ITCM `0x100–0x22C8` (8.6 KB), stored in flash after the code and copied by
  `fm_boot` (DSB/ISB before use). ITCM is zero-wait for both instructions and
  data. Rare work — note events, voice allocation, patch generation, FIFO
  initialization, telemetry refresh, envelope stage changes — stays in flash.
  Calls cross via linker-generated absolute thunks (`movw/movt ip; bx ip`).
  The only vendor-core change is an empty-by-default `FM6_HOT` marker on those
  functions (and `FM6_HOT_DATA` on the sine table). Stock code and the global
  caches are deliberately left untouched, to avoid changing stock timing.
- In the emulator, each additional voice adds ~6,800 instructions per block, all
  executing from ITCM; the remaining flash work is the fixed per-call overhead.
  Real cycle counts are only available from the device (`blockCycles`).
- **6-voice pool** (was 8) at ITCM `0x2480`, sized for the 96 MHz budget.
- **Overload guard:** when the DSP has fewer than 2 blocks queued at a service
  call (the MCU is falling behind), the oldest released voice is retired — or,
  with more than one voice per track and none released, the oldest voice. At
  most one per call; counted as `voiceSheds`. Normally the FIFO stays full and
  the guard never acts.

## Behavior

- **Polyphony:** a shared pool of 6 six-operator voices serves the four FM
  tracks (ITCM `0x2480`, 472 bytes per voice). Each note-on takes its own
  voice, so the previous note keeps sounding into its release instead of being
  cut. The same key on the same track retriggers its voice the msfa way
  (phases kept, no click). With all 6 busy, the oldest *released* voice is
  reused first (preferring the same track), then the oldest held voice on the
  same track, then elsewhere; steals are counted in the report. With all 6
  busy, the oldest is taken. A voice is freed
  as soon as it is silent for good (released, or held at a silent sustain).
- **Note events** (on, off, stop) are queued in a 32-entry ring by the note
  callbacks and applied in order by the audio service, so chords and quick
  off/on pairs between two service calls are all kept. Overflows are counted.
  Note-off releases every held voice of that key on that track.
- **Knobs:** each FM track's audio now runs through the Rhythm's own
  sample-track processing (see "Native distortion and filters").
- Boot defaults: tracks 1, 3, 4 the fixed algorithm-5 patch (track 4 an octave
  lower), track 2 the sustained sine reference.
- The MCU renders Tonitor/Felucca's integer FM6 core at 44.1 kHz, converts to
  48 kHz by the exact 147/160 step, honors the patch transpose and applies a
  ~7 Hz DC blocker. Stock held-note bookkeeping (`0x20006D90/98`) is kept for
  the latest note.
- FM note-on calls the stock parameter drain `0x0803081C(track)` first, exactly
  as the stock note routine does, so the DSP has the track's current knob values.

### Native distortion and filters

The DSP voice stage P:`$0511` processes every sample voice's 32-sample scratch
block X:`$2A02` in this order: distortion P:`$0648` (only when parameter `$A` is
non-zero), high-pass P:`$056E` (parameter `$B`, 0 = open), low-pass P:`$0590`
(parameter `$C`, 127 = open, resonance parameter `$D`), then the sample's amp
envelope. Each voice reads its track's parameter block Y:`$307C8 + 32×track`,
which the ARM's stock parameter writer `0x0804070E` fills from the track knobs.

FM tracks never trigger their native sampler voice, so the consumer reuses
that idle voice record (X/Y `$40 + 16×track`) for filter/distortion state and,
per FM track: copies the FM block into X:`$2A02`, executes exactly the head of
P:`$0511` (distortion, high-pass, low-pass — not the sample amp envelope, since
FM has its own), and writes the result to the track's output span. On the
first block after FIFO initialization the coefficients snap to the knob values
instead of gliding from zero. The parameter-to-filter mapping was established by
running the native routines in the DSP emulator (`$B` high-pass, `$C`
low-pass, `$D` resonance); which front-panel knob writes which parameter was not
re-traced — the FM tracks simply follow whatever the sample tracks' knobs write.

**Distortion caveat:** P:`$0648` shapes through a 512-word table at shared
X:`$30200`, which is filled at runtime (not present in the DSP image). Offline
tests therefore use a stand-in table; on the instrument the real one is used,
as it is for sample tracks. FM's level is lower than typical samples (−23 dBFS
median), so the same distortion setting may sound milder than on a loud sample.

### Patch randomizer

- **Gesture:** hold Shift, press track button 1–4 (hook at `0x0803D956` in the
  stock button handler `0x0803D8B8`; unchanged from 0.1.8). Shift + 5–8 still
  does nothing; other Shift combinations continue to stock.
- **Lanes:** each draw picks, 50/50 from its seed, **dark ambient** (Tonitor's
  AMBIENT path over DRONE, TIDE, ABYSS, HOLLOW, WIND, SPECTRE, BLOOM, CHOIR,
  BOWED, METAL bells, a little STATIC/FRICTION; then long tails and darker
  modulators) or **rhythmic noise** (Tonitor's EXPERIMENTAL path over IMPACT,
  BRUIT, METAL, FRICTION, GLITCH, STATIC, MALLET, ITERATIF, SHARDS, some ABYSS/
  WIND; then instant attacks and short decays — 65% one-shot hits, the rest
  short gated bursts — with extra feedback and sometimes an octave down).
- **No glides:** GLISSE, WARP and RUBBER are never drawn, and every patch is
  finished with a flat pitch envelope and pitch-LFO depth ≤ 6 / sensitivity ≤ 3
  (a slight vibrato at most). Tonitor's own `fm_generate` code is unchanged;
  the selection and shaping are a separate Rhythm layer (`rhythm_generate`).
- **Sweep (2000 seeds, host, firmware code):** 0 illegal bytes, 0 patches with
  pitch movement, 0 silent, 0 clipping. Rhythmic noise: median 90 ms to −40 dB,
  65% under 140 ms (held 1 s; gated bursts last while held). Dark ambient:
  sustains. Median RMS −26 dBFS (ambient) / −38 dBFS (noise, mostly silence
  after the hit); patches are still not loudness-normalized (no `fm_measure`).
- Publication, per-track seeds, held-note restart / released-tail cut and the
  lack of persistence are as in 0.1.8. The family byte in the report now has
  bit 7 set for the rhythmic-noise lane.

### Audio transport

- Sixteen packed FIFO slots, 131-word stride at `0x37710`; each slot is READY,
  sequence, length and 128 samples (32 per FM track, track-major): 131 words.
  The final used word is `0x37F3F`. Up to twelve blocks are queued; the producer fills at most six slots per
  service call and sends sequence/length plus samples in one 390-byte DMA
  transfer (native TX buffer is 512 bytes), then commits READY. The consumer
  advances its read sequence only after copying a complete block.
- The DSP consumer replaces the active-bank spans of tracks 1–4 after the native
  sampler pass (bank spans 64 words apart from `0x304F0`) with the processed FM
  blocks: 431 words at P:`0x0D40–0x0EEE`. It saves and restores every DSP
  register (the native routines it calls use nearly all of them) at X:`$2B00`,
  with loop variables at X:`$2B30–$2B34`. Its own copies use `REP` and counted
  software loops; the native filter/distortion routines use hardware `DO` loops
  at the same nesting depth as in the native voice loop. Tracks 5–8 and the
  opposite bank keep their data. With distortion on one track, one block costs
  about 4,000 DSP cycles in the emulator.

There is no track-engine assignment menu, patch editing or knob mapping yet.
Sample-specific controls on tracks 1–4 have not been remapped into FM controls.

## Build

From the project root, using the existing project environment and reference checkouts:

```
make rhythm-fm
make rhythm-fm-check
```

The builder uses Apple's Clang for Cortex-M7 objects, the project-local Zig linker, and the existing DSP analysis tool. Dependencies are pinned in `tools/requirements.txt`; no system-wide installation is part of the build. The stock input must be `firmware/official/circuitrhythm-firmware-5706.bin` plus its matching SysEx, with the hash checked by the builder.

Outputs are under `build/rhythm-fm/`: `rhythm-fm-0.1.14.bin`, `rhythm-fm-0.1.14.syx`, `extension.elf`, `manifest.json`, `validation.json` and `fm-preview.wav`.

## Buffered transfer

The producer holds the stock DSP SPI mutex at `0x20003688`, waits for prior
traffic and selects native word mode. It uses the stock extended-address
command (`0x080313EE`), then packs sequence, length and 128 signed 24-bit samples
in wire order into the existing 512-byte SRAM TX buffer at `0x200058B8`.
Packing happens after the command because that command also uses the buffer.
The stock raw transfer helper (`0x080315E6`) sends 390 bytes and waits before
the chip is deselected. The producer invalidates the single-word address cache,
unlocks, and publishes READY only after payload completion. ITCM is never a
DMA source. A different TX-buffer pointer causes a counted failure without
committing. DSP control/magic/telemetry is refreshed every 128 service calls;
the hot path reads the FIFO read sequence once per service invocation.

`dspSequence` is now the FIFO read sequence, not elapsed audio frames. It
advances only on an accepted block. `dspHeartbeats` counts DSP audio frames;
`dspUnderruns` counts frames without available data. These counters have
24-bit wrap and are sampled at different instants. Use two status snapshots
to assess underrun rate; counts accumulated during startup are expected.

## Runtime report

After playing notes, use the packaged uploader’s **Check selected device again**,
then **Save log**. The device sends a noncommercial `F0 7D 52 46 4D 07` report
before the standard identity reply (version 7): 55 unsigned 32-bit fields,
five little-endian 7-bit bytes each, 282 bytes. Version 7 appends drumNotes5/6,
drumPeak5/6, dspSample5/6, the drum role and kind last played per track,
drumRandoms, padEvents and lastPad (pad, Shift, service count). Fields: notes1–4, services, ticks,
blocks, nonzeroBlocks, peak1–4, dspMagic, dspSequence, dspStatus, dspFrames,
dspHeartbeats, dspUnderruns, dspDrops, resyncs, nextSequence, busy, dspCopies,
dspSample1–4, bulkErrors, dspNonzeroCopies, randomPatches, seed1–4, families
(one byte per track; bit 7 = rhythmic-noise lane; 255 = boot default),
voices per track (one byte each), voiceSteals, eventOverflows, maxVoices,
blockCycles and maxBlockCycles (Cortex-M7 DWT cycles to render one 32-sample
block, enabled at boot for this diagnostic; a block lasts 0.667 ms, about
64,000 cycles at 96 MHz), voiceSheds (overload-guard fade-outs), minQueued,
maxServiceGap and maxCallCycles. Maxima and minQueued reset at each report. DSP
control words moved: first samples at `CTRL+8..11`, nonzero copies at
`CTRL+12`, coefficient-snap request at `CTRL+13`. The browser tool decodes
versions 1–7. No DSP bus calls are made in the MIDI callback.

## What has passed (offline)

The actual patched image executes in ARM and DSP emulators (hardware endpoints
stubbed; no physical throughput, CPU load or UI scan path measured):

- 41 ARM cases (the fast path executes from the ITCM copy made by `fm_boot`), including: FM dispatch, parameter drain and held-note
  bookkeeping on tracks 1–4 and native sampler commands on 5–8; overlapping
  notes on one track; per-key release with the tail continuing; same-key
  retrigger without a new voice; a chord plus an off/on pair between two
  service calls applied in order; a full pool stealing released/oldest voices
  and keeping the newest notes; stop per track and on all four; the 32-event
  queue bound and drain; the overload guard retiring a released voice when
  the FIFO runs low but not cutting held voices; the Shift + track cases; seeds reproducing patches
  bit-for-bit on the host; no excluded family, pitch envelope or strong pitch
  LFO in drawn patches; host/ARM bit-identical audio; the v4 report.
- Cost: ~9,600 emulated instructions per block with one voice, +~6,800 per
  additional voice, almost all from ITCM. Instruction counts are not cycles;
  `blockCycles`/`maxBlockCycles` measure the real load on the unit.
- 25 DSP consumer cases: every accepted block's four output spans equal, bit
  for bit, an independent reference that runs the native distortion/high-pass/
  low-pass routines directly, with different knob values on each track
  (including distortion with a stand-in table). Missing, partial, stale,
  future, wrapped, malformed and disabled blocks behave as before. Knob
  behaviour: closed low-pass and full high-pass silence FM; neutral knobs pass
  it at 0.97–1.0 of its level; distortion changes it.
- Every DSP register preserved; composition after the actual patched sampler call.
- 12 FIFO jitter cases replay four-track payloads through the actual consumer
  and compare with the native-FX reference run over the accepted blocks in order.
- 9 bulk-transfer, 10 historical scheduling, CRC, request-encoder and uploader tests.

## Hardware status

Version 0.1.1 was rejected with a CRC error after all blocks were sent. Its 432,110-byte image was not word-aligned. The actual reference Tracks bootloader loop rounds the CRC input up to complete words, supplying two zero bytes from its cleared read buffer. Offline execution reproduces header CRC `0xDB4FF31B` versus reference CRC `0xD4EAFE8C` for that exact image. Version 0.1.2 includes final padding in the declared image size and CRC, retains a distinct installation marker `RF0102`, and reports diagnostic identity build **15707**. Regression checks execute the reference loop on the official image, candidate and deliberately unaligned fixtures. This is reference-loader evidence; the Rhythm loader itself has not been dumped. The new revision requires a hardware retest.

The first 0.1 hardware attempt received an updater success report but retained sampler behavior on tracks 1–2. Inspection found that 0.1 preserved the stock installation-version block; the reference Tracks bootloader explicitly skips a matching block. This is a plausible explanation of the Rhythm result, not a readback of its internal flash. Version 0.1.1 adds a unique `RF0101` marker, checks the difference in validation, and changes the actual standard identity reply to custom build 15706. Product ID, official base version and bootloader image remain unchanged. For that historical revision, build 15706 distinguished its application. Current 0.1.14 uses build 15723. A stock identity of 5706 does not confirm installation.

**Offline validated candidate; not installed or tested on the unit.** The image and source are complete enough for a controlled first hardware test, but this is not a claim of verified on-device operation.

The MCU state occupies 1832 bytes at ITCM `0x00003000` the 6-voice pool 2832 bytes at `0x00002480` and the ITCM fast path at `0x00000100–0x000022C8`, inside the
STM32F745/746 16 KB ITCM RAM region. The native heap limit stays `0x2004F760`;
FM initialization does not write native SRAM except its normal call stack.
It checks the implemented TCM size, points exception vectors to application
flash, enables ITCM with barriers, verifies a write/read/restore, and initializes
state. The normal identity reply reports 15713 if memory initialization fails.
The stock USB mass-storage code uses all 4 KB of backup SRAM; that buffer is
not used for FM. Stock reset and the covered constructor paths have no ITCM
allocation, but full workload ownership/timing still requires hardware validation.

Sources: [ST AN4667 memory map](https://www.st.com.cn/resource/en/application_note/an4667-stm32f7-series-system-architecture-and-performance-stmicroelectronics.pdf)
and [Arm Cortex-M7 TRM, ITCM data transfers](https://documentation-service.arm.com/static/5e906d72c8052b1608760d63).

New DSP program code is at P:`0x0D40–0x0EEE` (431 words); register-save scratch is at X:`0x2B00–0x2B34`; control is 14 words at shared `0x37700`; sixteen packed slots start at `0x37710` with a 131-word stride, ending at `0x37F3F`. The builder checks them against initialized stock segments and physical bounds. These fit static map gaps and do not overlap loaded program/data segments. A complete runtime allocation audit and workload measurements remain outstanding; the emulator fixtures do not prove every stock mode leaves those regions untouched.

The target rate follows [Rhythm's documented 48 kHz sample format](https://support.novationmusic.com/hc/en-gb/articles/360021702959-Circuit-Rhythm-Components-Guide); the physical audio clock has not been measured. MCU render/transfer deadlines, this revision's bootloader acceptance and recovery remain unverified. Earlier custom images were accepted, and official recovery succeeded on this unit.

No upload or installation command is part of the build/check targets.

The packaged release includes a standalone browser uploader, official recovery SysEx and `TRANSFER.md` checklist. Start the extracted `Start Transfer Tool.command` or run `python3 serve.py`; this uses the existing Python 3 and Chrome/Edge. Full candidate/recovery stream tests and failure checks run against simulated devices. These tests do not establish physical bootloader acceptance, device timing or recovery. `make transfer-bundle` rebuilds and validates the package without MIDI access.

## Sources and licenses

`vendor/fm6_core.c` is copied unchanged from local Tonitor commit `d3e4e36ec37e4cab151a4ee7fa6aacd419a08f00`. It retains its Apache-2.0 notices and accompanying license. `fm_generator.c` is adapted from Tonitor's GPL-3.0-only `fm.c`/`dsp.h` at commit `209a1d3ed9a827dd32568312c455184c309c87a0` (its header lists exactly what was taken and what was changed); the generator only uses the pinned core's patch layout and `fm6_carriers`, which are unchanged between the two commits. The new extension and build integration use GPL-3.0-only; see `LICENSE`. Tables are frozen from the established Tonitor reference generator. The original Rhythm application remains a separate proprietary build input.

The builder manifest records source hashes, original hook bytes, patch locations and output hashes. It preserves Rhythm's product identifier and all application vector bytes, and writes no bootloader image.
