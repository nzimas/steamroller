# Rhythm 5706 sampler removal map (stage A: mapping only)

Date: 2026-10-16. Research only: no firmware source or build was changed, nothing was installed or sent to the device.
Target: stock ARM image `firmware/official/circuitrhythm-firmware-5706.bin` (base 0x08010000, end 0x0806A570) and the
core-1/core-0 DSP images reconstructed by `tools/rhythm_dsp_probe.unpack_stream`.

Confidence tags: **V** = verified in disassembly and/or emulation; **S** = static reading; **U** = unverified/inferred.
Byte counts for ARM code are dead-code closures (span from entry to next entry incl. literal pools, about ±10 %).
DSP cycle counts come from dsp56kEmu (no external-memory wait states, clock not modelled): treat them as lower bounds.

Longer working notes (more evidence, per-method tables) are in the side-session notes folder:
`arm-sampler-features-map.md` and `dsp-sampler-voice-map.md`.

---

## 0. Headline findings

1. **Sample audio is NOT streamed from the ARM at runtime.** Samples are copied once, at boot and on pack change, from
   files on the QSPI flash volume into **DSP external Y memory** (Y:$300000..$D86FFF, 2,695 pages × 4,096 words ≈ 11 M words).
   Core-1 then copies from external memory into per-voice rings by plain CPU moves (no DMA). The ARM's only runtime
   sampler traffic is note/parameter commands plus one idle Sample Rec status poll per recorder tick.
2. **Most of the win is on the DSP**, not the ARM: about **20,000 core-1 cycles per 32-frame block** with 8 sampler voices
   sounding (≈2,490 per voice, +772 with distortion), ≈1,700 when idle; ≈2,655 of 3,124 P words; ≈10.6 k X/Y words.
3. **The biggest MCU flash win is data, not code:** the relocated core-1 download stream (written by `tools/build_rhythm_fm.py`
   at the image end) carries the sampler's interpolation tables X:$200–$29FF (10,240 words) and envelope tables Y:$122–$283
   (354 words) as 3-byte words: **≈31.8 KB** of contiguous flash at the image end once those records are dropped.
4. ARM sampler-only code is only **≈6 KB safely (≤9 KB)**, scattered in pieces of ≤1.4 KB, and ≈1 KB of heap/static RAM.
   The 169,096-byte heap block (allocated at 0x08027D8C) is the project/session image, not sample storage.
5. The DSP SPI link (object 0x20000318) is shared: the loader's bulk writes and the recorder's poll use the same bus as
   our FM FIFO. The loader does **not** hold the 0x20003688 mutex across a 2 KB chunk; only the single-word helpers lock.
6. Memory-mapped QSPI (FMODE=3) is **never used** (the HAL supports it, no command descriptor selects it).

---

## 1. Sample data path at runtime (Q1)

### Load time (ARM → DSP external memory) — V
- `0x08053E24(pack)` loads all slots: returns if `0x08040E28()==1`; resets DSP sample memory (allocator vt+0x10 → mailbox
  Y:$372FB, waits for Y:$37300==0), clears the 128-entry table `0x20000A8C`, then loops **slot 0..127** calling
  `0x08053E8C(slot)` and `0x08027624` (yield).
- `0x08053E8C(slot)` (to ≈0x08054138):
  - reads the WAV header (0x40 B into `0x20006AD8`) via storage read `0x0804603A`, path {type 4, slot, 0xFE};
    parses it with `0x0805B2B4` (RIFF/WAVE/fmt/data at 0x0805B324), data offset `0x0805B2A0`, length `0x080540D4`;
    clamps the length to 0x2F0000 bytes.
  - per **2 KB chunk**: storage read into one half of the double buffer `0x20003F94` (2 × 0x800, the shared 4 KB scratch),
    a 1024-iteration `rev16` loop (`0x08054020`, ≈3–4 k cycles), then SPI stream write `0x08031608`
    (preceded by `0x080313D0` address-page set / `0x0803153C`, finished by `0x08031626`/`0x0803157E`).
  - every 4,096 words it allocates a page: allocator vt+0xC (`0x080643F4`) reads Y:$371D2, writes cmd
    `(slot<<24)|0x100` to Y:$372F8.
  - writes slot descriptor words 0..3 at shared $30000+4·slot via `0x08031228`.
  - prints `"DSP Filesystem reports Sample Memory Exhausted."` (0x08066FB0) on allocation failure.
- Triggers: pack controller (vt 0x0806840C, ctor 0x080398D4) at **boot** and on **pack change**: `0x08039C70` →
  `bl 0x080460A8` at **0x08039C7C** → `0x08053E24`; slot reload after delete: `0x0803A0D8` → `0x080460B8` → `0x08053E8C`.
  Project load alone does not call the loader (U: no other reference found).
- Cost: load-time only. Per 2 KB: QSPI DMA read + ≈3–4 k swap cycles + 2,048 SPI bytes + one page-alloc round trip per 8 KB.
  No steady-state CPU cost. The removal shortens boot and pack switching.
- Bus sharing: `0x08031608`/`0x080313D0`/`0x0803153C`/`0x0803157E`/`0x08031626` all go through the DSP SPI object
  `0x20000318` (`0x08046414` wait). They do not take the mutex (`0x08029C8C` is only called by the single-word helpers
  `0x08031200/0x08031228/0x080314D6/0x08031430` and the note wrappers). With yields between chunks, an FM service
  transfer can interleave between chunks; inside a chunk the bus is held by the loader. **This is a risk for the
  current 0.1.x builds at boot/pack change, and disappears with the loader cut.**

### Playback (DSP-internal) — V/S
- Core-1 P:$073D (forward) / P:$0798 (reverse) translate the play position through the slot page table
  Y:$308C8 + 188·slot (two 12-bit page numbers per word) and copy external Y words into the voice ring
  **Y:$400 + $200·track** (8 × 512 words, m5=$1FF) with `move y:(r1)+,x1 / move x1,y:(r5)+`. No DMA/EMC programming in core-1.
- Correction to the brief: P:$01E5 reads the **sample ring in Y via r5**; **r1** walks the X interpolation coefficient
  tables X:$200..$29FF.
- Refill: priming on trigger ($02FD → $015A), then a top-up of ≥64 words when (read − fill) & $FF ≥ $40 (≈ every other block
  at unity pitch, ≈450 cycles without wait states).

### Runtime ARM work attributable to the sampler
| Item | Where | Cost |
|---|---|---|
| Note dispatch per-slot param write | `0x08040874` → `0x08031200(0, 0xFF00, 0x95+t)` | 1 locked SPI word per note |
| Trigger/release | `0x08040778`/`0x0804083C`/`0x08040814` → ring `0x080406A8` (3 words: 2 data + cursor Y:$307B5) | 3 locked SPI words per event |
| Param drain | `0x0803081C` → `0x0804070E` (cache 0x20006860+16t, Y:$307C8+32t via ring $B) | 1 locked word per dirty param |
| **Recorder idle poll** | recorder tick `0x0802F1F4` → `0x0802F256` then falls into `0x0802F204`; in states 0/1 it calls allocator vt+0x48 = `0x080644CC` → **SPI read of Y:$371D5 every tick** even when idle | 1 locked SPI read per tick; tick registered as slot 10 of the periodic table at 0x20000358 (`0x080267E8`); **rate unmeasured (U)** |
| DSP fs status reads | `0x080643B4` (free pages Y:$371D1), `0x0806455E` (busy Y:$371E7) | only from Sample Rec/meter UI (U for rates) |

There is no continuous sample stream, so **no SPI bandwidth is consumed by playback**. Note commands and parameters
stay (they also carry the FX parameters and drum selection, see §6).

---

## 2. External storage layout (Q2)

- **QSPI NOR flash = internal "current pack" filesystem volume.** Driver `0x08041A3A..0x08041D76` on HAL
  `0x0802A906..0x0802AAEA` (RCC AHB3ENR bit 1 at 0x0802A90E; IRQ 92 = 0x5C at 0x0802A960), DMA via `0x08024188/0x0802434A`.
  Device class vtable `0x0806866C` (object `0x2000037C` = 0x20000310+0x6C, config `0x20000004`), constructed at
  `0x080261B6`, initialised by `0x0802B17A` (enter QPI 0x35, 4-byte mode 0xB7, read ID 0xAF).
  Block-device methods: ioctl `0x0802B1BA` (sector count = 1<<ID[2] >> 12, **4 KB erase blocks**), read `0x0802B1F0`
  (cmd 0xEB/0xEC quad I/O, 6 dummy), program `0x0802B238` (0x02/0x12, 256-B pages), erase `0x0802B2D4`
  (0x20/0x21 4 KB, chip erase 0x60 on the magic range 0xCF525EB4).
- **SD card = second volume.** Device object `0x2000033C` (class vtable 0x08068640, ctor 0x0802B138). The SDMMC registers
  are reached through handle structs (0x20000108/0x200001F8/0x200002C8), not literals.
- Volumes: `0x08026664` creates filesystem objects (class 0x0802C190, vtable 0x08068698):
  `*0x20006E60` = QSPI volume, `*0x20006E64` = SD volume. FAT32 string at 0x08021A44.
- File types (table `0x080677A8`, stride 12 {flags, dir, suffix}, path format "%s/%s/%s"):

  | flags | dir | suffix | content | sampler-only? |
  |---|---|---|---|---|
  | 0x4000 | Sessions | _SESSION.ncs | projects | no |
  | 0x801 | Patches | _PATCHBANK.cpb | (Tracks-style banks) | no |
  | **0x8002** | **PCM** | **_PCM.wav** | **samples** (48 kHz mono 16-bit; "%02d_" naming) | **yes (data row only)** |
  | 0x203 | meta | _META.ncm | metadata | no |
  | 0x204 | GridFx | _GRIDFX.ncg | grid FX | no |

  Packs ("_PACK", 0x080697CC) bundle these; packs live on SD and are switched into the QSPI volume.
- **Memory-mapped mode: never used.** The command builder `0x0802A9AA` sets CCR FMODE=11 (`movmi r4,#0xC000000` at
  0x0802A9E6) only when descriptor flag bit 5 is set. All descriptors (0x080693B8/C8/D8/E8/F8, 0x08069408, and the
  adr-copied ones at 0x08041D78/88/98/A8) have flags 0x01/0x03/0x07/0x0D/0x0F/0x45: none has bit 5. No code literal
  points into 0x90000000. So QSPI XIP/read-only tables would need a new driver mode and coexistence with the filesystem
  driver (indirect mode switches would abort memory-mapped mode): **not a cheap option.**
- DSP external memory ≥ Y:$E80000 also holds a parked session image (SysEx msg 0x40/0x41, `0x08043C10/0x08043C5E` with
  `0x080536D2`/`0x08053690`, 0x25800 words): **not sampler-only; keep it reserved.**

---

## 3. ARM code that exists only for sampling (Q3)

| Piece | Address range(s) | Reached from | Evidence | Flash | Confidence |
|---|---|---|---|---|---|
| DSP sample-memory manager ("DSP filesystem") | object `*0x20006F24` (new 8 B at `0x080276B6`), vtable 0x08066E58 (0x54 B), methods `0x0806438C..0x080645D6`, `0x080674DC..0x08067524`; wrappers `0x0804A4A8..0x0804A4FC`, `0x0804B280..0x0804B2A6` | loader, recorder, Sample Rec, delete, meter | mailboxes Y:$372F8 alloc, $372F9 record, $372FB reset, $372FC free, $372FD/$372FE descriptor/trim; status $371D1/$371D2/$371D5/$371E7 | ≈0.6 KB | V |
| Recorder (DSP memory → WAV) | class vtable 0x080695C8, ctor `0x0802F1A8` (called at **0x080276E6**), code `0x0802F1A8..0x0802F6E4`; WAV header `0x0802F55C` (48000 Hz mono 16-bit into 0x20006AD8); glue `0x0804717A`, `0x0804719E`, `0x0804B264..0x0804B310`, `0x08045E54` | tick callback (periodic table slot 10) | §1 table | ≈1.3 KB (part of 2.8 KB) | V |
| Sample Rec controller | vtable 0x08068B30, ctor `0x0803C900` (built at **0x08028E26**), code `0x0803C900..0x0803CD2A` | Sample Rec button/view via router table in `0x08027980` | states idle/armed/recording/stopping; start → `0x0804B280`, stop → `0x0804B290` | ≈1.1 KB (part of 2.8 KB) | V code / I naming |
| Post-record commit controller | vtable 0x08068FD4, ctor `0x0803CD4C` built at **0x08028E78** (result discarded), `0x0803CD4C..0x0803CF40` | Sample Rec listener | issues $372FD/$372FE | ≈0.5 KB | I |
| Delete-sample controller | vtable 0x08067568, ctor `0x08039EBC` (at 0x08028690), methods `0x0803A07C`, `0x0803A0D8`, code `0x08039EBC..0x0803A1B0` | Sample view (delete gesture) | free slot ($372FC), delete file, reload slot | ≈0.75 KB | V/I |
| Sample loader + WAV parser | `0x08053E24..0x08054138`, `0x0805B294..0x0805B344`, `0x080460A8..0x080460C8`, `0x08031608`, string 0x08066FB0.. | pack controller `0x08039C7C`, `0x0803A0D8` | §1 | ≈1.15 KB | V |
| Audition/preview command | vtable 0x08069280, ctor `0x0803C840` (at 0x08028DEA), `0x0803C840..0x0803C900`, `0x080407F0..0x08040814`, thunks `0x080467CA/CE`, `0x08031A2A/2E` | router table listeners (+0xC start, +0x10 stop) | mailbox Y:$372FA → core-1 audition voice | ≈0.25 KB | I |
| Slicing/editing candidates | vtable 0x0806876C (ctor `0x0803A1B0`), vtable 0x08068474 (ctor `0x0803BDA8`), meter `0x0805E93C` | Sample view / Sample Rec knobs (U) | scale by slot length / free memory | ≈2.9 KB | **U: not positively identified; may share knobs we reuse** |
| Input settings → DSP | `0x08030D84` (globals group 0x21 items 0xC..0x10 → Y:$307B0/$307B1/$371E8/$371E9, GPIO 0x20000228) | Setup view, boot | $371E9 = record gate threshold, $371E8 = record source select (DSP side, S) | ≈0.1 KB | U: also boot path; keep |
| Components/SysEx sample transfer | handler `0x08043CC8` (+ helpers ≈0x08043B02..0x08044C48) from `0x0802D2A0` ← `0x08026DE8` ← USB MIDI | SysEx | handles all file types | 0 (shared) | V |
| Sample browser | Sample view (vtable 0x080672DC, ctor `0x0803CF68`) | — | **must stay** (drum kind) | 0 | V |
| Sample flip / per-voice params | `0x0804B060`, `0x08047D82`, `0x0805CE84`, `0x08040874`, `0x0804070E`, `0x0803081C` | sequencer | **must stay** | 0 | V |
| "Novation MSD" string | 0x08069468 | no reference found | — | 12 B | U |

**ARM totals:** ≈6.1 KB safe (V/I rows) + ≈2.9 KB uncertain (slicing candidates) + ≈0.1 KB (input settings).
The pieces are scattered (no contiguous block over ≈1.4 KB), so reusing them requires placing extension code in
several holes.

**How to cut (do not just delete constructor calls):** the Sample Rec (0x08028E26) and preview (0x08028DEA) controller
pointers are dereferenced into the router table at 0x08028EEA..0x08028F0A, and the delete controller pointer is passed to
`0x0803EC40` at 0x080292D0. Keep those constructors and stub the vtable methods (`movs r0,#0; bx lr`), or point the
router entries at a dummy listener. The commit controller call at 0x08028E78 can be removed outright. If the recorder
ctor at 0x080276E6 is removed, `*0x20000828` users (`0x0804717A`, `0x0804B290`, `0x0804B2A8`, `0x0804B2BE`) must be dead
or guarded.

---

## 4. RAM owned by the sampler (Q4)

### ARM SRAM
| Item | Address / size | Notes |
|---|---|---|
| DSP fs object | heap, 8 B (`0x080276B6`), pointer `0x20006F24` | removable with all wrappers |
| Recorder | heap 0x10 + state 0x74, pointer `0x20000828` | removable with recorder |
| Sample Rec / commit / preview / delete controllers + listeners | heap ≈0x38 + 0x228 + 0x14 + 0x30 + listeners | ≈0.7 KB; stubbed, not freed, in a first build |
| WAV header / file buffer | `0x20006AD8`, 64 B | shared only by recorder and loader |
| Slot length table | `0x20000A8C`, 128 entries | cleared by loader (size U: 128 × 1–4 B) |
| Edge-callback slots | `0x20006EC8..0x20006EDC`, 24 B | recorder glue |
| Shared scratch | `0x20003F94`, 4 KB | **not sampler-owned** (12 users incl. SysEx, file ops) |
| Per-slot sampler cache | `0x20006860 + 16t`, 128 B | **must stay** (drum kind, FX params) |
| Note state | `0x20006D90`, 16 B; ring cursor `0x20006F38` | **must stay** |
| Project image | heap 169,096 B (`0x08027D8C`) | not sampler; includes per-step sample flip |

ARM RAM reclaim ≈1 KB (mostly heap). It does not relieve ITCM. Heap space gained by not constructing objects lowers the
native heap high-water mark; it does **not** permit changing the heap limit 0x2004F760.

### DSP memory (core-1 and shared)
| Space | Range | Words | Use | After sampler off |
|---|---|---:|---|---|
| X | $200–$29FF | 10,240 | interpolation coefficient tables | free (and drop from stream) |
| X | $2A48–$2A4D | 6 | pitch exp polynomial | free |
| Y | $400–$13FF | 4,096 | voice sample rings | free |
| Y | $122–$283 | 354 | amp-envelope tables | free (and drop from stream) |
| X/Y | $40–$BF | 128 + 128 | voice records | **keep**: consumer FX state (X $45..$49,$4E; Y $46..$4F +16t) |
| X | $2A00–$2A47, $2A4E–$2ACE, $C0, $100–$13F | — | block scratch, HPF cutoff table, distortion scratch | keep |
| Y | $1400–$1405 | 6 | biquad scratch, track counter | keep |
| shared | $30000–$301FF | 512 | slot descriptors | free once the ARM stops writing them |
| shared | $306F0–$3070F | 32 | audition output (core-0 mixes it) | keep zeroed |
| shared | $30710–$307AF | 160 | record sources (written by core-0 every block) + mono buffer | free only after core-0 patch |
| shared | $308C8–$366C7 | 24,064 | per-slot page tables | free* |
| shared | $366C8–$371CE | 2,823 | page counts + free-page ring | free* |
| shared | $371CF–$37300 | ≈300 | allocator/record/audition/normalize variables, mailboxes | free* |
| shared | $37301–$37310 | 16 | voice gain ramps | free |
| ext Y | $300000–$D86FFF | ≈11.04 M | sample pages | free* (≥$E80000 session parking stays) |

\* only after the ARM no longer issues load/alloc/record traffic and polls (Y:$37300, $371D1, $371D2, $371D5, $371E7).
Shared memory that must stay: $30200–$303FF distortion table (filled by ARM `0x080406E6` at boot), $30410–$3042F command
ring, $30430–$304EF core-0 buffers, $304F0–$306EF track spans, $307B0–$307C7, $307C8–$308C7 parameter blocks, FM FIFO $37700+.

---

## 5. DSP core-1 sampler voice code (Q5)

Main loop P:$0111..$0129 per block: $07F8, $04CD, [$0D40 our consumer → jsr $0221 … jmp $08A6], $087B, $08BD, $02E4,
$0289, $03C1, $0469, $0CE2, $0A79. Block = X:$1 = 32 frames.

| P range | Function | Sampler-only? | Our firmware needs it? |
|---|---|---|---|
| $0151–$0159 | boot clear of voice rings | yes | no |
| $015A–$01C2 | voice start / ring refill planner | yes | no |
| $01C3–$01E4 | pitch → coefficient table select | yes | no |
| $01E5–$0220 | 16-tap interpolator → X:$2A02, refill check | yes | no |
| $0221–$0233 | gain targets ($0278), clear Y:$1405, **clear active bank of all 8 track spans** | clear: no | **yes** (underrun path) |
| $0234–$0243 | voice traversal: $01E5, $0511, $025C, $0244, $06AB | yes | no |
| $0244–$025B / $025C–$0277 / $0278–$0288 | gain ramp / end-of-sample declick / gain init | yes | no ($0278 harmless) |
| $0289–$02D9 | round-robin start/end/pitch recompute (runs idle) | yes | no |
| $02E4–$0373 | mailbox scan + trigger setup ($02FD); $02FD clears Y:$46..$4D (our filter state) | yes | **must not run** |
| $0374–$04CC | audition player → Y:$306F0, fade | yes | no (keep zeroing $306F0) |
| $04CD–$0510 | command consumer ($9/$8/$B) | trigger part | **yes** ($B param → Y:$307C8+32t) |
| $0511–$0528, $0529–$0533, $05CE–$0647 | voice stage head, amp envelope | yes | no (consumer inlines the head) |
| $056E–$05CD | HPF, LPF+resonance, biquad core $05B3 | no | **yes** |
| $0648–$06AA | distortion (table X:$30200) | no | **yes** |
| $06AB–$06CF | accumulate into span + `bra $0234` | yes | no |
| $06D0–$073C | port ISR / port config | generic | keep |
| $073D–$07F7 | external page fetch | yes | no |
| $07F8–$0865 | ARM mailbox dispatcher | yes | keep until ARM side patched |
| $0866–$08F1 | Sample Rec gate/source/write | yes | no |
| $08F2–$0D31 | free/trim/alloc/reset/normalize | yes | no (but the ARM handshakes with it) |
| $012A–$0150, $0534–$056D | no static reference | likely dead (U) | leave in place |

**Measured cost (interpreter, cycles per block):** native stages idle 2,036; 8 voices steady 21,972.
Per voice ≈2,490: interpolation+refill 1,404, FX+envelope 670 (HPF+LPF 471), gain ramp 282, end check 25,
accumulate+traversal ≈110; distortion +772; new trigger +≈1,100 in its block. External-memory wait states are not
modelled, so the real fetch cost is higher.

**Freed if voices never run:** ≈20 k cycles/block with 8 voices (≈5 k for the two sample tracks 7–8 of 0.1.17),
≈1.4–1.7 k idle. P ≈2,655 words (≈470 must stay). X ≈10,250 words, Y ≈4,450, shared ≈28 k, external ≈11 M.

---

## 6. UI and paths that must survive (Q6)

- **Sample view** (vtable 0x080672DC, ctor 0x0803CF68): its pad grid writes the selected sample slot = **param 0** →
  `0x0804070E` → cache `0x20006860+16t` → drain `0x0803081C`. Rhythm FM reads `slot & 7` (kind) and `slot & 15 >= 8`
  (gnarly) from that cache for tracks 4–6, and hooks `0x0803D7FE` (pad), `0x0803D8B8`/`0x0803D956`/`0x0803DC94`/literal
  `0x0803E444` (Shift + track). Keep the view, its pad → param-0 path, and the 64-pad "slot has sample" property bitmap
  (pack controller). Pads for empty slots must still be selectable (U: check whether the view refuses empty slots once
  no samples are loaded, see §8).
- **Sequencer sample flip**: player vtable 0x08068D20 +0x10 → `0x0804B060` (step byte 5; 0xFF = base from `[player+0x60]`),
  editor `0x08047D82` (lane 12), preview `0x0805CE84`/`0x0805CEC6` (our `plock_preview` hook). All feed param 0. Keep.
- **Per-voice parameter writer** `0x0804070E` and command ring `0x080406A8`, dispatch `0x08040874`, trigger `0x08040778`,
  release `0x0804083C`, stop `0x08040814`, drain `0x0803081C`: keep (our hooks/calls; FX params $A–$D).
- **Distortion table fill** `0x080406E6` (X:$30200, boot): keep.
- Pack controller (vtable 0x0806840C): keep; only its loader call is cut.
- Storage task/queue `0x08045EB6..0x080460E8` (minus loader thunks), path builder `0x0802E65C..0x0802E9DC`, QSPI/SD drivers,
  SysEx file transfer, DSP SPI layer (`0x08031228`, `0x080314D6`, `0x08031430`, `0x080313D0..0x08031626`; only
  `0x08031608` is loader-only): keep.

---

## 7. Removable pieces — summary table

| # | Piece | Addresses | Flash | ARM RAM | ARM CPU | SPI bandwidth | DSP cycles/block | DSP memory | Risk |
|---|---|---|---|---|---|---|---|---|---|
| D1 | Core-1 voice traversal | P:$0234 → `rts` | 0 | 0 | 0 | 0 | **≈2.5 k per sounding voice** (≈20 k for 8) | — | Low once no track routes to the native sampler; kills tracks 7–8 samples |
| D2 | Trigger setup | P:$02E4 → `rts` | 0 | 0 | 0 | 0 | ≈1.1 k per trigger + 235 idle | — | Low; also stops $02FD wiping consumer filter state on notes |
| D3 | Pitch round-robin | P:$0122 `jsr $0289` → `nop` | 0 | 0 | 0 | 0 | 146 | — | Low (writes only Y:$40..$44+16t) |
| D4 | Interp. + envelope tables in the relocated stream | stream records X:$200 (10,240 w), Y:$122 (354 w) | **≈31.8 KB at image end** | 0 | faster boot DSP download | DSP download −31.8 KB at boot | 0 | frees X $200–$29FF, Y $122–$283 | Low **only after D1** (tables are unread); medium if any path still reads them |
| D5 | Sample loader | `bl 0x080460A8` at 0x08039C7C → `movs r0,#0`; stub `0x0803A0D8` | ≈1.15 KB | 64 B + table | boot/pack-change seconds (U) | up to all sample bytes per pack load | allocator work at load | ext Y free | Low/medium: descriptors stay zero → native voices silent; ensure the Sample view still lets pads be selected |
| D6 | Recorder + Sample Rec controller + commit | 0x080276E6, 0x08028E26 (stub methods), 0x08028E78 | ≈4.1 KB | ≈0.4 KB | idle poll per tick | 1 SPI read/tick | $087B/$08A6/$08BD ≈220 | shared $30710.. after core-0 patch | Medium: router-table pointers; null `*0x20000828` users |
| D7 | Audition/preview | stub 0x0803C88C/0x0803C8B2 (vt 0x08069280) | ≈0.25 KB | 20 B | 0 | per audition | $03C1+$0469 ≈714 if removed DSP-side | Y:$306F0 (keep zeroed) | Medium: stale $306F0 → buzz if DSP side removed first |
| D8 | Delete-sample controller | stub methods of vt 0x08067568 | ≈0.75 KB | 48 B | 0 | 0 | 0 | — | Low/medium (pointer passed to 0x0803EC40) |
| D9 | DSP fs class + wrappers | 0x0806438C..0x080645D6 etc. | ≈0.6 KB | 8 B | 0 | handshake polls | dispatcher $07F8 ≈69 | ≈28 k shared words | Medium: remove only after D5–D8 |
| D10 | Core-1 sampler P code (stage 2) | P:$0151–$0220, $0234–$02D9, $02E4–$0510 (trigger part), $0529–$0547, $05CE–$0647, $06AB–$06CF, $073D–$0D31 | via stream: ≈8 KB | 0 | 0 | 0 | rest of idle ≈1.7 k | ≈2,655 P words for new engines | High: only after ARM side no longer talks to the mailboxes; leave unreferenced blocks |
| D11 | Slicing/editing candidates | vt 0x0806876C, 0x08068474, `0x0805E93C` | ≈2.9 KB | small | 0 | 0 | 0 | — | High / unidentified |
| D12 | Core-0 record source/audition mix | core-0 P:$02A0–$02B1, $19FA–$1A10, $1A11–$1A25 | via core-0 stream | 0 | 0 | 0 | core-0 cycles (unmeasured) | shared $306F0–$307AF | Medium; core-0 is not otherwise touched by us |

---

## 8. What must stay

- ARM: Sample view and its pad → param-0 path; pack controller; sequencer sample flip (`0x0804B060`, `0x08047D82`,
  `0x0805CE84`); `0x08040874`/`0x08040778`/`0x0804083C`/`0x08040814`/`0x0804070E`/`0x080406A8`/`0x0803081C`;
  `0x080406E6`; cache `0x20006860`, note state `0x20006D90`, ring cursor `0x20006F38`; storage task, path builder, file
  type table, QSPI/SD drivers, SysEx file transfer; DSP SPI helpers and mutex `0x20003688`; heap limit 0x2004F760 and backup
  SRAM untouched.
- Core-1: main loop; $04CD ($B parameter path); span clear in $0221–$0233; $056E/$0590/$05B3/$0648 with their tables and
  scratch (X:$2A4E.., X:$30200, X/Y:$C0, X:$100–$13F, Y:$1400–$1405); voice records X/Y:$40–$BF; ISR $06D0–$073C;
  dispatcher $07F8 and allocator/record/audition code **until the ARM side stops using them**; the session parking
  region ≥ Y:$E80000.
- Core-0: unchanged in a first build (it mixes Y:$306F0, keep it zeroed).

Open checks before or during stage B:
1. Does the Sample view still select a slot whose sample is absent (no file → bitmap bit clear)? If not, drum-kind
   selection on tracks 4–6 breaks: keep the "slot has sample" bitmap forced on, or keep the PCM files present while only
   skipping the DSP load (D5 alone leaves the files and bitmap untouched — U: verify which one the view reads).
2. Recorder tick rate (how much SPI time D6 frees).
3. Exact meaning of mailboxes $372FD/$372FE and the slicing candidates.
4. DSP clock / real wait states (cycle numbers are interpreter lower bounds).

---

## 9. Recommended order of cuts for a first "sampler off" build

1. **Route all 8 tracks away from the native sampler on the ARM** (our note hooks already intercept tracks 1–6; extend to
   7–8 or leave them silent). Without this, D1 silences tracks 7–8.
2. **D1 + D2 (+ D3)**: three one-word core-1 patches in the builder's stream (P:$0234 rts, P:$02E4 rts, P:$0122 nop).
   This is the CPU win (≈20 k DSP cycles/block with 8 voices; ≈5 k vs 0.1.17's two sample tracks) and protects the consumer's
   filter state. Verify in the DSP interpreter that spans are still cleared and the consumer output is bit-identical.
3. **D4**: drop the X:$200 and Y:$122 records from the relocated stream (≈31.8 KB of contiguous image-end flash). The
   builder must keep checking that nothing else maps those addresses.
4. **D5**: cut the loader call at 0x08039C7C and stub 0x0803A0D8; check open item 1 (pad selection) on hardware. This
   removes boot/pack-change SPI bursts that bypass the mutex.
5. **D6–D8**: stub Sample Rec, commit, audition and delete controller methods (keep constructors for the router table);
   remove the recorder tick (idle SPI poll). Make Sample Rec button a no-op.
6. **D9 + D10 (stage 2)**: once no ARM path reaches the DSP filesystem, remove the class and reuse core-1 P/X/Y
   (≈2,655 P words, ≈14.7 k X/Y words, ≈28 k shared) for new DSP-side engines/FX. Keep the dispatcher until the last ARM
   poller is gone; zero Y:$306F0 if audition is removed.
7. Leave D11 (slicing candidates) and D12 (core-0) for later investigation.

Validation for each step: offline DSP consumer cases and ARM integration cases as today; report counters for
service rate/underruns on hardware; boot and pack switch with an empty and a full pack.
