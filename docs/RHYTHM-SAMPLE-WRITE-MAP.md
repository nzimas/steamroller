# Rhythm 5706: writing a generated sound into a sample slot (map)

Date: 2026-10-16. Research only. No firmware source or build was changed, nothing was committed, installed or sent to
the device. Target: stock ARM image `firmware/official/circuitrhythm-firmware-5706.bin` (base 0x08010000) and the core-1
image unpacked by `tools/rhythm_dsp_probe.unpack_stream`. This builds on [RHYTHM-SAMPLER-REMOVAL-MAP.md](RHYTHM-SAMPLER-REMOVAL-MAP.md).

Confidence tags: **V** = verified in the disassembly (or on hardware, where noted); **S** = static reading of code that
was not traced end to end; **I** = inferred from structure; **U** = unverified.

---------------------------------------------------------------------------------------------------------------
## 0. Summary

1. **Use the storage task and a `{4, slot, 0xFE}` path, as the stock recorder and SysEx upload do.** Delete (op 5),
   write the WAV in ≤4 KB chunks (op 7), close (op 8). The requests are asynchronous when a callback is passed. The
   storage task delivers each callback in the **main task**, outside the DSP-SPI mutex. **V**
2. **To make the slot playable, emit the stock "file changed" event**:
   `0x08035D62(*(void**)0x20006EAC, &path{4, N, currentPack})`. The delete-sample controller listens for it. It then
   rescans the slot's "has sample" bit and reloads the slot: it frees the DSP pages ($372FC), checks that the file exists
   and calls `0x08053E8C(N)`. This is the same chain that runs after a Components/SysEx sample upload. **V** (chain), **S** (end to end)
3. **Pages must be freed before a reload.** Allocation appends pages to the slot's page table, and the descriptor
   start stays 0. Without the free, playback would start in the old pages. The event path above frees them. **V/S**
4. **WAV accepted by the loader:** `RIFF`/`WAVE`/`fmt `, with `data` immediately after `fmt ` (no `LIST` chunk). Mono,
   48000 Hz, 16-bit. The data size must be below 0x0124F801. The loader clamps it to 0x2F0000 bytes (32.08 s) and
   **drops the last 256 samples**, so append 256 zero samples. **V**
5. **Sample Rec view = router view id 4** (object `router+0x58`, class vtable 0x08067A00). Pads 0–15 select slot
   `pad + 16·page` and audition it if it has a sample. Pages go 0–7 with buttons 0x13/0x14 (128 slots). A **track
   button (no Shift) briefly switches to that track's view**: if a pad is pressed while it is held, the pad goes to the
   track view, and the view switches back on release. **Shift + track has no stock meaning** in view 4: the mask at
   literal 0x0803E444 discards ids 0–7. The existing hook at 0x0803D956 is reached in view 4 (not in view 9). **V**
6. **Audition via Y:$306F0 is not possible.** Core-1 $03C1 overwrites all 32 words every block, with zeros when idle.
   Writing into a track span after $0221 **is heard on that track through level, pan and FX**. That is the Rhythm FM
   consumer path, and its level/pan/FX behaviour was confirmed on hardware with the FM tracks. **V**
7. **Do no storage or DSP-locked work inside `fm_service`.** The hook at 0x08029F70 runs while the main loop holds the
   non-recursive DSP-SPI mutex (0x08025894). A locked SPI helper would deadlock there, and a synchronous storage call
   would stall the whole main task. **V**

---------------------------------------------------------------------------------------------------------------
## 1. Writing a sample file (Q1)

### 1.1 Path structure and file naming — V
`path` is 12 bytes: `{u32 type; u32 index; u8 pack; pad[3]}`. type 4 = sample (PCM). pack 0xFE = current pack (resolved by
`0x08040E28`: 0xFE → `**(u8**)0x20006E24`; 0xFF prints "Warning: INVALID_PACK Access"). Types 2–6 map through the byte
table `0x20003748[type]` to rows of the file-type table 0x080677A8 (`{0x8002, "PCM", "_PCM.wav"}` for samples).

- Path format "%s/%s/%s" (0x0802EA48, used by the directory rescan `0x0802E6C2`). Files are looked up by **index**, not
  by name, through a per-directory index (`0x080456D8`/`0x080456E0`).
- When the slot has no file, `0x0802E874` (open, mode bit0 = create) creates one with `0x08045708` and the name format
  **"/%02d%s"** (0x08045760), which gives `NN_PCM.wav` with NN = slot index (**I**: 0-based). When a file exists, its
  existing name is reused. After a new file is created, the write op calls `0x0802E6C2` (directory rescan).
- Current pack index for paths that must hold the real value (§2): `*(uint8_t*)(*(uint32_t*)0x20006E24)`.

### 1.2 Storage task API — V
Request builders `0x08045EB6(op, path*, params*, cb, ctx, out*)` and `0x08045F42(op, path*, cb, ctx, out*)`. Both
build a 0x48-byte request: `{u8 op; path @4; params @0x10/0x14/0x18; cb @0x3C; ctx @0x40; out @0x44}`.
`0x08045E98` posts it to the storage task object `*0x20006CA8`:

- **cb ≠ 0 → asynchronous.** `0x0802ACB8` copies the request into the queue (prints "Req Queue is full!" and waits when
  full), wakes the task and returns 0.
- **cb == 0 → synchronous.** `0x0802ACA2` posts and then spins with `yield` (`0x08024138` = PendSV) until the storage
  task is idle again. It returns the result code. It must not run on the storage task itself (`0x0802ACF8` returns at
  once there and the result is garbage). It blocks the calling task.

Dispatcher `0x0802AD40` (tbh table at 0x0802AD80). Wrappers:

| Wrapper | Op | Handler | Meaning (types 2–6) | Notes |
|---|---|---|---|---|
| `0x08045FB4(path*)` | 0 | 0x08040EB4 | file exists (index lookup `0x0802E7B6`) | sync; returns bool |
| `0x08045FCE(path*)` | 1 | 0x08040F92 | file size | sync |
| `0x08045FDE(path*)` | 2 | 0x0804106C | free space, 64-bit | type 1 (pack): `0x01E14000 − pack usage` (≈30 MB pack budget, **S**); type 8: volume free (**I**) |
| `0x08046098(path*, cb, ctx)` | 5 | 0x08041388 | **delete** + directory rescan | 5 = "not found" (SysEx treats 5 as OK) |
| `0x0804603A(path*, off, buf, len, cb, ctx)` | 6 | 0x0804113A | read | 0 ok, 4 short, 2 error, 5 not open/not current pack |
| `0x08046054(path*, off, buf, len, cb, ctx, u32* nread)` | 6 | same | read with byte count | used by the SysEx read-back verify |
| `0x0804607A(path*, off, buf, len, cb, ctx)` | 7 | 0x080411D8 | **write** at offset (opens or creates every time) | 0 ok, 2 fs error ("Content::Write: Failed"), 3 short write, 5 bad index/pack |
| `0x080460C8(path*, cb, ctx)` | 8 | 0x08041906 | **close/flush** the shared file object (`[0x20003748+8]->vt[0x20]`) | 6 on error |
| `0x080460A8(pack)` / `0x080460B8(slot)` | — | direct | not queued: run `0x08053E24` / `0x08053E8C` in the caller | return 0 ok, 5 empty/failed |

Thin "file manager" wrappers (the first argument is ignored): `0x0804736C(x, path*)` = exists, `0x08047366` = size,
`0x08047270` = sync read, **`0x080472BC(x, path*, off, buf, len)` = sync write + sync close**, `0x08047316` = sync
delete + close, `0x08047332(x, path*, listener)` = async delete with listener (used by the delete controller).

**Callback** (async): `cb(path* copy, u8 kind, u8 result, u32 a, u32 b, void* ctx)`. For read and write, a = buf and
b = bytes transferred. kind comes from the templates at 0x0802B098+: op5 = 2, op7 = 1, op8 = 5, op6 = 0. Delivery:
`0x080419CC` posts it to the main-task queue `(*0x20006CA0)->4`, and the main loop drains that queue with `0x080252C8`
at **0x08025922, after the DSP mutex is released** (mutex section 0x08025894..0x080258BC). So callbacks may call
locked DSP helpers and synchronous storage ops (the stock delete callback does both).

**Chunk size:** the API has no limit (len is a u32). Stock writers use ≤4 KB: recorder ≤0x800 samples per op, SysEx
4 KB, loader reads 2 KB.

**Buffers:** the 4 KB scratch `0x20003F94` is guarded by a counting semaphore at `0x20003F8C`.
`0x0802EF6A(tag)` acquires it (yield-spins until free) and returns 0x20003F94. `0x0802EF98()` releases it.
`0x0802EF5A()` peeks (returns the buffer or 0). The recorder and SysEx hold it across an async write and release it in
the callback. **The loader `0x08053E8C` uses it without the semaphore.** DTCM buffers work for QSPI writes (the stock
code does this). ITCM buffers cannot be DMA sources (handoff), so do not pass ITCM buffers to the storage task.

### 1.3 Sample Rec recorder (DSP → WAV) — V
Object `*0x20000828` (class 0x080695C8, state 0x74 B at obj+4, tick = periodic-table slot 10, run in the main loop at
0x0802586C).
- start `0x0802F354(obj, slot<0x80, path{4,idx<0x80,pack<0x20}, cb, ctx)`: **async delete** `0x08046098(path, 0x0802F3E1)`,
  copies the path, state 1 after the delete callback.
- tick `0x0802F256` → `0x0802F450`: reads DSP sample memory into the scratch (DSP-fs vt+0x4C = acquire scratch,
  vt+0x18 = bulk read), then **async write** `0x0804607A(path, 0x2C + 2·written, scratch, 2·n, 0x0802F5CD, state)`
  with n ≈ 0x7EA..0x800 samples (`0x0802F3E8`).
- callback `0x0802F5CC`: on success, written += bytes/2 and the scratch is released (vt+0x50); when done, state 4.
- state 4 → `0x0802F55C` writes the **44-byte header last**, at offset 0, from 0x20006AD8: RIFF, **size field not
  written** (stale), WAVE, fmt 16, PCM 1, mono, 48000, 0x17700, align 2, 16 bits, data = 2·samples. Then **close**
  `0x080460C8` (state 6 → 7), reset, user cb.
- "Sample Save Error" (0x0802F678) on failure.

### 1.4 Components / SysEx upload — V
Handler object (ctor `0x08043B2C`; its buffer object wraps the scratch, `0x080534A8(…, 0x20003F94, 0x1000)`):
- begin `0x08044162`: **sync delete** of the target file (`0x08046098`, result 0 or 5 accepted).
- data `0x080449C0..0x08044A44`: each full 4 KB buffer is written with op 7 at the running offset (sync, or async with cb
  0x08044ACD); the scratch is released after each.
- end `0x08044052`: last partial write (sync) + **close** (op 8, twice). Read-back verify in 4 KB chunks with
  `0x08046054` into the scratch (checksum `0x08043B08`), reply, then **`0x08044B74` → notify delegate
  `[handler+0x40]` = 0x20000A40** (`0x08024B68(0x20000A40, 0, path)`). That reaches the content manager (§2.2).

### 1.5 WAV format accepted by the loader (`0x0805B2B4`) — V
Header read: 0x40 bytes, sync, into 0x20006AD8 (`0x08053E8C`).

| Offset | Check |
|---|---|
| 0 | "RIFF" (size at +4 **not checked**) |
| 8 | "WAVE" |
| 0x0C | "fmt " ; fmt size at 0x10 = F |
| 0x14 | format tag **not checked** (write 1) |
| 0x16 | channels **== 1** |
| 0x18 | sample rate **== 48000** |
| 0x1C/0x20 | byte rate / block align not checked |
| 0x22 | bits **== 16** |
| 0x14+F | **"data" must follow fmt directly** (no LIST/fact chunk). It must lie inside the 0x40-byte read, so F ≤ 0x24 |
| 0x18+F | data size **< 0x0124F801** |

- Data offset = 0x1C+F (0x2C for F=16, `0x0805B2A0`). Length `0x080540D4`, **clamped to 0x2F0000 bytes** (1,540,096
  samples, 376 pages = the per-slot page-table size 188 words × 2).
- Samples: int16 little-endian in the file. Each 2 KB chunk is `rev16`-swapped (always 1024 halfwords) and streamed with
  `0x08031608`.
- Descriptor (`0x08031228`, shared X/Y:$30000+4·slot): word0 = 0; words 1–3 = **(samples − 0x100) << 8**. The last
  256 samples are never played (interpolation guard), and a file shorter than 256 samples gives a negative length.
  **Append ≥256 zero samples; minimum length >256 samples.**
- Stereo, other rates, other bit depths, or an extra chunk: the parse fails, the descriptor is zeroed and the slot is
  silent. **The Sample view still shows the slot as filled** (the bitmap follows file existence, §2).
- Page allocation: the first page before the data, and one more after every 4096 words, so pages = ⌊n/4096⌋+1.
  When a page allocation fails: "DSP Filesystem reports Sample Memory Exhausted." (0x08066FB0), and the slot keeps what was
  loaded (descriptor = loaded length − 256). DSP address ≥ $E00000: "Sample Memory Exceeded" (0x08066FE0).

---------------------------------------------------------------------------------------------------------------
## 2. Overwriting a slot (Q2)

### 2.1 Delete-sample controller (vt 0x08067568, ctor 0x08039EBC, object 0x30 B) — V
Fields: +0x10 event source (content manager +0x2C list); **+0x14 "slot has sample" bitmap property** (128 bits, new
0x18 B, `0x0804A42E(obj, 0xB)`); +0x18 current-pack property; +0x1C file manager; +0x20 delete-state property (field 0 =
{state 6, pack, slot}, field 1 = busy); +0x24 Sample Rec state property; +0x28 record-slot property.

| Method | Address | What it does |
|---|---|---|
| file event (vt0) | `0x08039F58(this, list, code)` | if `(code & 0x3F) == 4`: slot = `(code>>16)&0xFF` → `0x08039FB4` rescan slot → **`0x0803A0D8` reload** |
| rescan all | `0x08039F7A` | for slot 0..127: `0x0804736C` exists (sync op 0) → `0x0804A474(bitmap, slot, exists)`; notify `0x0803FBEC` |
| rescan slot | `0x08039FB4(this, slot)` | same for one slot + notify |
| property listener | `0x08039FEC` | pack change → rescan all; Sample Rec state == 3 → set the bit of the recorded slot |
| delete request | `0x0803A048` → `0x0803A10E(this, slot)` | if not busy and the bit is set: state {6, pack, slot}, busy = 2, **`0x0804A4A8(slot)` frees DSP pages ($372FC)**, **async delete** `0x08047332` |
| delete done | `0x0803A07C` | state 6 → clear the bit, notify; busy = 0 |
| **reload** | **`0x0803A0D8(this, slot)`** | `0x0804A4A8(slot)` (free) → `0x0804736C` exists? → `0x080460B8(slot)` → `0x08053E8C(slot)` (sync reads, SPI stream, pages, descriptor) |

So "delete" = free the DSP pages + delete the file + clear the bit. "File changed" = update the bit + free + reload.

### 2.2 How a finished upload reaches the controller — V
- Content manager: class vt 0x08068F48, ctor `0x08035844`, object 0x58 B. **Pointer = `*(void**)0x20006EAC`**
  (registered by `0x08045F7E`, which also adds `0x08045F6F` to the delegate 0x20000A40).
- `0x0803598C(cm, path)`: for type 4, if `path.pack == current pack` (the real index, **not 0xFE**), it sets
  dirty bit idx in `cm+0x18` (128 bits). Unless busy, it then drains through `0x08035AF0`/`0x08035C90`, which emit
  **`0x08035D62(cm, &path{4, idx, curPack})`**.
- `0x08035D62(cm, path*)` packs `code = type | pack<<6 | idx<<16` (`0x0804756C`) and dispatches it with
  `0x08031782(cm+0x2C, code)` to every listener on that list (delete controller, pack controller and others).

### 2.3 Minimal sequence: "replace slot N with a new WAV and make it playable now"
All steps run in the main task, outside the DSP mutex: from a UI hook or from a storage callback.

1. `0x08046098(&{4,N,0xFE}, cb, ctx)`: delete the old file (result 0 or 5 = OK). This frees its flash space and
   avoids a stale tail when the new file is shorter.
2. `0x0804607A(&{4,N,0xFE}, off, buf, len, cb, ctx)` for each chunk: 44-byte header + data + 256-sample zero tail.
3. `0x080460C8(&{4,N,0xFE}, cb, ctx)`: close.
4. In the close callback: **`0x08035D62(*(void**)0x20006EAC, &{4, N, curPack})`**. This synchronously runs the bitmap
   rescan + `0x0803A0D8` (free pages → exists → load). (Stock-equivalent alternative:
   `0x08024B68((void*)0x20000A40, 0, &path)`, which marks the slot dirty and drains when the content manager is idle.)

**Do pages have to be freed first? Yes.** DSP $0BDB (cmd $372F8) pops a free page and *appends* it to the slot's page
table. The descriptor start is 0, so without $08F2 (cmd $372FC, "return pages, clear descriptor") playback would begin in
the old pages. `0x0803A0D8` always frees before loading. If you bypass the event: `0x0804A4A8(N)` then
`0x080460B8(N)`, then update the bitmap yourself (the bitmap property is only reachable through the controller object,
so the event path is simpler).

Free memory check before a commit: `0x0804A4F4()` = free pages (Y:$371D1). The old slot's pages come back first.
Old length: `0x0804A4E4(N)` (descriptor). Need ⌊n/4096⌋+1 pages. Total for a pack: 2,695 pages ≈ 230 s at 48 kHz.

---------------------------------------------------------------------------------------------------------------
## 3. Sample Rec view UI (Q3)

### 3.1 Top-level UI router (class vt 0x080672DC, ctor `0x0803CF68`, object 0x234 B) — V
- Pad press `0x0803D7FE(router, pad, vel)` → `router[0x7C]` (current page view, or `router[0x78]` when 0x7C is null)
  vt[0]. Pad release `0x0803D896`. Button press `0x0803D8B8(router, id)`. Button release `0x0803DDB0` → `0x0803DCC8`.
- **View id** = byte 0 of property `router+0x14` (`0x0803FC58(*(router+0x14), 0)`). View table at `router+0xB0`:
  id0 `[+0x3C]` track pages, id1 `[+0x44]`, id2/id7 `[+0x6C]`, id3 `[+0x54]`, **id4 `[+0x58]` = Sample Rec view**,
  id5 `[+0x5C]`, id6 `[+0x50]`, id8 `[+0x40]`, id9 none (modal: all buttons go to `[+0x78]`, and the Shift branch is
  not reached).
- Held-button bitset **`router+0xA0`** (ids 0..27): `0x08051B7A` test-and-set on press (a repeat press is ignored),
  `0x08051BB8` clear on release. Held pads: `router+0x9C`. Shift = property `router+0x0C`.
- Button → view (no Shift): 13 → 1, 14 → 2, 15 → 3, **18 (0x12) → 4**, 22 → 5, 26 → 6. Shift or a second press:
  14 → 7, 18 → `[+0x3C]` code 0xA, 26 → 8 (refused while recording, `0x08051C82`), Shift+25 → 9.

### 3.2 Sample Rec view (view 4; class vtable 0x08067A00, ctor `0x0804F884`, object 0x60 B) — V
Identified by its constructor arguments: it receives the Sample Rec controller's start/stop listener (router ctor at
0x0803D32C, frame `[sp+0x268]` = builder `[sp+0x158]` = `[SampleRecCtl+8]`).
- vt: [0] pad `0x0804F9D8`, [1] pad release `0x0804FB26`, [2] button `0x0804FD34`, [4] `0x0804FD68` (encoder, **U**),
  [7] `0x0804FE9A`.
- **Pads 0–15:** slot = `pad + 16·page` (page = byte `view+0xC`, 0..7). Normal mode: if the slot has a sample (bitmap
  `view+0x30`, `0x0805985C`), **audition it**. Listener `view+0x50` → preview controller (vt 0x08069280) →
  `0x080407F0(slot, a, b)` → Y:$372FA → core-1 $081C plays the slot into Y:$306F0 (master, not a track). Then it
  **writes the record-slot property** `view+0x20` (= the Sample Rec controller's `+0x18`). Pad release stops a gated
  audition. Delete mode (property bit 2 of `view+0x24` and `view+0x2C` bit 0; probably Clear held, **I**): pressing the
  selected filled slot fires listener `view+0x3C` (delete request).
- **Pads 16–23** → sub-object `view+0x1C` (pad−16; only when `0x0804FF98`). **Pads 24–31** → `view+0x18` (pad−24),
  ignored while recording. Their meaning is **U** (probably source, threshold and monitor options).
- **Buttons reaching the view:** 0x13 page+1 (max 7), 0x14 page−1. Button 0x19 (Record) in view 4 goes to
  `router[+0x4C]` instead of `[+0x48]` (`0x0803EA8A`).

### 3.3 Answers
- **Eight track buttons (no Shift):** the router (`0x0803D9A4`) selects the track (`0x0804D662`) and switches to that
  track's view (`0x0803DEBC`), saving Sample Rec as the return view in `router+0x19E+id`. On release (`0x0803DD04`): if a pad
  was pressed during the hold (`router+0x1BA`) or the 0x14D ms hold timer fired, it **returns to Sample Rec**
  (momentary peek). Otherwise it stays in the track view.
- **Pad press:** select the record slot and audition it if it is filled (§3.2).
- **Track held + pad:** stock sends the pad to the *track view* (peek), not to Sample Rec. Our gesture must suppress the
  navigation on press (see the hooks below).
- **Shift + track:** no stock meaning in view 4. With Shift held, the mask `0x07DFFF00` (literal 0x0803E444) discards
  ids 0–7. The existing hook `b.w` at **0x0803D956** (`fm_shift_button`) is reached in every view except 9.
- **Detecting the Sample Rec view** (router from the hooks: r4 in 0x0803D8B8, r0 in 0x0803D7FE):
  `*(void**)(router+0x7C) == *(void**)(router+0x58)`, or byte 0 of the `router+0x14` property == 4. These are plain memory
  reads. Page: `*(uint8_t*)(*(uint8_t**)(router+0x58) + 0xC)`.

### 3.4 Hook points for the two gestures
| Gesture | Hook | Action |
|---|---|---|
| Shift + track 1–8 → generate + audition | existing **0x0803D956** → `fm_shift_button` (ids 0–7) | if view 4: request generation for family `id+1`; exit via 0x0803DC94 (held bit stays set) |
| Track press without Shift in view 4 | **new `b.w` at 0x0803D9A4** (displaces `subs r6,r5,#1` + `cmp r6,#6`, 2+2 bytes) | if view 4 and a generated sound is pending for track `id`: exit to **0x0803DC94** (no navigation; `router+0xA0` bit stays set until release). Otherwise re-run the two instructions (flags matter) and continue at 0x0803D9A8 (`bhi`) |
| Track held + pad → store | existing pad hook **0x0803D7FE** | if view 4, pad < 16, and bit `id` set in `router+0xA0` for a track with a pending sound: slot = pad + 16·page → start commit; return without calling the view (no select/audition) |
| Release | none needed | `0x0803DCC8` clears the bit. `router+0x19E+id` is 0xFF because navigation was skipped, and ids 0–7 have no release action (`0x0803DD40`: id−0x10 > 8) |

Keep the pad-numbering caveat from the handoff: physical pad order is not hardware-confirmed (**U**). The view code
treats pads 0..15 as rows 1–2.

---------------------------------------------------------------------------------------------------------------
## 4. Audition output (Q4)

- **Core-1 P:$03C1 (audition player) writes Y:$306F0..$3070F every block.** When `Y:$371EA ≥ $80` it jumps to $0415:
  `clr b; dor x:$1 → move b,y:(r5)+` (32 zeros), then sets $371EA = $7F and $371EB = 0. When playing, $0422 copies
  page data and $045D zero-fills the remainder. Core-0 $1A11–$1A25 adds $306F0 to both master channels. **An ARM
  write to $306F0 is overwritten within one block and bypasses track level/pan.** **V** (disassembly; idle cost 350
  cycles measured earlier)
  - Side note: the idle value $7F is a valid slot number. Idle stays silent only because the remaining-length logic hits
    $045D. **Do not use slot 127 as a scratch/preview slot** (**I**).
- **Adding into a track's active span after $0221** (Y:$304F0 + 64t + 32·bank) is mixed by core-0 P:$0367–$03AE with the
  per-track gain buffers X:$DE+64t / $2DE / $4DE (main / two sends). That is the Rhythm FM consumer path ($0D40 → `jsr
  $0221` → add). On hardware it carries FM audio through the track's level, pan, mixer view and FX (handoff 0.1.9 /
  0.1.17). **V (hardware).** If the stock sampler voice on that track is also sounding, the two add.
- Stock audition of a *stored* slot: `0x080407F0(slot, loopFlag, gateFlag)` / stop `0x08040808()` (locked SPI words,
  so call them outside the mutex). It plays on the master via $306F0, not through a track.

**Recommendation for pre-commit audition:** keep a one-track version of the consumer/FIFO and stream the poundhard
render through it (no flash, no DSP sample memory, heard with that track's level/pan/FX). There is no free slot index
for a preview buffer: the page tables cover exactly slots 0..127, and all 128 are user-visible.

---------------------------------------------------------------------------------------------------------------
## 5. Risks (Q5)

- **Storage full:** op 7 returns 2/3 and op 8 may return 6. Op 2 on `{1, 0, curPack}` (**I**: path form) gives the pack
  budget left (0x01E14000 − usage). Check it before deleting. DSP memory: check `0x0804A4F4()` (§2.3). On exhaustion the
  loader keeps a truncated slot. If the delete succeeds and the write fails, the slot is left empty: the stock code has
  the same behaviour.
- **Task context:** synchronous ops yield-wait for the storage task and stall the main task, which runs UI, periodic ticks,
  `fm_service` and the callbacks. The reload (`0x08053E8C`) is synchronous: ≈94 × (2 KB read + swap + SPI) for 2 s of
  audio. The stock code does the same after uploads and record. Never call any of this from `fm_service` (DSP mutex
  held, not recursive: `0x08029C8C` yield-spins → deadlock).
- **Concurrency:** one storage queue and **one shared file object** (`[0x20003748+8]`). Every op re-opens its path, but
  whether opening another file in between flushes ours is **U**. Mitigation: write the chunks back-to-back and close
  promptly. The scratch is shared with SysEx and the recorder (use the semaphore). The loader ignores the semaphore, so do
  not hold the scratch across the reload. The reload frees slot N's pages while a track may be playing slot N: there may be
  a glitch, as on a stock upload. Stopping that track's voice first is optional (**U**: needed or not).
- **Flash wear:** QSPI NOR, 4 KB erase blocks. A 2 s sample is ≈193 KB, about 48 sectors plus FAT and directory
  sectors per file op. Avoid rewriting: render once to learn the length, then write the header and the first 4052 data
  bytes as chunk 0, so that later chunks are 4 KB-aligned (the recorder's header-last scheme rewrites sector 0). Never
  audition through flash.
- **Pack/project:** samples belong to the **pack** (128 slots, Sample Rec pages 0–7). Projects reference slots only
  by number, so replacing slot N changes slot N in every project of that pack. Files are written straight to the
  current pack on the internal QSPI volume and survive power-off, like Sample Rec results; there is no save step.
  Whether and when the pack's SD-card copy is updated is **U**. Loading a project does not reload samples. A pack change
  reloads all 128 (`0x08039C70` → `0x08053E24`).
- Also: a bad WAV leaves the "filled" bit set but the slot silent (§1.5). The file must be ≥ 257 samples.

---------------------------------------------------------------------------------------------------------------
## 6. Recommended implementation: generate → audition → commit to slot N

State lives in extension RAM. All storage and DSP work happens in the main task, outside `fm_service`.

1. **Gesture A (Shift + track t, view 4)**, in the existing 0x0803D956 hook: choose a seed and the family (t+1);
   `ph_family/ph_design` → patch. Store `{family, seed, patch}` as track t's pending sound. Start the audition: set a
   flag that `fm_service` turns into a real-time render through the consumer FIFO on track t (no allocation, no I/O).
   While auditioning, do a **measuring pass**: render until the envelope is below a threshold (e.g. −60 dBFS for
   10 ms) or 96,000 samples, and record the length n and the peak (the normalisation gain). poundhard is deterministic
   (`ph_strike` + `ph_render` per block), so the sound can be re-rendered bit-exactly later.
2. **Gesture B (track t held + pad p < 16, view 4)**, in the 0x0803D7FE hook: slot N = p + 16·page; reject if no sound
   is pending for t or a commit is busy. Checks: DSP free pages (+ old slot pages) ≥ ⌊(n+256)/4096⌋+1. Build
   `path = {4, N, 0xFE}`. Consume the pad.
3. **Commit state machine** (each step issued from the previous callback; callbacks run in the main task):
   1. `0x08046098(&path, cb, ctx)` delete → cb (0 or 5 = OK).
   2. `buf = 0x0802EF6A(tag)` (4 KB scratch). Chunk 0: 44-byte header (RIFF size = 36 + 2·(n+256), fmt 16, PCM,
      mono, 48000, 96000, 2, 16, data = 2·(n+256)) + 2026 samples. Re-render with the same patch and seed, convert to
      int16 with the gain from step 1. `0x0804607A(&path, 0, buf, 4096, cb, ctx)`.
   3. In each cb: check result == 0 and b == len; render the next 2048 samples (the last chunk carries the 256-zero
      tail); write at offset 4096·k. Keep the scratch held between chunks, as the recorder does.
   4. After the last chunk: `0x0802EF98()` releases the scratch; `0x080460C8(&path, cb, ctx)` closes.
   5. Close cb: `0x08035D62(*(void**)0x20006EAC, &(path){4, N, **(uint8_t**)0x20006E24})` → bitmap + free + reload.
      Optionally `0x080407F0(N, 0, 0)` to audition the stored slot. Clear the pending sound.
   - On any error: release the scratch, abandon the commit, and optionally flash a pad. The slot may then be empty.
4. **Do not** call the loader, `0x08031228`-style locked helpers, `0x0802EF6A` or synchronous storage ops from
   `fm_service`.

### 6.1 Stock functions to call
| Function | Address | Arguments | Context |
|---|---|---|---|
| delete file | `0x08046098` | `(path*, cb, ctx)` | async (cb≠0) |
| write | `0x0804607A` | `(path*, offset, buf, len, cb, ctx)` | async; buf in DTCM/SRAM, not ITCM |
| close | `0x080460C8` | `(path*, cb, ctx)` | async |
| exists | `0x08045FB4` / `0x0804736C(x, path*)` | `(path*)` | sync |
| free space | `0x08045FDE` | `(path*)` → u64 | sync (**I** for the pack-path form) |
| scratch acquire / release | `0x0802EF6A(tag)` / `0x0802EF98()` | — | main task only |
| file-changed event → reload | `0x08035D62` | `(*(void**)0x20006EAC, path*{4,N,curPack})` | main task, outside the mutex |
| alt. notify (stock delegate) | `0x08024B68` | `((void*)0x20000A40, 0, path*)` | same |
| direct reload (no bitmap) | `0x0804A4A8(N)` then `0x080460B8(N)` | slot | same |
| DSP free pages | `0x0804A4F4()` | — | locked SPI read |
| slot length | `0x0804A4E4(N)` | slot | locked SPI read |
| audition stored slot | `0x080407F0(N, a, b)` / `0x08040808()` | slot | locked SPI write |
| current pack | `**(uint8_t**)0x20006E24` | — | memory read |

### 6.2 Data addresses used
`0x20006EAC` content manager pointer; `0x20000A40` file-changed delegate; `0x20006E24` current-pack pointer;
`0x20003F94` / `0x20003F8C` scratch / semaphore; `0x20006AD8` loader header buffer (do not use while a load may run);
`router+0x14/+0x58/+0x7C/+0xA0` (§3).

---------------------------------------------------------------------------------------------------------------
## 7. Open questions needing a hardware check
1. Does the `0x08035D62` emit reload the slot at once, and does the Sample Rec / Sample view show the slot lit? Check
   by playing slot N on a track right after the commit.
2. Physical pad → index mapping in the Sample Rec view (pads 0–15 = rows 1–2?), and the page buttons (0x13/0x14 =
   up/down?).
3. Does suppressing the track-button press at 0x0803D9A4 in view 4 leave any LED or held-state artefact after release?
4. Commit duration (erase/program time for ≈48 sectors) and UI responsiveness during the reload. Does the sequencer keep
   time while the main task is blocked in `0x08053E8C`?
5. Is the SD-card copy of the current pack updated (on pack switch or shutdown), or does the sample live only in internal
   flash?
6. Is there a glitch when slot N is reloaded while a track is playing it?
7. Shared file object: is an interleaved file op from another component (autosave, SysEx) safe between our chunk writes?
8. Meaning of the Sample Rec view pads 16–31 and of the encoder method `0x0804FD68` (input source, threshold, trim?).

### Correction to the removal map
"Slicing candidate" vt 0x0806876C (ctor 0x0803A1B0) is the **post-record trim model**: fields 1/2/3/4 = start, end, length,
slot. It issues `0x0804A4B8` (DSP-fs +0x2C → $372FD) and `0x0804A4CE` (+0x30 → $372FE trim/rotate). It listens to
the record-slot property and the slot bitmaps. `0x20000A8C` (cleared by `0x08053E24`) is an object with a vtable
(progress display, **I**), not a slot-length table.

---------------------------------------------------------------------------------------------------------------
## 8. Follow-up (2026-10-17): the 0.2.1 commit gesture, release hook, pads 22/23, pad LEDs, two-pad mode UI

Context: in 0.2.1 on hardware, Shift + track auditioning works, but holding a track button and pressing a pad in the
Sample Rec view does nothing. Research only, as before.

### 8.1 Input path, from hardware to the router — V
- The panel object (vptr 0x080665E4) has an input method at +0x14 = `0x0803F7A8` → `0x0803EE18(wrapper, hwId, value)`.
  The wrapper is built at 0x0802930C (`0x0803EE00`), and its word 0 = the router.
  - `hwId 0x00–0x1F` → **pads, with pad index = hwId unchanged**: value ≠ 0 → router vt+0x00 (`0x0803D7FE`), value 0 →
    vt+0x04 (`0x0803D896`).
  - `hwId 0x20–0x3B` → button id = `table[hwId−0x20]` at **0x0803EEE0**, press → vt+0x08 (`0x0803D8B8`), release →
    vt+0x0C (`0x0803DDB0`). **hwId 0x2B–0x32 → ids 0–7 (track buttons 1–8)**; 0x2A → 8; 0x20–0x29 → 18–27.
  - 0x3C–0x43 → encoders (vt+0x10, delta = value−0x40); 0x44/0x45, 0x4E/0x4F → other controls.
- There is no remapping and no "button held" layer between hardware and the router. A pad pressed while a track button
  is held reaches `0x0803D7FE` with r0 = router, like any other pad. The decoder looks MIDI-note-like and is reached from
  the main-loop MIDI/port processing (`0x08029ABC`, before the DSP mutex). That context is **S**; it fits the fact that
  the 0.2.1 UI hooks already run storage calls.
- Pad numbering: the Sample Rec view treats pads 0–15 as slots and draws LED i for pad i (`0x0804FB52`), and the
  router's LED map (§8.4) sends view LED i to hardware LED i for i = 0..31. The user's "pad 22 = third row, second from
  right" and "23 = rightmost" match a row-major 8-wide layout. That makes pads 0–15 rows 1–2 and 16–23 row 3
  (**I**, consistent).

### 8.2 Q1: why the 0.2.1 commit gesture probably failed
Stock side, checked step by step (**V**):
- `router+0xA0` really is a **plain bitset of held button ids**: 4 bytes, `+0xA0..+0xA3`, zeroed in the ctor at
  0x0803D578. `+0x9C..+0x9F` is the same for pads. `0x08051B7A(set, id)` = test-and-set
  (`ldrb [set + id>>3]`, bit `id&7`, returns the old bit). `0x08051BB8(set, id)` = clear. Reading a u32 at +0xA0 gives
  bit k = id k for ids 0..27. The press handler sets the bit on its first instruction (0x0803D8BE) and ignores a press
  whose bit was already set. **The bit stays set when our 0x0803D9A4 hook exits** (§8.6). It is cleared only by
  `0x0803DCC8` (release, below). The view-change helpers do not clear it for ids 0–7: `0x0803DFCC` synthesises
  releases for the 32 *pads*; `0x0803DFE4` releases only buttons **outside** mask 0x0664FFFF (ids 0–15 are inside);
  `0x0803E00E` releases all 28 buttons, but only on a change of the property `router+0x20` (0x0803E20C, rare: **U**
  what it is).
- **The view stays 4** while the track press is swallowed, because only `0x0803DEBC` changes the view and it is
  skipped. If the press is *not* swallowed (because `fm_track_press` returned 0, e.g. the pending sound was not valid yet),
  stock briefly switches to the track view. The pad then goes to the track view, and the view flips back on release.
  **If the user saw that flip, `pending[t].valid` was 0 at press time.**
- **Order inside the router's pad handler:** our hook replaces the first instruction at 0x0803D7FE, so `fm_pad` runs
  **before** everything stock does: the `+0x8C` "restricted" check, the overlay cancel (`router+0x34`: a 1–2 s full-grid
  flash pattern; a pad press first only cancels it, 0x0803D816–0x0803D822), the held-pad test-and-set at `+0x9C`
  (0x0803D82C), `router+0x1BA = 1` ("pad pressed during a hold", 0x0803D834), and the dispatch to `router[0x7C]`. None
  of that can hide the press from `fm_pad`.
- Shift: `router+0x0C` is the Shift property (byte 0 of `0x0803FC58(*(router+0x0C), 0)`, as at 0x0803D944). `fm_pad`
  reads it correctly.
- Storage preconditions: op 7's check `0x0802E7CC` is only `index < directory capacity` (vt+0xC of the index),
  **not** "file exists", so writing right after a delete is valid. Delete of a missing file returns 5 (handled).

Conclusion: the stock path carries the gesture exactly as 0.2.1 assumes, so **the failure is most likely in the
extension's state, not in routing**. Candidates, in order of likelihood:
1. `commit_state` stuck at a non-idle value from an earlier attempt. `commit_start` then returns 0 and `fm_pad` still
   `return`s, so the pad does nothing at all. Every later attempt is then swallowed silently.
2. The commit started but failed (delete/write/close result). `commit_fail` gives no audible or visible feedback.
3. `pending[t].valid` was 0 when the track was pressed: see the view-flip symptom above.
4. Shift still held when the pad went down: `fm_pad` requires `!shift` and passes the press to stock.

Check report v15: `pad_events`/`last_pad_press` (bit 8 = Shift at the press), `commits`, `commitErrors` and
`lastCommit` (slot | track<<8 | **state<<16** | error<<24). Make `fm_pad` consume the pad only when `commit_start`
returned 1, and light a pad or count a "busy" field when it refuses.

### 8.3 Q2: a release hook for ids 0..7 — V
`0x0803DCC8(router, id)` is the single release routine. It is reached from vt+0x0C `0x0803DDB0` (hardware release)
**and** from the synthetic releases `0x0803DFE4`/`0x0803E00E`. It clears the `+0xA0` bit, then handles a momentary
view return via `router+0x19E+id`, Shift (0x1B), view-9 forwarding, and the per-id release actions at 0x0803DD40, where
ids 0–7 have none (id−0x10 > 8 → `bhi` → return).

| Option | Site | Displaced | Continue | Notes |
|---|---|---|---|---|
| **A (recommended)** | `b.w` at **0x0803DCC8** | `movs r2,#0` (`00 22`) + `push {r4,r5,r6,lr}` (`70 B5`) | **0x0803DCCC** (`mov r4,r1`) | sees every release, synthetic ones included. Trampoline: `push {r0-r3,r12,lr}; bl fm_release(r0=router, r1=id); pop {r0-r3,r12,lr}; movs r2,#0; push {r4,r5,r6,lr}; b.w 0x0803DCCC` |
| B | vtable word **0x080672F0** (now 0x0803DDB1) → `fm_button_release(router,id)` that tail-calls 0x0803DDB1 | none | — | hardware releases only. No direct `bl` to 0x0803DDB0 exists (only the vtable) |
| C | `b.w` at 0x0803DDB0 | `push {r3,r4,r5,lr}` (`38 B5`) + `mov r4,r0` (`04 46`) | 0x0803DDB4 (`mov r5,r1`) | same coverage as B |

`fm_release` can be called for any id, even one whose bit is not set (stock tests the bit right after). Filter
`id < 8`. The stock bit at `router+0xA0` is already a reliable "held now" state, so the hook is only needed for an
explicit release action (e.g. ending an audition on release).

### 8.4 Q4: how the Sample Rec view's pads are lit — V
- **Buffer:** each view/sub-object has an LED object at `obj+4` (made by `0x080520D8(obj, n)`: new 4 B →
  `0x0805198C`). **`*(uint16_t **)(*(obj+4))` = array of n u16 colour words.** Views use n = 0x2A (42): indices 0..31 =
  pads, 32..41 = button LEDs. The Sample Rec view writes 32/33 (page arrows, `0x0804FC3A`) and 35/36 (`0x0804FC6C`).
  Sub-objects use n = 8 or 16.
- **Colour word** = index into an RGB table **in RAM at 0x20003974** (u32 0x00RRGGBB per entry, as read from
  `rhythm-startup-ram.bin`). The views take their indices from the u16 palette at 0x08062AB0 (`palette[k]`,
  `0x080555C0(led, i, k)` = `buf[i] = palette[k]`). Measured RGB: `0x00` off; `0x01` white;
  `0x09` dim grey; `0x22` green 0x00FF00; `0x55` red 0xFF0000; `0x56` dark red; `0x6D` yellow 0xFFFF66 (the stock
  "toggle on" colour, palette 0x22B); `0x73` dim yellow 0x292910 (stock "toggle off", palette 0x22C); `0x18` cyan
  0x44DDFF; `0x2B` orange 0xFFAF2F. Indices 0xD2–0xE0 are remapped (track colours) by the output stage (0x0803E8FE).
  Treat the channel order as **I**.
- **Who draws when:**
  - Pads 0–15: written on events only, by `0x0804FB52` (pad press, property listeners at 0x0804FDDA).
  - **Pads 16–31: rebuilt on every frame** by the view's **getFrame, vt+0x1C = 0x0804FE9A** (vtable word at
    **0x08067A24**). While recording (state 2/3) it copies 16 words from `view+0x10`. Otherwise pads 16–23 come from
    `view+0x1C` if `0x0804FF98` is true, else from `view+0x14`. Pads 24–31 come from `view+0x18` (plus 2 bytes from it to
    byte offset 0x52). It returns the buffer pointer.
  - The router's LED refresh callback (registered at 0x08029534 → `0x080295A8` → `0x0803E47A`, periodic) calls
    **`0x0803E80C(router, out)`**. That gets frames by `vt+0x1C` from `router[0x7C]` (current view) and 8 other objects,
    then builds the 69 hardware LEDs through the map at 0x08066744 (`{source, index}`; source 0 = current view, entries
    0..31 = pads 0..31 identity, 51/52/60–67 = view entries 32–41). The flash overlay `router+0x34` is drawn on top when
    active. The colour index → RGB conversion happens at `0x0803E8E6`.
- **Minimal way to light pads 22/23:** **patch the vtable word 0x08067A24** (0x0804FE9B) to point at
  `fm_samplerec_frame` (Thumb bit set):
  ```c
  uint16_t *fm_samplerec_frame(void *view) {
      uint16_t *f = ((uint16_t *(*)(void *))0x0804fe9bu)(view);   /* stock compose */
      if (gen_mode)    f[22] = 0x22;   /* green */
      if (commit_mode) f[23] = 0x55;   /* red   */
      return f;
  }
  ```
  No dirty or refresh call is needed: the frame is pulled again on every LED refresh, and stock rebuilds pads 16–31 just
  before our override, so turning a mode off restores the stock colour by itself. There are no direct `bl` calls to
  0x0804FE9A (only this vtable slot and the `0x08052108` thunk). If the builder can only place `b.w` hooks: hook
  0x0804FE9A, displacing `push {r3,r4,r5,lr}` (`38 B5`) + `mov r4,r0` (`04 46`), continue at **0x0804FE9E**. The wrapper
  must then call a trampoline copy of the original (re-run the two instructions, `b.w 0x0804FE9E`) and modify the result
  after it returns. The vtable patch is simpler.
  - Pads 0–15 are event-drawn, so overriding them would need a redraw trigger. Avoid that: keep mode LEDs on 22/23.
  - Without our override, pads 21–23 are dark (sub-object `view+0x1C` idx 5–7 draws colour 0). Under `view+0x14` their
    content is **U**, but it is overwritten anyway.

### 8.5 Q3: what stock does with pads 22 and 23 in view 4 — V
- View pad method `0x0804F9D8`: pads 24–31 → `view+0x18` vt+0 (pad−24), refused while recording. Pads 16–23 →
  **only if `0x0804FF98(view)`** → `view+0x1C` vt+0 (pad−16). `0x0804FF98` = *the selected record slot is empty*
  (bit clear in the `view+0x30` bitmap) **and** the Sample Rec state `view+0x48` ≠ 2 (not recording). Otherwise pads
  16–23 do nothing.
- Sub-object `view+0x1C`: class vt 0x080681AC, ctor `0x08059684` (8-entry LED buffer, property `+0xC`, listener `+0x10`),
  pad method **`0x080596C0`**:
  - idx 0 (pad 16): toggles the property between 0xFF (off) and 0, and posts a message.
  - idx 1–4 (pads 17–20): set it to 0..3, for any value except 0xFF.
  - **idx 5–7 (pads 21–23): no state change.** When the property ≠ 0xFF the method only re-notifies it
    (`0x0803FBEC`, 0x08059776), which triggers the sub-object redraw `0x08059780`.
  - Colours: idx 0 = palette 0x22B/0x22C (on/off), idx 1–4 = 0x229 (selected)/0x22A, idx 5–7 = 0 (off).
  - This is probably the "threshold record" arm toggle + four levels (it is shown only for an empty, not-recording slot;
    **I**).
- **Consuming presses of 22/23 at our hook is safe.** Stock would at most re-notify a property. Because our hook runs
  before the router's held-pad test-and-set (0x0803D82C), the `+0x9C` bit is never set for a consumed pad. The router's
  pad release (`0x0803D896` → `0x0803D85A`) then sees the bit clear and **does nothing**, so no release hook is needed
  for 22/23 or for consumed slot pads. Do not consume pads 16–20 (stock threshold UI).

### 8.6 Q5: the swallow site 0x0803D9A4 — V
Path for a track press without Shift in view 4:
1. `0x0803D8BE` test-and-sets `+0xA0`.
2. Overlay cancel.
3. `+0x8C` check: ids 0–15 are in the allowed mask 0x0664FFFF.
4. The view is not 9.
5. `0x0804ED60(router[0x48])` (clears a flag property; harmless).
6. Shift clear → 0x0803D962 `cmp r5,#0x1b` → tbh (table 0x0803D96C) → ids 0..7 → **0x0803D9A4**.

At 0x0803D9A4 the bytes are `6E 1E` `subs r6,r5,#1` + `06 2E` `cmp r6,#6`. Continue at 0x0803D9A8 (`13 D8` `bhi`) with the
flags from that `cmp`, which the existing trampoline restores correctly.

Exiting to **0x0803DC94** (`pop {r0,r4,r5,r6,r7,pc}`, matching the handler's `push {r3-r7,lr}`) is consistent:
- The `+0xA0` bit stays set until release.
- `router+0x19E+id` stays 0xFF (only `0x0803DEBC` writes it), so the release does no view return.
- `+0x1BA` is untouched.
- No track selection happens (`0x0804D662` is skipped), so the "current track" does not change. That is wanted.

This is already the best place: the earliest point where the id is known to be a non-Shift track press after stock's
own checks. A vtable wrapper on 0x080672EC (0x0803D8B9) would run before the test-and-set and would have to
duplicate it.

### 8.7 Recommended implementation of the two-pad mode UI (view 4 only)
State: `gen_mode`, `commit_mode` (recommended mutually exclusive; turning one on turns the other off), and `cur[t]`
(the sound per track: seed/family/measure). Clear both modes when the view is not 4 at any hook entry (or leave them
latched; this is a UI choice).

| Event | Hook | Behaviour |
|---|---|---|
| Pad 22 press, view 4 | `fm_pad` (0x0803D7FE) | toggle `gen_mode` (commit off); consume (no `rhythm_pad_original`) |
| Pad 23 press, view 4 | `fm_pad` | toggle `commit_mode` (gen off); consume |
| Pad 0–15 press, view 4, `commit_mode`, a track t held (`router+0xA0` bit t) with `cur[t]` valid | `fm_pad` | `commit_start(t, pad + 16·page)`; consume **only if it returned 1**; on refusal, flash pad 23 (e.g. set a 300 ms "error" colour in the frame hook) |
| Other pads | `fm_pad` | stock |
| Track t press, no Shift, view 4, `gen_mode` | 0x0803D9A4 hook | request a new sound for t + audition; swallow (0x0803DC94) |
| Track t press, no Shift, view 4, `commit_mode` | 0x0803D9A4 hook | re-audition `cur[t]` (same seed); swallow |
| Track t press, no Shift, no mode | 0x0803D9A4 hook | stock (track select + momentary view) |
| Shift + track t, view 4, `gen_mode` | 0x0803D956 hook | re-audition `cur[t]`; exit 0x0803DC94 |
| Shift + track t, view 4, no mode / commit mode | 0x0803D956 hook | as 0.2.1 (generate), or nothing; a product choice |
| Track release | optional 0x0803DCC8 hook | e.g. stop the audition; otherwise the `+0xA0` bit suffices |
| LEDs | vtable word **0x08067A24** → `fm_samplerec_frame` | after stock compose: `f[22]` = green (0x22) while `gen_mode`, `f[23]` = red (0x55) while `commit_mode`, optional blink/flash for busy/error |

Notes:
- Recording does not have to be armed. None of these paths touch the Sample Rec controller. A commit while stock is
  recording (Sample Rec state 2/3, readable from `view+0x48` byte 0) should be refused: the recorder holds the scratch
  and writes its own file.
- In `commit_mode`, the stock "select + audition" of a slot pad still happens when no track is held. That is useful for
  choosing the destination.
- `fm_pad` already runs before every stock pad check, so the new pads need no further hooks. Only add the vtable-word
  patch (0x08067A24), and optionally the release hook.
