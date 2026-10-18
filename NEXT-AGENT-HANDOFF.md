# UPDATE 2026-10-18 (newest): 0.6.0 — pattern generator pitch fix, one-sound and pack paths

**0.6.0** (build 15749, marker `SR0600`), not yet tested on hardware:
- **0.5.0 hardware:** the generator wrote note 0 (an octave down). It now never sets the pitch.
- **Paths:** pads 25/26 of the secondary Sample Rec view (track view sub-mode 0xA) choose one sound or the pack (by role, sample flips).
- **Space:** about 0.9 KB of app flash left. Further features need space freed first (for example, lowflash
  candidates or trimming tables).

---

# UPDATE 2026-10-18: 0.5.0 — pattern generator, BAYAREA without kicks

**0.5.0** (build 15748, marker `SR0500`), not yet tested on hardware:
- **Pattern generator:** Record + Shift + track runs `pattern.c`. It writes the stock pattern and reloads the editor
  (`0x08048932`).
- **0.4.0 hardware:** "pretty good"; BAYAREA kicks too weak. Kicks removed, no pitch movement, clunk and clang added.
- **Space:** about 2 KB of app flash left.
- **Unknown:** the stock gate maximum. The generator stays within 1–24 (four steps).

---

# UPDATE 2026-10-18: 0.4.0 — second engine, BAYAREA

**0.4.0** (build 15747, marker `SR0400`, `dist/steamroller-0.4.0`), not yet tested on hardware:
- **Engine:** Tonitor's BAYAREA ported at 48 kHz (`firmware/rhythm_fm/bayarea.c`, tables from `tools/gen_bayarea_tables.py`).
- **Selection:** pads 16/17 in Sample Rec pick poundhard/BAYAREA while generate or commit mode is on; stock threshold
  pads otherwise (user asked for 16/17; this mode condition was our choice and is open for review).
- **Kits:** pages by either engine are planned against the whole pack's tags (shared categories; see the firmware README).
- **Listening:** `tools/bayarea_host.c` renders every BAYAREA family to WAVs (`build/bayarea-renders`).
- **Space:** non-audio code is `minsize`; about 4.7 KB of app flash left.
- **Note:** `dist/steamroller-0.3.2.zip` was rebuilt locally on 2026-10-18 from the `minsize` build before the bump. The
  published site still carries the original 0.3.2.

---

# UPDATE 2026-10-17: renamed SteamRoller — 0.3.2

The firmware is now called **SteamRoller**:
- **0.3.2** (build 15746, marker `SR0302`, `dist/steamroller-0.3.2`) is 0.3.1 renamed.
- **0.3.1 hardware:** all Grid FX, including the reworked crusher and stock colours: "all perfect".
- **Unchanged names:** internal paths and tool names keep `rhythm_fm`. Older archives stay `rhythm-fm-<version>`.

---

# UPDATE 2026-10-17 (newest): 0.3.1 — crusher presets, stock FX colours

**0.3.1** (build 15745):
- **0.3.0 hardware:** all Grid FX work except the bit crusher, which was too faint.
- **Crusher:** presets now 40–127 bits + 1–32 hold.
- **Colours:** the FX row colours come from the stock Grid FX view palette.

---

# UPDATE 2026-10-17 (newest): 0.3.0 — per-step Grid FX on a per-track FX bus

**0.3.0** (build 15744):
- **Seven Grid FX rows** in the step sliders (8 presets each), stored in micro-steps 1–5 of lanes 10/11.
- **The step editor** no longer overwrites micro-steps 1–5.
- **`gridfx.c`** drives the stock FX engine params and writers.
- **The DSP core-0 FX bus patch** (56 words in unused vectors) routes only the owner track through the Grid FX;
  newest lock wins.
- **Research map:** `docs/RHYTHM-GRID-FX-MAP.md`.
- **Hardware untested;** the presets are first guesses.

---

# UPDATE 2026-10-17 (newest): 0.2.12 — DSP gain glide

**0.2.12** (build 15743):
- **Cause:** core-0 `P:$2D9` glides track L/R gains at 2^-6 per block (~42 ms), so locks missed transients.
- **Patch:** the literal at core-0 `P:$2DA` changes `0x020000` → `0x400000`, in place in the flat core-0 image at
  `[0x08031658]`.
- **Validator:** a dsptool case on the built image.

---

# UPDATE 2026-10-17 (newest): 0.2.11 — locks written straight to the DSP

**0.2.11** (build 15742):
- **Cause of the leaked transient:** stock params reach the DSP only via the main-loop drain (`0x0803081C`), while
  notes go at once.
- **Fix:** `lock_dsp_now` writes each lock value immediately via `0x0804070E`.
- **Early send:** reduced to one tick.

---

# UPDATE 2026-10-17 (newest): 0.2.10 — early locks, content-tied locks

**0.2.10** (build 15741):
- **Early locks:** the next hit's locks/revert are sent a sixth of a step early, because the transient leaked with
  the old pan.
- **Content-tied locks:** lane bytes on steps without a trigger are ignored, and `lock_sweep` erases them.
- **Report v21.**

---

# UPDATE 2026-10-17 (newest): 0.2.9 — locks hold over the sound's tail

**0.2.9** (build 15740):
- **Locks:** they revert only on the next hit without a lock; empty steps keep the previous hit's lock (`plock_tick`).
- **Spice:** locks on hits only.

---

# UPDATE 2026-10-17 (newest): 0.2.8 — step sliders

**0.2.8** (build 15739): with Record on and a step held in the track view, the bottom rows are level/pan/delay/reverb
sliders (`firmware/rhythm_fm/steplock.c`):
- **Scrolling:** buttons 0x13/0x14 scroll the rows.
- **Hooks:** vtable words for the router button press, the track view frame and the transport listener.
- **Validator:** `step_lock_cases`.
- **Hardware untested**, and the arrow direction is unconfirmed.

---

# UPDATE 2026-10-17 (newest): 0.2.7 — spice

**0.2.7** (build 15738):
- **Spice:** Shift + track (outside generate mode) adds structure-aware random locks to the track's current pattern
  (`firmware/rhythm_fm/spice.c`; re-roll from a snapshot).
- **Mixer lane locks** now go through the router path.
- **`plock_tick`** reads all lanes.
- **Report v19.**
- **Validator:** `spice_cases`. Hardware untested.

---

# UPDATE 2026-10-17 (newest): 0.2.6 — focus-based kit expansions

**0.2.6** (build 15737): expansions draw a focus of 3–4 deficit-weighted categories, rows are ordered focus-first, and the
`kit_category` destroyed/tom order is fixed. Report v18 adds `kitKnown`/`kitFocus`.

---

# UPDATE 2026-10-17 (newest): 0.2.5 — expandable kits

**0.2.5** (build 15736):
- 16-sound kit pages, each planned from the WAV tags of the whole pack (`kit_tags`/`kit_plan`); tags live in a 36-byte fmt chunk.
- Clear = the page.
- Host `--kit` grows a whole 128-sound kit.

---

# UPDATE 2026-10-17 (newest): 0.2.4 — clean single stores

0.2.3 on hardware: kits are perfect, but single stores were crackly. The audition and the store shared `S->mix`.
**0.2.4** (build 15735) gives the store its own buffer `S->store_mix`. The regression test is in `store_cases`.

---

# UPDATE 2026-10-17 (newest): 0.2.3 — kit pad

0.2.2 is confirmed working on hardware.

**0.2.3** (build 15734, `RF0203`):
- Dim idle LEDs.
- Pad 21: a tap makes a kit of 32 sounds (`ph_kit`); a 1.5 s hold clears those 32 slots.
- Pad release is hooked through router vtable word 0x080672E8.
- Report v17.

See firmware/rhythm_fm/README.md.

---

# UPDATE 2026-10-17 (newest): 0.2.2 — two-pad generator UI

- 0.2.1 on hardware: the audition works; the store gesture did nothing.
- **0.2.2** (build 15733, `RF0202`): pad 22 toggles generate mode and pad 23 toggles commit mode (Sample Rec view). Details are in
  firmware/rhythm_fm/README.md and docs/RHYTHM-SAMPLE-WRITE-MAP.md §8.
- The frame vtable word 0x08067A24 is patched.
- Stall guard; the noise is seeded per sound.
- Report v16.

---

# UPDATE 2026-10-16 (newest): 0.2.1 — store generated sounds into sample slots

**0.2.1** (build 15732, `RF0201`): store-to-slot per docs/RHYTHM-SAMPLE-WRITE-MAP.md (see firmware/rhythm_fm/README.md).
- Generation happens only in the Sample Rec view.
- New hook at 0x0803D9A4 (`fm_track_hook`).
- Commit state machine in the main task.
- Report v15.
- Validator `store_cases` with `StoreMachine` (stub storage, callbacks driven by the test).
- Not hardware-tested; 0.2.0 has not been tested either.

---

# UPDATE 2026-10-16 (newest): 0.2.0 — stock sampler + poundhard generator (strategic change)

The user decided:
- keep the stock sampler on all eight tracks and drop the FM engine;
- use poundhard as a sound generator: Shift + track 1–8 generates a sound of 8 industrial families and auditions it;
- next, hold the track button and press a slot to store the sound as a sample (any slot, overwrite; length until decay, ≤ 2 s);
- keep parameter locks.

0.1.20 (sampler off) and 0.1.21 (FM lock fix, never packaged) are dead ends.

**0.2.0** (build 15731, `RF0200`):
- New extension.c (generator), ph_family in poundhard.c, PH_SR 48000.
- Audition-only DSP consumer (adds to the track span).
- Report v14; validator rewritten (generator, consumer, plock cases).
- Research for the store step: proposed session "Map stock sample write, slot reload and Sample Rec UI" →
  docs/RHYTHM-SAMPLE-WRITE-MAP.md.

---

# UPDATE 2026-10-16 (newest): 0.1.20 — sampler off (stage B)

**0.1.19** fixed level/pan locks (untested). **0.1.20** (build 15729, `RF0120`) is stage B of
docs/RHYTHM-SAMPLER-REMOVAL-MAP.md: drums on tracks 4–8 over 3 shared voices; 8-track FIFO (8 slots × 259);
core-1 P:$0234/$02E4 → rts and P:$0122 → nop; X:$200/Y:$122 tables dropped (image −30.7 KB); loader cut at
0x08039C7C. ITCM: voices 0x1980, drums 0x1F20, state 0x3400. Report v12. See firmware/rhythm_fm/README.md.

Next: hardware check of Sample-view pad selection; then D6–D8 (recorder poll, Sample Rec, audition, delete); then
stage 2 (reuse DSP core-1 memory and cycles; raise the extension window into the freed image space).

---

# UPDATE 2026-10-16 (newest): 0.1.19 — level/pan locks via the mixer store

0.1.18 hardware: level/pan locks still fail. Knob order: 1 reverb (p18), 2 delay (p19), 3 level (p16), 4 pan (p17).
**0.1.19** (build 15728, `RF0119`): `plock_out` sends remapped values like a live turn: setter `[router+8]` plus a mirror into the router live store.
The next step is stage B (sampler off, tracks 7–8 become drums); see docs/RHYTHM-SAMPLER-REMOVAL-MAP.md.

---

# UPDATE 2026-10-16 (newest): 0.1.18 — level/pan locks

0.1.17 hardware: locks on macros 1/2 (delay/reverb) work; locks on 3/4 (level/pan) don't; live turns and the Mixer view work.
**0.1.18** (build 15727, `RF0118`): `plock_setter` sends remapped macro values through the knob router's setter `[router+8]` instead of
the player's `[player+0x2C]`. Order: macro 1 delay (p18), 2 reverb (p19), 3 level (p16), 4 pan (p17).
Stage A sampler mapping is running in its own session (docs/RHYTHM-SAMPLER-REMOVAL-MAP.md).

---

# UPDATE 2026-10-16 (newest): 0.1.17 — mixer macros

0.1.16 hardware:
- Locks work with Record engaged.
- Macros 1–4 do nothing on the FM tracks.
- Hold step + Shift + knob registered nothing, and Shift turned the Record LED green: the stock Shift layer swallows knob turns.

The user chose to make macros 1–4 on tracks 1–6 the mixer/FX controls. **0.1.17** (build 15726, `RF0117`):
- `plock_map` in plock.c remaps live turns (`plock_knob` mode 0 replicates stock: setter `[router+8]` plus mirror 0x08038A09 under both ids), lock sends, reverts and previews.
- The setter table at 0x08063708 maps ids to `(0x10+t, param)`: macros → 6..13, 0x80 → 18, 0x88 → 19, 0x91 → 16, 0x99 → 17, 0xA1 → 20.
- The level/pan/reverb/delay order is still unconfirmed.

---

# UPDATE 2026-10-16 (newest): 0.1.16 — parameter locks — read this first

The user asked for p-locks on top of the untested 0.1.15 and confirmed that macros 5–8 = distortion/filters.
**0.1.16** (build **15725**, `RF0116`): firmware/rhythm_fm/plock.c, implementing docs/RHYTHM-AUTOMATION-PLOCKS.md plus the preview fix.

| Address | Hook |
|---|---|
| 0x08055324 | plock_tick |
| 0x080552F6 | plock_reset_hook |
| 0x0805518C | plock_stop_hook |
| 0x0802854E | plock_router_capture |
| 0x08038E90 | plock_knob |
| 0x0805CE84 | plock_preview |
| 0x08049544, 0x0804982C | in-place recording patches |

- State: plock counters, router/controller/preview pointers and preview bases, after `report` (offsets stay host-compatible).
  drum_mix was removed.
- Report v11 (62 fields).
- Validator `plock_cases` uses a `PlockMachine` with fake objects at 0x2004xxxx.
- Low flash ~1.2 KB free; image end ~3.6 KB.

---

# UPDATE 2026-10-16 (newest): 0.1.15 — read this first

The user says 0.1.14's FM drums sound fantastic; no log and no word on crackle or tempo. They want much wider, characterful,
gnarly drum variety (the kick is fine, but they want more snare outcomes too).

**0.1.15** (build **15724**, `RF0115`):
- `ph_character` in poundhard.c: archetype table per role (8 × treatment bitmask), mild/harsh pools, random tuning.
- Gnarly row = sample slot & 15 >= 8; Shift + pads 24..31; drum_seed[3][16]; EV_DRUM role in bits 19..22.
- Report v10: drum byte role|kind<<4.
- Pad rows 3/4 = 16..31 are still an assumption: check lastPadPress.
- Space left: lowflash (0x080101F0..0x0801A837) and the image end (< 0x08080000) are each only a few KB.
  ITCM ~0.1 KB, state ~50 B free.
- The user also asked about Elektron-style parameter locks replacing stock automation. Not started: it needs the stock sequencer's
  automation storage, the UI hold-step path and the project format reverse-engineered first.

---

# UPDATE 2026-10-16 (newest): 0.1.14 — read this first

The 0.1.13 hardware log (one rust drum on track 6, notes6=160):
- services 655/s (0.1.11: ~1026);
- maxCallCycles 544k, maxBlockCycles 69k, blockCycles (last, silent) 5361;
- minQueued 0, underruns 9357 / 364k;
- the user reports crackle and a tempo far too slow at 137 BPM;
- drumRandoms 0, lastPad 14 (Shift + pad not exercised).

The service runs in the stock superloop (0x08025898). The TIM5/TIM4 ISRs (0x08064698/0x080646B4 → 0x0806480C) bump
saturating byte counters at 0x200001DA/0x200001E6 (limit 0xFE, period 10). Whether they pace the sequencer is unproven;
report v9 logs their max backlog. TIM9 only toggles a GPIO. The 0x20000310+0xC "semaphore" is the DSP SPI mutex, not a tick.

**Rhythm FM 0.1.14** (build **15723**, `RF0114`):
- PH_SR 24000; render_block interpolates 16→32.
- Packed transfer: length word = 32|mask<<6; the DSP consumer `lsr`s the mask per track and fills silent tracks with zeros
  (V_MASK X:$2B35, SCRATCH_WORDS 0x36, Jcs 0x0AF0A8).
- CALL_BUDGET 48000 cycles / CALL_MIN_AHEAD 4.
- Report v9 (302 B, 59 fields): avgRenderCycles, avgTransferCycles, maxTransferCycles, avgDrumCycles, timerBacklog.

Emulated cost: silent block 538 instructions; one drum ~5.4–7.2k per block; strike ~19k. The hottest loop is the noise/transient stage
(~120 instr/sample). Next: read the v9 log. If avgDrumCycles ≫ instructions, the code is fetch-bound in flash; then
consider moving ph_period stages to ITCM (needs space) or a fixed-point rewrite of the noise and operator stages.

---

# UPDATE 2026-10-16 (newest): 0.1.13 — read this first

0.1.12 worked but crackled significantly (no log). User's new layout: FM 1–3 (mono, one voice each), poundhard drums 4–6,
samples 7–8; optimize drums. **Rhythm FM 0.1.13** (build **15722**, `RF0113`): NTRACK=3/NVOICE=3 mono (allocate=V[track]),
DRUM_TRACKS=3/DRUM0=3, report v8 (277 B, 54 fields), drum renderer block-wise per 16-sample period with control-rate
envelopes (`ph_env_block`) and no unrolling (fits low flash). Emulated drum cost still ~9–13k instr/block — the open problem.
Next candidates: fixed-point operator/fold path; half-rate render for low kinds; ask user for v8 log (maxBlockCycles etc.).
ITCM: fast 0x100–0x1958, FM 0x1B60, drums 0x2100, state 0x3600. Fold curves moved to main flash.

---

# UPDATE 2026-10-16 (newest): 0.1.12 — poundhard drums on tracks 5–6 — read this first

User asked to port Vascular's poundhard (vascular-ios `Sources/CVascularOpcodes/src/PoundhardCore.h`) to tracks 5–6:
Sample view third-row pads = kick, snare, hat, clap, tom, metal, noise, glitch; Shift + pad randomizes; sequence like samples.
**Rhythm FM 0.1.12** (build **15721**, `RF0112`), not hardware-tested; no 0.1.11 report was received.
- `poundhard.c/.h` port (FPU softfp, `-mexecute-only`, `-ffp-contract=off`); tables from `tools/gen_poundhard_tables.py`.
  Matches Vascular's engine within ~0.8 dB (rust up to +4.7 dB from the 256-sample comb). No sends, LFO seed 0.
- Kind = selected sample slot (param 0, stock cache 0x20006860+16t after drain) & 7. Pad hook 0x0803D7FE (controller vtable
  0x080672E4 slot 0) records Shift; the next drum note within 64 services re-seeds that role (pads 16..23 assumed = row 3).
  Pad numbering and whether Sample view auditions on pad press are NOT hardware-confirmed: report v7 has lastPad/padEvents.
- 6 tracks to DSP: 11 slots × 195 words at 0x37720, seq wrap 11<<19, read-slot CTRL+16, CTRL+8..13 samples, +14 nonzero,
  +15 snap; two DMA chunks ≤170 words. FM pool 5. ITCM: fast 0x100–0x1958, pool 0x1EB0, drums 0x2800, state 0x3600.
- Drum code/tables in reclaimed stock core-1 stream flash 0x080101F0–0x0801A837; main extension starts 0x0806A580.
- I-cache enabled only inside fm_tick (stock busy-wait at 0x08030F48 must keep its timing). Drum ≈ 12–15k emulated instr/block.

---

# UPDATE 2026-10-16 (newest): 0.1.11 — read this first

0.1.10 on hardware: much better; occasional clicks and "sustain then abrupt end". Log: dspUnderruns 531/360018,
voiceSheds 23, ~1026 service calls/s, blockCycles 0 (DWT locked). **Rhythm FM 0.1.11** (build **15720**, `RF0111`):
16 packed FIFO slots at `0x37710` stride 131 (DSP: `(seq&15)*131` via shifts), MAX_AHEAD 12 (8 ms), 6 blocks/call,
guard fades (1-block ramp) below 3 queued, DWT LAR unlock, report v6 (232 B) adds minQueued/maxServiceGap/maxCallCycles
(reset per report). Not hardware-tested. Next: read the user's v6 log — if maxServiceGap ≫ 8 ms the stock task stalls
(consider rendering from a timer/DSP-driven interrupt); if maxCallCycles/blockCycles are high, reduce voice cost.

---

# UPDATE 2026-10-16 (newest): 0.1.10 — read this first

0.1.9 on hardware: distortion/filters on FM **work**; overlapping notes (second note while first held/releasing)
crackle/stutter like a bitcrusher. Diagnosis: stock MCU ~96 MHz (PLL M=12,N=192,P=2; FLASH ACR 3 WS + ART/prefetch),
**no I/D-cache enabled by stock**, extension executed from AXI flash (ART does not serve it) → 2 voices exceed the
0.67 ms block budget → FIFO underruns. **Rhythm FM 0.1.10** (build **15719**, `RF0110`): per-block/per-sample path +
SINE linked to ITCM `0x100–0x22C8` (`.itcm` AT flash, copied in fm_boot; lld absolute thunks for far calls; vendor core
only gained an empty-by-default `FM6_HOT` marker), pool 6 voices at `0x2480`, overload guard sheds oldest released voice
when FIFO < 2 queued (`voiceSheds`, report v5, 217 B). Global caches deliberately NOT enabled (stock timing risk).
Not hardware-tested. Ask the user for a log with `blockCycles`/`maxBlockCycles` (DWT) to size the pool for real.
Page: see latest `serve.py` output in `dist/rhythm-fm-0.1.10/`.

---

# UPDATE 2026-10-16 (latest): 0.1.9 — read this first

User reported **0.1.8 is working**, then asked for: no pitch-glide/siren patches; dark ambient + rhythmic noise focus;
sample-track distortion and filter knobs on FM tracks; and no note cutting off the previous one. All done offline in
**Rhythm FM 0.1.9** (build **15718**, marker `RF0109`), `dist/rhythm-fm-0.1.9/` + `.zip`, page `http://127.0.0.1:60883/uploader/`.
Not hardware-tested. Other knobs on FM tracks are deliberately unassigned: the user will decide their use next.

- Polyphony: shared 8-voice pool at ITCM `0x1000` (472 B each); 32-entry note-event ring; steal released→own-track oldest.
  Worst case (8 voices) ≈ 49k emulated instr/block — main risk; report v4 has `blockCycles`/`maxBlockCycles` (DWT).
- Native FX: DSP consumer runs the head of native voice stage P:`$0511` per FM track on X:`$2A02`: P:`$0648` distortion
  (param `$A`), P:`$056E` HPF (`$B`), P:`$0590` LPF+resonance (`$C`,`$D`); params at Y:`$307C8+32t` (ARM writer
  `0x0804070E`); state in the idle native voice record `$40+16t`. FM note-on calls stock drain `0x0803081C`.
  Distortion's 512-word table X:`$30200` is runtime-filled; offline tests use a fixture. `tools/rhythm_fm_fx_reference.py`
  is the independent reference (consumer outputs match bit-for-bit).
- Generator: `rhythm_generate` (end of fm_generator.c) — 50/50 ambient/noise lanes with own family weights,
  no GLISSE/WARP/RUBBER, flat pitch EG, LPMD≤6/LPMS≤3, percussive shaping for noise lane. Tonitor code untouched.
- Report v4: 212 bytes, 41 fields (see firmware README). CTRL words: samples `+8..11`, nonzero `+12`, snap `+13`.

---

# UPDATE 2026-10-16 (later): 0.1.8 implemented — read this first

The task in §1 is done offline. **Rhythm FM 0.1.8** (build **15717**, marker `RF0108`) is packaged at
`dist/rhythm-fm-0.1.8/` + `.zip`; the local transfer page was opened at `http://127.0.0.1:60798/uploader/`.
No hardware result for 0.1.7 or 0.1.8 has been received. The user still transfers manually.

- Tracks 1–4 FM, 5–8 sampler. Version identities now live in `tools/rhythm_fm_version.py` (scripts import it).
- **Randomizer gesture (user-specified): Shift + track button 1–4.** Stock button handler `0x0803D8B8`
  (r4 ctrl, r5 id); with Shift held, mask `0x07DFFF00` at literal `0x0803E444` discards ids 0–7. Hook: `b.w`
  at `0x0803D956` → `fm_shift_button` (trampolines.S): ids 0–3 → `fm_randomize_request`, exit via `0x0803DC94`;
  others re-load the mask and continue at `0x0803D95A`. Emulated from the hook onward only.
- `firmware/rhythm_fm/fm_generator.c`: Tonitor `fm_generate` (commit 209a1d3e), gen 3, open path, compiled
  `minsize`; no `fm_measure` → ~34 dB loudness spread (2000-patch host sweep: no illegal/silent/clipping).
  Drawn in `fm_tick` between blocks; held note restarts, releasing tail cut. No persistence.
- FIFO: 8 slots × 256-word stride, 131 words/slot, last word `0x37F82`; 390-byte DMA. DSP consumer 297 words
  (REP + counted software loop, borrows/restores B). ITCM state 3424 B. Extension ends `0x080723B8` (7 KB spare).
- Report v3: 202 bytes, 39 fields (v2 fields + FM3/4 + randomPatches + seeds + packed families). Browser parses v1–v3.
- `host-render --generate <seed>` reproduces a track's patch from its reported seed.
- Open: hardware boot/audio/crackle, four-voice CPU load, real UI path into the handler, loudness normalization
  (bounded incremental measure is the obvious follow-up), persistence.

---

# Circuit Rhythm custom firmware — handoff for four FM tracks and patch randomizers

Prepared 2026-10-16, Europe/Riga. Workspace: `/Users/nzimas/development/circuit-rhythm`.

## 1. Latest user request and scope

The user explicitly requested this handoff. The next development task is:

- Add **patch randomizers for the FM synth tracks**, inspired by the local **Tonitor firmware for M-VAVE FM-1**.
- Expand the fixed engine assignment to **tracks 1–4 = FM synth; tracks 5–8 = native sample player**.
- Randomization applies to all four FM tracks, not just the existing tracks 1–2.

This is native **Rhythm-based** custom firmware. The user explicitly corrected an early misunderstanding: do not install or port the Circuit Tracks application wholesale. Tracks/community work is a reverse-engineering reference. The engine is Tonitor's DX7-style six-operator FM implementation. Dynamic assignment of any track to FM or sampling remains a later goal; the next requested split is fixed 4/4.

The current implementation is monophonic per synth track. Four synth tracks does not imply requested polyphony. Randomizer controls, patch persistence, family selection and seed presentation have not been specified. Make reasonable implementation choices, document actual behavior, and ask only for genuinely consequential missing preferences. Do not represent an offline patch-generation script alone as the requested on-device randomizer.

**This handoff turn did not implement the new features or change the firmware.** The latest packaged candidate is still 0.1.7, with two FM tracks.

## 2. Working constraints and user expectations

- macOS laptop; shell is Bash. Use the existing workspace `.venv`, Apple Clang, local Zig linker and DSP interpreter. **Do not install software or dependencies on the laptop.** All prior development used existing tools.
- The user manually transfers firmware through our local browser uploader. **Do not flash automatically.** Starting the local read-only server or opening its page does not itself request MIDI or transfer anything.
- Do not assume the current device mode. It has moved between application and bootloader modes during testing. Last delivered instructions were to test 0.1.7; the user has not reported its result.
- User has repeatedly expressed frustration with prolonged investigation without concrete firmware. Carry the next task through implementation, appropriate validation and a transferable bundle. Explain limits honestly; emulator success is not clean hardware audio.
- Current developer instructions disallow spawning subagents unless the user or applicable instructions explicitly request delegation. This request asks for a handoff note, not delegated development.
- Do not send messages to another chat without explicit authorization. No new chat was created or messaged for this handoff.

## 3. Exactly where hardware testing stands

**Latest reported physical result is from the 0.1.6 test context:** sound is now audible. It has a sharp/plucky attack followed by sustain, but sounds heavily degraded with background crackling. The user clarified that it does sustain while holding the pad. The running build was not explicitly re-reported for that observation, so the release association is inferred from the current transfer test rather than a new identity capture.

**0.1.7 has been built, validated, packaged and opened for transfer, but no hardware result or runtime log has been received.** Do not call 0.1.7 clean, boot-confirmed, or successfully flashed. Do not describe the previous crackling as fixed on hardware.

A recent request for a fresh `FM runtime status` line remains unanswered. The user was instructed to hold a note, click **Check selected device again** using normal-mode ports, and paste the line. No external recording equipment is needed. New counters distinguish ARM rendering from DSP-accepted nonzero blocks. Existing root files `rhythm-transfer-log.txt` and `rhythm-transfer-log (1).txt` are earlier logs, including the old CRC failure; do not mistake them for a new 0.1.7 runtime capture.

### Release history worth retaining

| Revision/test | Reported result / established finding |
| --- | --- |
| Initial 0.1 | Updater reported success, but tracks 1–2 still played samples. Its installation-version block matched stock. The reference Tracks bootloader skips matching versions; this plausibly explained it, without Rhythm flash readback. |
| 0.1.1 | Physical CRC error. Image was 432,110 bytes, not word-aligned. Reference bootloader CRC rounds reads to complete words; later images include final padding in size and CRC. |
| 0.1.2 | Boot failure; DSP program placed beyond the sampler core's real 4K P memory. |
| 0.1.3 | Relocated DSP code, but still late boot failure near pad 31/32. |
| D1 | DSP-only diagnostic booted. |
| D2 | FM initialization plus lowered native heap limit failed boot. |
| D3 | D2 with the single heap-limit store disabled booted. This isolated heap reduction in that hardware pair; exact failing allocation was not captured. |
| 0.1.4 / build 15712 | ITCM allocation preserved native heap; boot and initialized FM identity confirmed, but FM tracks silent. |
| 0.1.5 / build 15714 | Boot/identity confirmed. Received notes and rendered FM, but DSP severely starved; silence/occasional clicks. |
| 0.1.6 / build 15715 | Switched 64 separate payload writes to one DMA transfer. User reports audible attack/sustain with heavy degradation/crackling. No new runtime snapshot received. |
| **0.1.7 / build 15716** | Eight-slot FIFO, combined header/payload transfer, reduced polling, track-2 sine reference. **Offline validated; hardware result pending.** |

Official Rhythm firmware restoration has worked repeatedly on this unit. A blank project was prepared during prior boot tests. Do not assume recovery of a new revision is proven merely because earlier recovery worked.

### Last captured runtime snapshot (0.1.5, not 0.1.7)

At `2026-10-16T09:27:39.646Z`:

```json
{"notes1":59,"notes2":0,"services":37201,"ticks":37201,"blocks":30396,"nonzeroBlocks":16959,"peak1":1600827,"peak2":0,"dspMagic":4607281,"dspSequence":91262,"dspStatus":1,"dspFrames":32,"dspHeartbeats":92161,"dspUnderruns":90460,"dspDrops":29616,"resyncs":56096,"pending1":0,"pending2":0,"nextSequence":91263,"busy":0}
```

This establishes notes/rendering and DSP progress, with roughly 97% of published blocks dropped. Values are cached at slightly different instants. `notes2=0` means that track was not exercised in this snapshot; it is not proof of an input defect. This snapshot does not establish nonzero data reaching the output. The later audible result is separate evidence.

## 4. Latest deliverables and exact identities

Release directory: `dist/rhythm-fm-0.1.7/`

- `rhythm-fm-0.1.7.syx`: transferable file.
- `rhythm-fm-0.1.7.bin`: decoded image, 434,424 bytes.
- `Start Transfer Tool.command`: macOS launcher. It opens Terminal, runs the local Python server, and asks macOS Launch Services to open Chrome/Edge.
- `serve.py`, `uploader/`, `TRANSFER.md`, `FIRMWARE-NOTES.md`, source snapshot, validation reports, official `recovery/` image and `SHA256SUMS`.
- `fm-preview.wav`: two-second host-rendered preview.
- `fm-sustained-reference.wav`: four-second held middle C; track 1 left, plain track-2 sine right. Host rendering, not a recording of the Rhythm or its mixer/effects.

Archive: `dist/rhythm-fm-0.1.7.zip`

| Item | Value |
| --- | --- |
| Custom ready identity | **15716** |
| Memory initialization failure identity | **15713** |
| Stock application identity | **5706** |
| Observed stock bootloader identity | **4799** |
| Installation marker | `RF0107` |
| BIN SHA-256 | `89cb363ca59e4a94a8cfac46f4ee9582b262a56d219ac59b537f95b46e0c3a02` |
| SysEx SHA-256 | `794749763bb037cb20f27b8818d97189cbd99311e8609d8e7d5caa53ab4666f7` |
| ZIP SHA-256 | `f3d82631b2168966a32cc21c6ab52c31ee6ecc623ec0e5818635cd3ae81d3634` |

Last local transfer page: `http://127.0.0.1:60545/uploader/`, server exec session `31924`. Chrome was opened. Session/port survival is not guaranteed across agents. Relaunch the packaged launcher or run the existing Python on `serve.py --port 0` in the release directory. The server prints its actual URL. Old pages may still be open; ensure the page header and selected candidate say the new revision. The uploader rejects arbitrary/unapproved files by hash.

Preserve immutable ZIPs of 0.1.4, 0.1.5 and 0.1.6: regression probes read their exact bytes and manifests. Do not overwrite these reference archives while repackaging a new version. Older withdrawn archives are under `dist/withdrawn/`.

## 5. Source and build map

Key files:

- `firmware/rhythm_fm/extension.c`: ARM state, patches, note dispatch, rendering/resampling, SPI publication and runtime report.
- `firmware/rhythm_fm/trampolines.S`: displaced native entry instructions / continuation adapters.
- `firmware/rhythm_fm/extension.ld`: appended executable area and fixed native symbols; refuses allocated `.data` or `.bss`.
- `firmware/rhythm_fm/vendor/fm6_core.c`: vendored Tonitor/Felucca integer FM core, Apache-2.0; preserve notices.
- `firmware/rhythm_fm/generated/fm6_reference_tables.h`: frozen tables for the original 44.1 kHz core.
- `tools/build_rhythm_fm.py`: pinned stock image patcher, DSP consumer assembler, identities/markers, manifest/SysEx builder.
- `tools/validate_rhythm_fm.py`: actual linked ARM/DSP integration checks.
- `tools/probe_fm_bulk_transfer.py`: compiled C burst + native SPI helpers, then real DSP receiver handlers.
- `tools/probe_fm_fifo_jitter.py`: current-vs-0.1.6 service-jitter, partial-payload, wrap and payload-order tests.
- `tools/probe_fm_delayed_transfer.py`: preserved historical 0.1.4/0.1.5/0.1.6 scheduling regression; it no longer tests the current FIFO itself.
- `tools/rhythm_fm_host.c`: exact source host renderer, supports `--sustain`.
- `tools/rhythm_uploader/`: browser application, pure protocol module, tests, launcher and server templates.
- `tools/prepare_rhythm_uploader.py`, `tools/validate_rhythm_uploader.py`, `tools/package_rhythm_fm.py`, `tools/probe_update_crc.py`.
- `firmware/rhythm_fm/HARDWARE-STATUS.json`: explicit current release and hardware evidence. Keep accurate; it gates packaging.

From workspace root:

```sh
make transfer-bundle
```

This builds the existing DSP analysis wrapper, compiles Cortex-M7 C/Thumb objects with Apple Clang, links using the existing project-local `ziglang`, builds the DSP hook, validates everything offline and packages the release. No dependency installation or device access occurs. Outputs are in `build/rhythm-fm/`, then `dist/`.

Unicorn JIT execution requires sandbox escalation in this environment. Use `sandbox_permissions: "require_escalated"` for the offline emulator/build command when needed and describe that it does not access hardware. Prior requests were allowed. Local server binding/browser opening also used escalation. Do not interpret this as permission to flash.

Latest complete build log: `build/rhythm-fm/transfer-bundle-0.1.7.log`.

Latest successful coverage: **20 ARM integration cases, 24 DSP consumer cases, 9 ARM burst/DSP receiver cases, 12 FIFO jitter cases, 10 historical delay cases, reference bootloader CRC cases, 9 actual ARM request-encoder cases and 27 uploader tests**. All 66 packaged files and ZIP contents were checksum-verified. These tests stub hardware endpoints and do not measure physical throughput.

Versions are currently hardcoded across the builder, validators, preparation/packaging scripts, CRC probe, bulk/jitter probes, C identity define, UI header, transfer instructions and READMEs. Search before creating the next version. Use a distinct installation marker and observable ready build. Preserve the failure identity unless semantics change intentionally. Update hardware status and manifests, then rebuild. Firmware source hashes include firmware notes/status, so editing those after building requires refreshing the manifest before packaging.

For ZIP construction use `DIST.parent / (DIST.name + '.zip')`; `Path.with_suffix('.zip')` incorrectly truncates dotted release names.

## 6. Hardware integration that must be preserved

### Native application and boot

Pinned input: `firmware/official/circuitrhythm-firmware-5706.bin`, SHA-256
`d5b9ff5dadf2e284f80d69ecb7087486cf0df315a697bed8272af9de0e90df15`.

Application base is `0x08010000`. Builder does not write a bootloader image. Product is `0x63` and vectors remain unchanged. The custom six-byte marker occupies stock zero padding at image offset `0x1E8`; preserve surrounding version/product fields. Container image length must be word-aligned, including padding in declared size and CRC.

Current hooks:

| Native address | Extension |
| --- | --- |
| `0x08025688` | `fm_boot` |
| `0x08040778` | `fm_note` |
| `0x0804083C` | `fm_release` |
| `0x08040814` | `fm_stop` |
| `0x08029F70` | `fm_service` |
| `0x08026F32` | `fm_identity_send` (BL call-site replacement) |

Extension starts at `0x0806B000`; linker end must be below `0x08074000` so the relocated DSP stream still fits. Entire application image must fit `0x70000` bytes from base. Do not move hooks without checking displaced instruction widths/native continuations.

### ARM state and memory

FM state starts at **ITCM `0x00003000`**, below `0x00004000`. Current host `sizeof(Voice)=620`, `sizeof(State)=1748`; current ARM first-block audio is bit-identical to host. Recheck actual target layout after changes.

- ITCM region is enabled only after checking implemented size in `SCB_ITCMCR` at `0xE000EF90`.
- Vector table is explicitly pinned to application flash with VTOR at `0xE000ED08` before enabling ITCM; barriers and write/read/restore probe protect initialization.
- **Keep native heap limit `0x2004F760` untouched.** Lowering it caused the isolated boot failure.
- **Do not use backup SRAM `0x40024000..0x40024FFF`: stock USB mass storage uses the full 4 KB buffer.**
- Current compile assertions cap state at 4096 bytes and ITCM end. Do not remove them to force a build through.
- No new application persistence/project format has been implemented.

### Audio and FIFO (current two-track design)

ARM renders 32 source samples using integer FM6 at 44.1 kHz and rationally resamples to the 48 kHz target using step `147/160`. One voice per track, independent note/envelope/LFO/patch/resampler state. Current note dispatch intercepts `owner & 7 < 2`; other tracks use native sampler trampolines. Held-note globals at `0x20006D90` and `0x20006D98` are maintained. Matching note-off releases the envelope; hard stop silences the voice.

Track 1 has a fixed algorithm-5 FM patch. Track 2 currently has algorithm 32 with one audible carrier, no feedback and a held envelope for diagnostics. It is still generated by the FM engine. This diagnostic default need not remain the final randomizer experience.

FIFO control is shared DSP `0x37700`. Magic is `0x464D31`. Eight slots are at `0x37800 + 128*i`, `i=0..7`. Each current slot is READY, sequence, length 32, then 64 signed 24-bit samples (32 per track). Final occupied word is `0x37BC2`.

Producer fills at most four blocks per service call. It reads expected/read sequence once per call and applies eight-slot backpressure. It sends sequence/length plus samples together, then writes READY last. Consumer advances the read sequence **only after copying a valid complete block**. On underrun it clears active FM spans, increments misses, and retains the read sequence. Completed audio does not expire merely because wall-clock frames passed. The two active track spans are replaced after the native sampler pass; other tracks and inactive bank remain intact.

### SPI staging and native helper contract

The native SPI DMA cannot use ITCM as its source. The C code copies CPU-readable ITCM samples into the **existing native SRAM TX buffer** while holding the stock SPI mutex.

- SPI object: `0x20000318`.
- Mutex: `0x20003688`; acquire `0x08029C8D(lock)`.
- Release `0x08030EB9(&lock)` takes **pointer to the local lock pointer**, not lock directly.
- Wait `0x08046415()`; word mode `0x08046535()`.
- Set extended DSP write address: `0x080313EF(core=0, address)`.
- Get TX buffer: `0x0802B685(object)`; current pinned-stock buffer is `0x200058B8`, 512 bytes. RX buffer starts `0x20005AB8`.
- **Address setup uses the first three TX bytes. Pack after the address command finishes.**
- Raw transfer: `0x080315E7(core=0, tx, byte_count)`. Native helpers wait for completion before chip deselection.
- Invalidate cached single-word address at `0x20003684` to `0xFFFF` after raw transfer.
- Unlock, then commit READY via native single-word writer `0x08031229(core, word<<8, address)`.
- Read native DSP word: `0x080314D7(0, address) >> 8`.

Current `fm_write_block` accepts at most 66 words and sends **198 bytes**, including sequence/length. It validates the native staging pointer and counts failures without committing. Respect bounds and completion/ordering in any expansion.

### DSP code and physical limits

Stock core-1 configuration gives **P=4K, X=12K, Y=8K words**; shared SRAM is `0x30000..0x37FFF`. Prior placement beyond P `0xFFF` caused boot failure. `tools/rhythm_dsp_layout.py` validates the pinned boot configuration and physical segments.

Current consumer is **455 words at P `0x0D40..0x0F06`**, register-save scratch at X `0x2B00`. The native patched call pair is P `0x11D/0x11E`; wrapper executes displaced sampler pass P `0x221` and continues at native P `0x8A6`. Borrowed SR/accumulator/address registers are preserved. Tests install preparation/snapshot stubs at P `0xF40/0xF80`, so additional consumer code can collide with tests before reaching the physical end.

Builder checks staging slots against initialized stock DSP maps. Static gaps are not a full audit of runtime ownership in all stock modes. Do not claim that audit is complete.

## 7. Important four-track expansion constraints

This is not a change of only `track < 2` to `track < 4`.

1. **Parameterize track count throughout C.** `pending`, `held`, `voice`, note/peak counters, initialization/dispatch/release/stop, render offsets and telemetry currently assume two. Current buffer is `render[66]`: 2 header words + 64 samples. Four tracks need **130 words**: 2 header + 128 samples. Update bounds, reports and every test fixture that assumes two.
2. **SPI bytes still fit.** Four tracks produce `130*3 = 390` bytes per header/payload transfer, within the native 512-byte TX buffer. Actual CPU/render load and wire occupancy increase substantially; measure hardware counters instead of assuming sufficient real-time capacity.
3. **Slot stride must grow.** A four-track slot needs **131 DSP words including READY**, which does not fit the current 128-word stride. Eight slots at **256-word stride** would span shared `0x37800..0x37FFF`; final used word would be `0x37F82`. This is a possible layout, not an implemented or fully runtime-audited allocation. Change both ARM index shift and DSP index arithmetic, builder metadata/range checks, probes and tests together. Check every alias/map and exact bounds.
4. **ITCM budget is tight but plausible.** Adding two current `Voice` objects and 64 extra render words to 1748 bytes gives an approximate **3244-byte** base state, before extra counters, seeds, patch buffers and generator state. Four additional 155-byte inactive patch buffers would add about 620 bytes and leave little room under 4 KB. Verify target sizeof/stack use rather than trusting this estimate. Do not reduce the native heap again.
5. **DSP program headroom matters.** Only 704 words are available from P `0xD40` through `0xFFF`; current code uses 455. Naively doubling the unrolled clear/copy/OR loops is likely to exceed this area. Compact loops or otherwise redesign and validate register/loop-context preservation. If using DSP hardware loops, audit native caller/loop-stack semantics and emulator behavior. Moving beyond 4K is not an option. Relocate test stubs if necessary without masking real overlaps.
6. **Preserve sample tracks 5–8.** Current tests assert native sampler routing for tracks 3–8; adapt them to 5–8 and verify all four FM dispatches, releases/stops, DSP track/bank spans and untouched sampler commands. Native per-track bank spacing is 64 words, with 32-word active bank offset at shared base `0x304F0`.
7. **Telemetry must grow compatibly.** Add counters for FM3/FM4 with a new report version or explicit compatible extension, and update parser/tests/UI. Keep diagnostics in the MIDI callback free of DSP bus operations.
8. **Keep meaningful jitter/ordering tests.** They must prove four-track payload order, no slot overlap, full/empty backpressure, wrap, partial publication, all sampler spans preserved, and contiguous consumed FIFO sequences. Existing timing fixtures are not throughput measurements.

## 8. Tonitor randomizer references — freshly located during handoff

Source checkout: **`/Users/nzimas/development/mvave-fm1`**. Read it; do not modify or run builds that write into that unrelated checkout.

The original vendored core came from inspected commit **`d3e4e36ec37e4cab151a4ee7fa6aacd419a08f00`**. **The local checkout has since advanced:** current HEAD observed during this handoff is **`209a1d3ed9a827dd32568312c455184c309c87a0`**. Do not assume current Tonitor equals the old reference. Current local core and vendored core have different hashes.

Fresh source SHA-256 values:

| Source | SHA-256 |
| --- | --- |
| Current Tonitor `firmware/src/fm.c` | `e06c53d6f4503735cedf4bf45e1ad860991b6ceffcee313f9ced94e1924f431e` |
| Current Tonitor `firmware/src/fm6_core.c` | `3af09b05a7614905a8798b347ab531eba35e508c0e9a01fd23ef01e22a99b308` |
| Current Tonitor `firmware/src/dsp.h` | `e4a23cc82467a56b9b1cb412db15b8f770ca17a4f4ee1696af3fe1c7ab0e5514` |
| Current Tonitor `firmware/src/layers.c` | `02630f314e689db9f0f8e3cf4482a47444364af9b6dd001c6780b391cfbe9d7e` |
| Rhythm vendored `firmware/rhythm_fm/vendor/fm6_core.c` | `9ebbd96eed5b72d79957a3855d68e2a5aac7bf91a22240db7a4b58cde04b45bd` |

Concrete entry points (line numbers refer to current local Tonitor and may move):

- `firmware/src/fm.c:50`: family enum/weight tables. Current source has **22 families** through RUBBER, with historical generation subsets and path-specific weights. The old investigation's nine-family description is historical, not the current generator's complete scope.
- `fm.c:75`: `fm_patch_t`: 155-byte voice, family/path/generation, Q12 gain, deterministic seed.
- `fm.c:96` onward: sanitizer/defaults and operator helpers. Preserve legal parameter ranges and carrier audibility.
- `fm.c:245`: `fm_render`; applies DC removal and measured gain, optional macros/output treatment around the core. Rhythm currently uses the core directly, not all this sound layer. Randomized patches can expose DC/level problems that fixed diagnostic patches did not.
- `fm.c:315`: `fm_gen_cur` generator-generation selector; global scratch means generation is not automatically reentrant.
- `fm.c:1068`: shaping; `fm.c:1087`: mutations.
- **`fm.c:1163`: `fm_generate(pt, family, seed, path, gen)`**. Uses seeded RNG, path/family selection, family-specific synthesis design, shaping/mutation, audible-carrier fallback, names and sanitization. Saves seed/path/gen for reconstruction. This is the main adaptation reference.
- **`fm.c:1255`: `fm_measure(pt)`**. Renders up to 3445 blocks (~2.5 seconds of source audio) to estimate loudness/brightness and choose gain. The implementation uses static voice state and main-loop assumptions, plus FM-1 watchdog feeds under non-HOST builds.
- **`firmware/src/layers.c:1473`: `new_fm`**. Generates up to four candidates, measures brightness/loudness, prefers a suitable/different family and publishes an inactive patch buffer using `fm_pub`/`fm_use` and a publication barrier. `fm_misfit` is immediately above it. `new_kind` selects same-family vs new-family behavior.
- `layers.c:1535`: `sound_from_seed` reconstructs/publishes patches from seed/path/generation/gain. `sessions.c` also records/restores seeds and generator metadata, but those persistence layouts are FM-1-specific.
- **`firmware/src/dsp.h:200`**: `rnd_t`, xorshift RNG, seeded initialization, weighted selection and bounded random helpers. `dsp.h:31` defines OPEN/AMBIENT/EXPERIMENTAL paths; current `GEN_NOW` is **3**.
- `firmware/src/ui.c:1838` and surrounding controls show generation entry points. Audit actual UI gestures before borrowing concepts; FM-1 controls do not map automatically to Rhythm.
- `LICENSING.md`: core Apache-2.0 versus surrounding firmware/generator GPL-3.0-only; preserve required notices/source distinctions. New Rhythm integration is already GPL-3.0-only.

### Suggested adaptation boundaries

- Port a scoped, deterministic patch generator compatible with the pinned core, or deliberately update the core with regression evidence. Do not silently combine incompatible revisions or import the entire FM-1 application.
- Give each FM track its own seed/patch identity and request state. A new random patch should actually affect that track's next/active voice in a defined way, independently of other tracks.
- Build into an inactive patch, sanitize, then publish at a safe renderer boundary. Avoid mutating parameters that an active voice/render is reading halfway through its block. Decide and document retrigger/release behavior to avoid stuck notes and avoidable transients.
- **Do not call the complete blocking `fm_measure` from the audio service, MIDI callback, button ISR, or while holding the SPI mutex.** Its multi-thousand-block analysis (possibly four candidates) can starve a 5.33 ms FIFO and violate watchdog/UI assumptions. Consider bounded incremental preparation, a cheaper bounded normalization strategy, or host-prepared reference data, then validate real-time behavior. Keep gain/DC treatment consistent with Rhythm's signed 24-bit pipeline rather than copying FM-1 Q15 arithmetic blindly.
- Expose a practical on-device randomize gesture for the selected FM track. Choose/document a gesture after auditing native controls; do not invent an unsupported button hook address. A browser control can assist testing but is not a substitute for the firmware interaction unless the user chooses that workflow.
- Persistence is not implemented. Do not claim random patches survive power cycles/project changes until an actual safe format/restore path exists. Seed/path/gen/gain can reduce storage, but changing the native project or SD layout requires design and validation.

## 9. Runtime report and transfer workflow

Current report: noncommercial SysEx header `F0 7D 52 46 4D 02`, then 25 unsigned 32-bit fields encoded as five little-endian seven-bit groups each, then F7: **132 bytes total**.

Fields in order:

```text
notes1 notes2 services ticks blocks nonzeroBlocks peak1 peak2
dspMagic dspSequence dspStatus dspFrames dspHeartbeats dspUnderruns
dspDrops resyncs pending1 pending2 nextSequence busy
dspCopies dspSample1 dspSample2 bulkErrors dspNonzeroCopies
```

`dspSequence` in 0.1.7 is the consumed FIFO sequence, not elapsed audio frames. `dspHeartbeats` counts audio frames, including misses. `dspStatus=1` accepts native 32-sample frames; 2 means unsupported size. `dspNonzeroCopies` counts actual nonzero consumer blocks. Counter snapshots are cached every 128 service calls and are not atomic across DSP reads. Interpret deltas from two captures rather than raw startup totals.

Identity callback first sends this report, then standard identity. The browser closes its inquiry listener/ports on standard identity, so order matters. Report storage has permanent ITCM lifetime; native `0x080298B8` copies it into native SRAM before transport. No DSP calls are made from identity handling. Browser supports report versions 1 and 2.

User's manual transfer procedure:

1. Save/back up work; close Components, DAWs and other MIDI apps.
2. Launch the correct release's `Start Transfer Tool.command`; keep Terminal running. Chrome/Edge opens the localhost page. The terminal URL is the fallback if a tab does not open.
3. Select the correct packaged firmware, click **Load packaged file**, wait for **Verified**. Merely selecting the dropdown does not load it.
4. Power off Rhythm; hold **Sample Rec + Samples + Note** while powering on to enter bootloader. USB directly connected, stable power.
5. **Allow MIDI and check device**, grant SysEx permission, select bootloader ports. Check bootloader identity.
6. Acknowledge and explicitly start/confirm **Transfer firmware…**. Device requests each block; completion requires verification reply, not merely 100% sent.
7. Keep power/USB connected during restart. Select application-mode ports and check identity for the new build.
8. Test synth tracks, sample tracks, note release/stop, then retrieve runtime status. Save log if behavior fails.
9. Recovery: bootloader again, select included **Official Rhythm 5706 — recovery**, load and transfer; Components' Reinstall Firmware is another established path.

The device is pull-driven. Stop does not undo data already sent. Sender does not invent rollback or auto-retry.

## 10. Recommended pickup order

1. Read this note, `firmware/rhythm_fm/extension.c`, builder consumer, hardware status and latest validation reports. Verify files still match the recorded release; the shared workspace may have advanced.
2. Incorporate any new 0.1.7 physical result or runtime line. The stream is not yet known clean; do not conceal this while adding features.
3. Read current Tonitor generator/RNG/publication/measurement code at the pinned paths. Decide a scoped adaptation and safe generation/publication workflow.
4. Parameterize four tracks and choose a nonoverlapping FIFO format/bounded DSP implementation before adding generator state. Prove memory, flash, slot and program limits explicitly.
5. Implement per-track randomization and its actual user control. Verify deterministic seed reconstruction, legal parameters, non-silent bounded output, track independence, note/release behavior and no audio starvation during generation.
6. Update the uploader/report protocol, four-track probes, source/transfer notes, identities and hardware status. Preserve reference ZIPs. Build a distinct candidate with `make transfer-bundle`.
7. Package complete tools/recovery/source/reports, verify hashes and open the correct new local page. Tell the user precisely what is ready and what remains a hardware test. Leave transfer to the user.

Further evidence and historical reports: `docs/RHYTHM-FM-AUDIO-FAILURE.md`, `docs/RHYTHM-FM-LATE-BOOT.md`, `docs/RHYTHM-FM-BOOT-FAILURE.md`, `docs/TONITOR-FM-PORT.md`, `docs/DSP-BUFFER-HANDOFF.md`, `docs/RESEARCH-INDEX.md` and the JSON probes they reference. Older research documents describe the state when written; prefer current source/manifests and explicitly dated hardware evidence.
