# Circuit Rhythm 5706 Grid FX: research map (2026-10-17)

Built from the firmware alone (no hardware capture). It is the basis of Rhythm FM 0.3.0's per-step, per-track Grid FX
locks.

## View and pads

- **View:** the FX button's second page is router view 7. It shares the FX view object `router[+0x6C]` (vt
  `0x08067910`) with view 2.
- **Pads 16–31:** they go to a sub-object (vt `0x08068024`, ctor `0x0805A630`, at `view+0x15C` in state 7).
- **Press `0x0805A70C`:**
  - Every pad has an effect type, from slot store index 0 (`0x0805F52C`); types ≥ 7 are ignored.
  - Within one type only one pad is on: the previous pad of that type is switched off.
  - It then posts `(pad, on)` through `0x08047038` (`0x0805AB78`).
- **Release `0x0805A766`:** posts `(pad, off)` through `0x0805AC46`.

## Engine (listener vt at `0x08063A4C`; `0x080393FC`…)

- **Slots:** 16 × 7 bytes `[type, settings…]`, from the pack's `_GRIDFX.ncg`. Factory defaults are in RAM at
  `0x20003904` (see the table below).
- **Pad on (`0x08039770`):** for each of the type's settings, setter(first id + k, slot byte k), then setter(on id, 1).
  Setter ids are `0xA9 + (param − 0x0A)` in global group `0x20`.

| Type | Effect | On param | Settings (count) | DSP writer | DSP params | Factory pads |
|---|---|---|---|---|---|---|
| 0 | looper | `0x0A` | `0x0B` length 0–8, `0x0C` level (inverted) | `0x08030988` | `X:$D4` = (127−lvl)<<16 \| (code+1)<<8, codes {0,1,2,3,4,6,7,9,12} | 6 (7,5,3,1,4,2) |
| 1 | reverser | `0x0D` | `0x0E` length 0–3 | `0x080309DA` | `X:$D6`, codes {3,6,9,12} | 2 (3,1) |
| 2 | gater ("stutter") | `0x0F` | `0x10` rate index | `0x08030A10` | `X:$D0`, `Y:$116` via rate table `0x08030E84` | 4 (7,5,3,1) |
| 3 | phaser | `0x11` | `0x12`–`0x17` (6) | `0x08030A56` | `Y:$120`–`$126`; LFO + 4 all-pass stages (`P:$148B`) | — |
| 4 | bit crusher | `0x18` | `0x19` bit reduction (0 off), `0x1A` sample hold | `0x08030B0E` | `Y:$11D`–`$11F` (`P:$4E4`/`$504`) | — |
| 5 | auto-filter | `0x1B` | `0x1C` rate, `0x1D`, `0x1E`, `0x1F` | `0x08030B96` | `Y:$117`–`$11C` (`P:$196F`) | 2 (10,75,80,63 / 8,82,114,43) |
| 6 | vinyl | `0x20` | `0x21`–`0x24` | `0x08030C18` | `Y:$10F`–`$112` (noise LFSR `0x75AA55`, `P:$1353…`) | 2 (63,80,65,65 / 105,127,127,127) |

Rates are tempo-synced indices that the DSP resolves itself (release notes: "Gater phase is now synced with the
sequence").

## DSP core-0 signal chain (per block)

1. **Mix (`$367`):** tracks are mixed (`P:$37F`, three stereo buses per track: main `$30430`, delay send `$30470`,
   reverb send `$304B0`).
2. **Master build (`$1A11`):** master `X:$4AD8` = input monitor + main bus + audition `$306F0`.
3. **Sends (`$597`):** reverb/delay; their returns are added to `$4AD8`.
4. **Grid FX:**
   - the looper (`$166B`) runs on `$4AD8`;
   - `$4AD8` is copied to `$4B18`;
   - reverser `$17F8`, `$1260`, gater `$12A2`, `$1448`, phaser `$148B`, bit crusher, `$1933`, auto-filter `$196F` and
     vinyl `$1372/$13BB/$1302` run on `$4B18`.
5. **Master dynamics:** group `0x21` `$1A26`.
6. **Output.**

So stock Grid FX always process the whole mix. There is no per-track Grid FX routing (per-track param `0x14` is the
choke group).

## FX bus patch (0.3.0)

See `tools/rhythm_fm_fxbus.py`. Core-0 free program space is only the unused interrupt vectors (`$00`, `$3E` and `$44`
are used; `P:$46–$FF` are zero). The image is loaded flat from `P:0`.
