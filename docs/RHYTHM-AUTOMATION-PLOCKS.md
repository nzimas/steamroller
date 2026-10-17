# Stock automation and Elektron-style parameter locks: research notes

Research 2026-10-16; **implemented in Rhythm FM 0.1.16** (`firmware/rhythm_fm/plock.c`), with the step-preview fix (§5.5) included. **0.1.17 change:** the Shift + macro remap (P5) never ran on hardware, because Shift switches the stock UI into its Shift layer; macros 1–4 on tracks 1–6 are remapped to the mixer ids at output instead (see firmware/rhythm_fm/README.md). Firmware: stock Circuit Rhythm 5706
(`firmware/official/circuitrhythm-firmware-5706.bin`, base `0x08010000`). Disassembly:
`research/analysis/circuitrhythm-firmware-5706.asm`.

The Circuit Tracks hack notes (`research/CIRCUITS-HACKS-RC/docs/NOTAS.md`) gave the starting signatures. Tracks
4486 equivalents: gate `0x0802E62C`, lane marker `0x0803FC20`, step writer `0x0803FE24`, table `0x08059080`. Rhythm
uses the same code base, so the matching byte patterns were located in 5706. Every address below is from 5706.

**Verdict: feasible, and cheap.** The stock lane format already holds one value per step, with a "no value" sentinel.
The stock "hold a step + turn a knob" path already writes exactly one value per step. Only playback semantics need
replacing (hold → revert to base), plus two small in-place patches so live recording quantizes to steps. No new
RAM is needed for lock data, because locks stay in the stock pattern lanes and save with the project. The base
values live in an existing, already-allocated heap array. Estimated cost: **≈0.6–0.8 KB of extension flash** and
**≈4–8 B of ITCM state**. Neither the native heap limit `0x2004F760` nor backup SRAM is touched.

Per-step **pan and FX sends** are already stock lanes (8–11). They lack only a UI entry, because Mixer/FX views have
no step pads. A Shift + macro remap adds that for ≈60 B.

---

## 1. Where automation lives

### 1.1 Project/session object (RAM, heap)

- Allocated at boot by `0x08027D86` → `operator new` `0x08025278`, constructor `0x08032D94` → `0x08046E50`. Size is
  about `0x29110` bytes (literals `0x0002910C/0x0002910D` at `0x08046EA8`). **It is heap memory**, so its address is
  only known at run time. Neither RAM dump contains it (no `0xFF` runs ≥ 1000 B in `rhythm-startup-ram.bin`).
- Patterns: `session + 0x2E4`, **8 tracks × 8 patterns × 0xA28 (2600) B**. The track stride is `0x5140`; the
  constructor `0x08046E70` initializes `0x40` items of `0xA28` bytes. The accessors `0x0804709E/0x080470CC/0x080470E8`
  compute `session + 0x5140·track + 0xA28·pattern + 0x3E4…` (= pattern settings).
- The stock save/load writes this object, so anything written into lanes is persisted in the project.

### 1.2 Pattern layout (`0xA28` bytes), initialized by `0x08032F3C`

| Offset | Size | Contents |
| --- | --- | --- |
| `0x000` | 32 × 8 | steps; init per step: `00 06 00 00 07 FF 00 00` |
| `0x100` | 0x28 | pattern settings: `+0x100` u16 (from a literal), `+0x102` **sync rate** (init 3), `+0x103` = 0, the rest zero |
| `0x128` | 12 × 192 | **automation lanes**: lane `L` at `0x128 + 192·L`; byte `step·6 + micro` |

Step bytes (accessors are `0x080549B0…0x08054AB0` on the editor's step stores; meanings marked * are inferred):
- `[2]` = micro-step trigger mask (6 bits). Tested bit-by-bit against the tick at `0x0804AF22` and `0x0804B08E`.
- `[4]` = probability (init 7; compared with `counter & 7` at `0x0804AF1A`).
- **`[5]` = sample flip** (per-step sample; `0xFF` = none). It is the 13th "lane".
- `[0]`* = note/slot, `[1]`* = gate length (init 6 = one step), `[3]`* = velocity (0x60 when a step is created by
  sample flip, `0x08047DBA`), `[6]` bit 0 = flag.

**Per-step "has value" flag:** none as a separate bit. Each lane byte is a value 0–127, or **`0xFF` = no value**
(init and clear write `0xFF`: `0x08032F6C`, `0x080478D6`, `0x08047CA8`). A per-lane summary exists, though: a
**12-bit "lane has data" mask** per track for the current pattern. It is computed by `0x08048808`, which scans the
6 micro-steps of each step for non-`0xFF`, and is kept in the editor store `[editor+0x44]` and the player store
`[player+0x30]`.

Resolution: **6 micro-steps per step** at sync rate 3. The table `0x0806651C` (8 rates × 24 B) maps a tick to a micro-step:
rates 0–7 have 2/3/4/6/8/12/16/24 ticks per step, and **tick 0 always maps to micro-step 0**.

### 1.3 RAM objects (all heap; pointers held in the objects)

| Object | Vtable | Key fields |
| --- | --- | --- |
| Recorder gate (1) | `0x08068CE4` (noteOn `0x08036A80`, noteOff `0x08036AC4`, **param `0x08036B06`**); secondary `0x08068CF8` | recorders at `[gate + 0xC + 4·track]`; transport store `[gate+8]` |
| Recorder (per track) | `0x08067864` | `[+0x28]` pending value per lane, `[+0x2C]` touched mask, `[+0x34]` lane count = 12, `[+0x50]` pattern base; vt `+0x1C` = lane address `0x08023696` (`[+0x50] + 0xA28·pat + 0x128 + 192·lane`) |
| Pattern editor (per track) | — (`0x080477xx–0x080488xx`) | `[+4]` pattern base, `[+0x28]` store {pattern, held step}, `[+0x40]` step-held flag, `[+0x44]` lane-has-data mask |
| **Track player** (per track) | **`0x08068D20`** | `[0]` transport `0x08055120`, `+4/+8` note step `0x0804AEEE/0x0804AFBC`, `+0x10` sample flip `0x0804B060`, `+0x14` lane address `0x08022D96`; `[+8]` playhead store {pattern, step, tick}, `[+0x10]` idx1 = sync rate, `[+0x2C]` param setter, `[+0x30]` lane-has-data mask, `[+0x34]` touched mask, `[+0x38]` recording, `[+0x3C]` lane count (12), `[+0x40]` → lane id table, `[+0x48]` u16[12] look-ahead, `[+0x4C]` u16[12] last sent |
| Knob router (1) | `0x08068F34` (vt0 = `0x08038E90`, vt1 = `0x08038EF2`); ctor `0x08038BE0` | `[+8]` real setter, `[+0x24]` **live value stores** (8 track stores × 32 + mixer stores at `+0x20…+0x3C`), `[+0x28]` displayed values, `[+0x38]` step-held flag, `[+0x3C]` message source |
| Step preview player | `0x0805Cxxx` | plays a pressed step (stopped) for 300 ms: sample flip + lanes (`0x0805CE84`) |

Lane id tables (13 u16 per track, stride 28 B): `0x0804ACC4 + 28·track` =
`16t+0..7, 0x80+t, 0x88+t, 0x91+t, 0x99+t, 16t+8`.

## 2. What is automatable

The param id → (recorder = track, lane) table is `0x08065464` (2 B per id, `FF` = not automatable). It is used by the
gate `0x08036B32` and the knob router `0x08038EB2`:

| ids | Lane | Meaning |
| --- | --- | --- |
| `16t+0 … 16t+7` | 0–7 | the track's **8 macro knobs** (track store slots 0–7) |
| `16t+8` | 12 | **sample select** → stored as per-step **sample flip** (step byte 5), not a lane |
| `0x80+t` | 8 | mixer group A |
| `0x88+t` | 9 | mixer group B |
| `0x91+t` | 10 | group C |
| `0x99+t` | 11 | group D |
| `16t+9…15`, `0x90`, `0xA1+`… | — | not automatable |

The id groups come from the setter `0x08038A08`: `<0x80` → track store `[id>>4][id&15]`; `0x80–87`, `0x88–8F`,
`0x90` (flag), `0x91–98`, `0x99–A0`, `0xA1–A8` → separate 8-entry stores; `0xA9–0xC3` = global toggles.

- **Distortion and filters:** the MIDI CC table for track channels (`0x08065EDC`, used by `0x0805F7FE`) maps CC 21–28
  to sound params 6–13. The FM work established DSP params `$A` distortion, `$B` HPF, `$C` LPF, `$D` resonance
  (`docs`/README, writer `0x0804070E`). Lanes 0–7 are therefore very likely macros 1–8 → sound params 6–13, which
  makes **macros 5–8 = distortion/HPF/LPF/resonance**. **Confirmed by the user (2026-10-16): knobs 5–8 drive
  distortion and the filter parameters.**
- **Pan and FX sends:** lanes 8–11 are the only mixer/FX params with lanes. The likely order is level, pan, reverb
  send, delay send for groups A–D, matching the Rhythm manual's automatable set. *The order is inferred and was not
  traced to the mixer/FX views; confirm which knob writes `0x88+t`, `0x91+t` and `0x99+t`.*

## 3. Playback: hold, not revert

The track player's tick `0x0805526E` reads the playhead {pattern, step, tick} (`[+8]`). At tick 0 it calls sample
flip (vt `+0x10`), then **`0x08055324(player, pattern, step, tick)`**. For each lane < 12, that function:

1. If the lane-has-data bit is clear → `last[lane] = 0xFFFF`, nothing is sent (`0x080553CC`).
2. If not recording and the lane is "touched" (the user is turning that knob) → it skips (the knob wins).
3. It reads `lane[step·6 + map[rate][tick]]` (`0x0805543A–0x0805544C`):
   - **`0xFF` → no send** (`0x08055452` → `0x080554CC` loads `lookahead[lane]`, which is `0xFFFF` at rate 3).
   - Otherwise it sends through the setter `[player+0x2C]` vt0(id, value), but only if the value differs from
     `last[lane]`.
   - Faster rates (0–2) use look-ahead `0x08055530`. Slower rates (4–7) interpolate with `0x0805BE00`, which is
     glide only for data that was recorded on in-between micro-steps.
4. Ids `base+1` / `base+2` also send companion flags `base+0xE/+0xF` = `0x7F` or 0 (`0x080554F6–0x0805551C`).

So **steps without data hold the last value**: the parameter stays wherever the previous value left it until the
lane has another value. Live recording smooths the line: `0x08049706` fills the micro-steps between knob events with
averages and thirds (`0x08049780…0x080497D4`). Together these produce the continuous lines the user dislikes.
On transport stop/reset the player clears `look-ahead/last` to `0xFFFF` (`0x0805518C`, `0x080552F6`), and nothing is
restored.

**Sample flip already reverts.** `0x0804B060` applies step byte 5 when it is not `0xFF`. Otherwise it re-sends the
track's base sample from store `[player+0x60]`. The preview player does the same (`0x0805CEC6`). This is the
Elektron model, already in stock code.

## 4. "Hold a step pad and turn a knob" path

1. Knob views call the knob router vt0 **`0x08038E90(router, id, value, mode)`**. Mode 0 means live: it forwards to
   `[router+8]` vt0 and mirrors into `[router+0x24]`. **Mode 1 means a step is held**: it looks up (track, lane) in
   `0x08065464` and posts a message through `0x08047038([router+0x3C], track, lane, value)`. It then updates the
   displayed values `[router+0x28]` through `0x08038A08`.
2. The sequencer dispatcher `0x08036390` (thunk; the editors are at `[this+0x84+4·track]`) routes it:
   - message `[+0x64+0x14]` → **`0x08047D82(editor, lane, value)`**: for lane 12 it writes the sample flip (and
     creates the step if it is empty). For lanes 0–11 it writes **`lane[step·6] = value` and `0xFF` into micro-steps
     1–5** (`0x08047E24–0x08047E6A`), then sets the lane-has-data bit.
   - message `[+0x64+0x10]` → `0x08047CA8` (Clear + knob): with a step held it clears that step's 6 bytes (or its
     sample flip); otherwise it clears the whole lane (192 B).
3. Display: `0x08038EF2` (router vt1). While a step is held it shows the step's first non-`0xFF` micro value for lanes
   0–7 on the knob LEDs; otherwise it mirrors live values.

**So step-hold entry already produces one value per step at micro-step 0.** Only lanes 0–7 (and sample flip) can be
reached this way: there is no step-hold entry in the Mixer/FX views.

Live recording (Record + Play + knob): the gate `0x08036B06` (only when transport store p0 ∧ p1 ∧ ¬p2) calls
`0x0804926A`, which marks the lane touched and stores the pending value. On every tick the step listener
`0x0804946E` calls `0x08049706` (interpolation fill) and then `0x080497EC`, which writes the pending value at
`step·6 + current micro`. After that, pending = `0xFF`, so later ticks write `0xFF` while the lane stays touched.

## 5. Implementation plan for parameter locks

Semantics: a lane byte at `step·6` is the lock; `0xFF` means unlocked; micro-steps 1–5 are ignored and written as
`0xFF`. A locked step sets the parameter at the start of the step. **The next unlocked step returns it to its base**,
which is the knob value. Turning a knob while the pattern plays changes the base (the stock "touched" rule).
Sample flip is unchanged (it already works this way). Locks apply whether or not the step has a note; there is no
"trigless" distinction.

### 5.1 Patches

| # | Where | What | Size |
| --- | --- | --- | --- |
| P1 | `0x08055324` entry (`push.w`, 4 B: `2d e9 f8 4f`) | `b.w plock_tick`: full replacement, same arguments, no displaced instructions | 4 B hook; C ≈ 300–380 B |
| P2 | `0x0805518C` (6 B: `movs r0,#0; add.w r1,r4,#0x3C`) and `0x080552F6` entry | trampoline → `plock_revert_all(player)` (re-send base for locked lanes), then displaced instructions, then continue stock | 2 × (4 B hook + ~10 B trampoline); C ≈ 60 B |
| P3 | `0x08049544` `bl 0x08049706` (`00 f0 df f8`) | `nop.w`: no interpolation fill when recording | in place, 0 B |
| P4 | `0x0804982C–0x08049839` (14 B: `06 eb 46 01 08 eb 41 01 c9 b2 00 f8 01 a0`) | `movs r1,#6; muls r1,r6,r1; cmp.w sl,#0xFF; it ne; strbne.w sl,[r0,r1]`: recording writes micro-step 0 and never overwrites a lock with `0xFF` | in place, exactly 14 B |
| P5 | `0x08038E90` entry (knob router vt0; or vtable slot `0x08068F34`) | if mode 1 ∧ Shift ∧ id = 16t+k with k < 4 → id = `{0x80,0x88,0x91,0x99}[k] + t`: **Shift + macros 1–4 on a held step lock level/pan/FX send** | ≈60 B |
| P6 | router ctor `0x08038BE0` tail, or the first P5 call | save the `router` pointer to ITCM state (needed to read base values) | 4 B state; ≈16 B code |

Shift state: the extension already latches Shift in its pad and button hooks (`fm_shift_button`, `fm_pad`). Reuse
that latch, or read the controller's Shift store (Tracks: param 0 of `[ctrl+0xC]`; not yet confirmed for Rhythm).

### 5.2 `plock_tick(player, pattern, step, tick)` (P1)

```
if tick != 0: return                 // all rates map tick 0 → micro 0; nothing else to do
lanes = vt14(player, pattern, ·)     // lane address, stock vt +0x14 (0x08022D96)
for lane in 0..11:
    st = &player->lookahead[lane]    // [player+0x48] u16[12]; 0xFFFF = unlocked (stock init/reset value)
    if touched(lane) && !recording: { if *st != 0xFFFF: *st = 0xFFFF; continue }   // knob wins, its value becomes the base
    v = lane_has_data(lane) ? lanes[lane][step*6] : 0xFF
    if v != 0xFF:
        if *st == 0xFFFF: *st = read_live(router, id[lane])   // capture base before the first lock
        send(id[lane], v)
    elif *st != 0xFFFF:
        send(id[lane], *st); *st = 0xFFFF                       // revert
send(id, v): if v != last[lane]: setter vt0(id, v); last[lane] = v; companion flags for base+1/+2 as stock
```

`read_live`: `0x0803FC58(router[+0x24][group], index)` → byte, with the same group split as `0x08038A08`. It uses
the stored router pointer (P6).

**RAM:** the base/lock state reuses the player's `[+0x48]` array, which is allocated by the stock constructor
`0x0805504C` (12 × u16 per track). It is only needed for look-ahead at rates 0–2, which locks do not use. New state is
the 4 B router pointer, and optionally 1 B for a reentrancy flag. **No pattern RAM, no heap growth.**

### 5.3 Flash budget

| Item | Estimate |
| --- | --- |
| `plock_tick` + `send` + `read_live` (C, `minsize`) | 300–380 B |
| `plock_revert_all` | 50–70 B |
| P5 Shift remap | 50–70 B |
| P6 pointer capture | 10–20 B |
| trampolines/thunks (`trampolines.S`) | 30–50 B |
| literal pools / alignment | 40–60 B |
| **Total** | **≈0.5–0.65 KB, plan for 0.8 KB** |

This fits either region (≈2.7 KB low flash `0x080101F0…0x0801A837`, ≈3.6 KB at the image end). It is not hot code
(once per step per track), so it needs no ITCM. P3/P4 are in-place edits.

### 5.4 Steps

1. In `tools/build_rhythm_fm.py`, add hooks P1/P2/P5/P6 to the `hooks` list. Add the P3/P4 byte edits through
   `write()`, each with a stock-bytes check (the bytes are listed above). Write the code in a new
   `firmware/rhythm_fm/plock.c`.
2. Emulation: drive `0x08055324` with a fabricated session (pattern base with lanes) and check the sequence of
   setter calls. Expected: lock, revert, lock, and nothing on repeats. Also check `0x0804946E` with a pending value
   (writes only at `step·6`, keeps the lock through later `0xFF` ticks).
3. Report counters (`locksSent`, `reverts`) in the runtime report for the first hardware test.
4. Hardware test: hold step 5, turn macro 7 (LPF) down, release, play. Only step 5 should be dark. Then use
   Shift + macro 2 on a held step (pan), and Clear + knob on a held step.

### 5.5 Risks and open points

- **Project saved while a lock is active** stores the lock value as the knob value. Stock automation has the same
  issue. P2 reverts on stop, so saving while stopped is clean.
- **The step preview player** (`0x0805CE84`, pressing a step while stopped) sends lane values without reverting, so
  the parameter stays locked after previewing. Fix: call `plock_revert_all`-style code from its 300 ms end event
  (event 6 at `0x0805CBE0`). That is about 40 B more. It is optional for a first build.
- **Old projects** with recorded continuous automation would play only the micro-0 samples as locks. This is
  harmless; Clear + knob removes them.
- **Not verified on hardware:** the order of mixer lanes 8–11 (macros 5–8 = distortion/filters is user-confirmed), whether
  `[player+0x2C]` sends pass through the gate, and the exact Shift source. The track player vtable shows notes,
  sample flip and lanes in one class; whether its tick runs before the note trigger within a step was not checked.
  A lock may land one DSP block (~0.7 ms) after the note, which is inaudible for filters.
- The FM/drum tracks read the same per-track DSP params (`Y:$307C8+32t`), so filter/distortion locks should apply to
  them as they do to samples. The drum kind (sample slot & 7) follows sample flip, which already works per step.
