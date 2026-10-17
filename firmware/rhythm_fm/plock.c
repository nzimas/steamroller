/* SPDX-License-Identifier: GPL-3.0-only
 * Elektron-style parameter locks on the stock Rhythm automation lanes.
 * See docs/RHYTHM-AUTOMATION-PLOCKS.md for the stock structures.
 *
 * Stock keeps one byte per micro-step (6 per step) in 12 lanes per pattern
 * (8 macros, 4 mixer/FX groups); 0xFF = no value. Its player sends a value
 * and the parameter then HOLDS it until the lane has another value. Here the
 * byte at micro-step 0 of a step is that step's lock: it is sent when the step
 * starts and lasts for the whole sound it plays: steps without a hit (no
 * trigger, step byte 2) keep it, and the next HIT without a lock returns the
 * parameter to its base (the knob's value, read from the knob router's live
 * mirror just before the first lock), 0.2.9. A lock belongs to its step's
 * sound: on a step without a trigger it never plays, and lock_sweep erases
 * it (a removed step loses its locks, 0.2.10). The next hit's locks are sent
 * a sixth of a step early, so they are in place for its transient (0.2.10).
 * Locks are the stock lane data, so they save with the project.
 *
 * Hooks (tools/build_rhythm_fm.py):
 *   0x08055324 lane playback           -> plock_tick (replaces it)
 *   0x080552F6 player reset            -> plock_reset_hook (revert, then stock)
 *   0x0805518C player transport clear  -> plock_stop_hook (revert, then stock)
 *   0x0802854E after the router ctor   -> plock_router_capture
 *   0x08038E90 knob router vt0         -> plock_knob (live turns of macros 1-4
 *                                         on tracks 1-6 go to the mixer/FX)
 *   0x0805CE84 step preview lanes      -> plock_preview (a held step's preview
 *                                         plays its locks, then restores the knobs)
 * In place: 0x08049544 (no interpolation fill when recording) and 0x0804982C
 * (recording writes micro-step 0 only, and never overwrites a lock with 0xFF).
 */
#define PL_U8(p, o) (*(uint8_t *)((uint8_t *)(p) + (o)))
#define PL_U16P(p, o) (*(uint16_t **)((uint8_t *)(p) + (o)))
#define PL_PTR(p, o) (*(void **)((uint8_t *)(p) + (o)))
#define PL_UNSET 0xffffu          /* lane not locked (stock's own reset value) */
#define PL_NOBASE 0xfffeu         /* locked, but the base could not be read */

extern unsigned plock_lanes_original(void *player, unsigned pattern, unsigned step, unsigned tick);
extern unsigned plock_knob_original(void *router, unsigned id, unsigned value, unsigned mode);
extern unsigned plock_preview_original(void *preview, unsigned micro);

#ifndef FM_HOST
static uint8_t *plock_store(void *store, unsigned index) {
    return ((uint8_t *(*)(void *, unsigned))0x0803fc59u)(store, index);
}
#else
extern uint8_t *plock_store(void *store, unsigned index);
#endif

/* The knob router's live value of a parameter id (stock setter 0x08038A08's
   groups), or PL_NOBASE. */
static unsigned plock_live(unsigned id) {
    void *router = S->plock_router;
    if (!router) return PL_NOBASE;
    void **stores = (void **)PL_PTR(router, 0x24);
    unsigned group, index;
    if (id < 0x80u) { group = id >> 4; index = id & 15u; }
    else if (id < 0x88u) { group = 8; index = id - 0x80u; }
    else if (id < 0x90u) { group = 9; index = id - 0x88u; }
    else if (id >= 0x91u && id < 0x99u) { group = 11; index = id - 0x91u; }
    else if (id >= 0x99u && id < 0xa1u) { group = 12; index = id - 0x99u; }
    else return PL_NOBASE;
    if (!stores || !stores[group]) return PL_NOBASE;
    return *plock_store(stores[group], index);
}

/* Tracks 1-6 (FM and drums) do not use macros 1-4 (sound params 6-9): there
   they are the track's mixer/FX parameters, ids 0x80/0x88/0x91/0x99 + track
   (params 18, 19, 16, 17: reverb send, delay send, level, pan; confirmed on
   hardware, level/pan read by DSP core 0 at P:$033E). */
static unsigned plock_map(unsigned id) {
    static const uint8_t mixer[4] = {0x80, 0x88, 0x91, 0x99};
    if (id < 16u * OUT_TRACKS && (id & 15u) < 4u) return mixer[id & 15u] + (id >> 4);
    return id;
}

/* Send one value. A remapped macro (mixer/FX) goes exactly the way a live
   turn does: through the knob router's real setter [router+8], and into the
   router's live store for that mixer id (0x08038A08, which notifies its
   listeners). On hardware, locks sent only through the player's setter
   applied the sends but not level/pan (0.1.17); through [router+8] alone
   still not (0.1.18): the mixer re-sends its stored level/pan. The mixer
   lanes (ids 0x80+: sends, level, pan) therefore go the same way (0.2.7).
   Macros go through the caller's setter unchanged. */
void lock_dsp_now(unsigned id, unsigned value);
static void plock_out(void *own, unsigned id, unsigned value) {
    const unsigned out = plock_map(id);
    void *router = S->plock_router;
    lock_dsp_now(out, value);
    if ((out != id || out >= 0x80u) && router && PL_PTR(router, 0x08)) {
        void *setter = PL_PTR(router, 0x08);
        (**(void (***)(void *, unsigned, unsigned))setter)(setter, out, value);
#ifndef FM_HOST
        ((void (*)(void *, unsigned, unsigned))0x08038a09u)(PL_PTR(router, 0x24), out, value);
#endif
        return;
    }
    (**(void (***)(void *, unsigned, unsigned))own)(own, out, value);
}

/* Send through the player's setter, with stock's companion flags for the
   lane ids base+1/base+2 (0x080554F6); a remapped macro has none. */
static void plock_send(void *player, unsigned lane, unsigned value) {
    uint16_t *last = PL_U16P(player, 0x4c);
    const uint16_t *ids = PL_U16P(player, 0x40);
    const unsigned base = *(uint16_t *)((uint8_t *)player + 0x44), id = ids[lane], out = plock_map(id);
    void *setter = PL_PTR(player, 0x2c);
    void (*set)(void *, unsigned, unsigned) = **(void (***)(void *, unsigned, unsigned))setter;
    if (last[lane] == value) return;
    plock_out(setter, id, value);
    if (out == id && id == base + 1u) set(setter, base + 0xeu, value == 0x7fu ? 0x7fu : 0);
    else if (out == id && id == base + 2u) set(setter, base + 0xfu, value == 0x7fu ? 0x7fu : 0);
    last[lane] = (uint16_t)value;
}

/* Remember each track's player (spice.c finds a track's pattern through it):
   macro 1's id is 16 * track. */
static void plock_seen(void *player) {
    const unsigned id = PL_U16P(player, 0x40)[0];
    if (id < 0x80u) S->players[id >> 4 & 7u] = player;
}

/* Return every locked lane of a player to its base. */
static __attribute__((noinline)) void plock_revert_lanes(void *player) {
    uint16_t *held = PL_U16P(player, 0x48), *last = PL_U16P(player, 0x4c);
    const unsigned count = PL_U8(player, 0x3c);
    for (unsigned lane = 0; lane < count && lane < 12u; lane++) {
        const unsigned base = held[lane];
        if (base == PL_UNSET) continue;
        held[lane] = PL_UNSET;
        if (base < 0x100u) { last[lane] = PL_UNSET; plock_send(player, lane, base); S->plock_reverts++; }
    }
}
void gfx_hit(void *player, unsigned track, unsigned pattern, unsigned step);
void gfx_release_track(unsigned track);
void plock_revert_all(void *player) {
    if (S->magic == MAGIC) { plock_seen(player); plock_revert_lanes(player); gfx_release_track(PL_U16P(player, 0x40)[0] >> 4 & 7u); }
}

/* Step preview (pressing a step while stopped plays it for ~300 ms; stock
   0x0805CE84 is called with micro-steps 0..5 and sends every lane value, which
   then stays). Here micro-step 0 sends the step's locks, remembering each
   knob's value, and micro-step 5 restores them. [+0x0C] -> lanes at [+0x18],
   [+0x18] store index 1 = step, [+0x50] setter, [+0x54] lane ids. */
static void plock_preview_restore(void) {
    void *preview = S->plock_preview;
    if (!preview) return;
    const uint16_t *ids = PL_U16P(preview, 0x54);
    for (unsigned lane = 0; lane < 12u; lane++) {
        const unsigned base = S->plock_preview_base[lane];
        if (base < 0x100u) { plock_out(PL_PTR(preview, 0x50), ids[lane], base); S->plock_reverts++; }
        S->plock_preview_base[lane] = PL_UNSET;
    }
    S->plock_preview = 0;
}
unsigned plock_preview(void *preview, unsigned micro) {
    if (S->magic != MAGIC) return plock_preview_original(preview, micro);
    if (micro == 0) {
        plock_preview_restore();
        const uint8_t *lanes = (const uint8_t *)PL_PTR(PL_PTR(preview, 0x0c), 0x18);
        const unsigned step = *plock_store(PL_PTR(preview, 0x18), 1);
        const uint16_t *ids = PL_U16P(preview, 0x54);
        for (unsigned lane = 0; lane < 12u; lane++) {
            const unsigned value = lanes[192u * lane + 6u * step];
            S->plock_preview_base[lane] = PL_UNSET;
            if (value == 0xffu) continue;
            if (lane >= 8u && lane < 12u && plock_map(ids[lane - 8u]) != ids[lane - 8u]) continue;   /* as in plock_tick */
            S->plock_preview_base[lane] = (uint16_t)plock_live(plock_map(ids[lane]));
            S->plock_preview = preview;
            plock_out(PL_PTR(preview, 0x50), ids[lane], value);
        }
    } else if (micro >= 5u) plock_preview_restore();
    return 0;
}

/* Replaces stock 0x08055324(player, pattern, step, tick), called every tick. */
unsigned plock_tick(void *player, unsigned pattern, unsigned step, unsigned tick) {
    if (S->magic != MAGIC) return plock_lanes_original(player, pattern, step, tick);
    uint8_t *(*lane_at)(void *, unsigned, unsigned) = (*(uint8_t *(***)(void *, unsigned, unsigned))player)[5];
    /* the pattern (base = lane 0 - 0x128): step byte 2 = micro-step triggers, +0x100 end, +0x101 start */
    const uint8_t *pat = lane_at(player, pattern, 0) - 0x128;
    if (tick != 0) {
        /* The next hit's locks (and a revert before an unlocked hit) go out
           on the last tick of the step before it, so they are in the DSP
           before the note (tick 0 then repeats them; nothing is sent twice).
           Every value is also written to the DSP at once (lock_dsp_now). */
        static const uint8_t tps[8] = {2, 3, 4, 6, 8, 12, 16, 24};   /* ticks per step, table 0x0806651C */
        const unsigned n = tps[*plock_store(PL_PTR(player, 0x10), 1) & 7u];
        if (tick != n - (n >= 24u ? 2u : 1u)) return 0;   /* >= ~5 ms ahead: the DSP's gain glide settles (~1 ms, see build) */
        step = step == (pat[0x100] & 31u) ? pat[0x101] & 31u : (step + 1u) & 31u;
        if (!(pat[8u * step + 2u] & 0x3fu)) return 0;   /* no sound starts there: nothing to prepare */
        S->plock_early++;
    } else plock_preview_restore();       /* a preview cut short must not leave its locks */
    plock_seen(player);
    uint16_t *held = PL_U16P(player, 0x48), *last = PL_U16P(player, 0x4c);
    const unsigned count = PL_U8(player, 0x3c);
    const uint32_t data = *(uint32_t *)plock_store(PL_PTR(player, 0x30), 0);
    const unsigned recording = *plock_store(PL_PTR(player, 0x38), 0);
    const uint32_t touched = *(uint32_t *)plock_store(PL_PTR(player, 0x34), 0);
    const unsigned hit = (pat[8u * step + 2u] & 0x3fu) != 0;
    for (unsigned lane = 0; lane < count && lane < 12u; lane++) {
        if (!recording && (touched >> lane & 1u)) {
            /* the knob is being turned: it wins, and its value becomes the base */
            held[lane] = PL_UNSET; last[lane] = PL_UNSET;
            continue;
        }
        unsigned value = 0xffu;
        if (lane >= 8u && lane < 12u && plock_map(PL_U16P(player, 0x40)[lane - 8u]) != PL_U16P(player, 0x40)[lane - 8u]) {
            /* The mixer/FX lanes (level, pan, sends; recorded from the Mixer and
               FX views) set the same parameters as macros 1-4 on these tracks.
               Old recorded data there overrode macro locks on the same step
               (hardware 0.1.20, FM tracks): only macro locks play. */
            if (data >> lane & 1u) S->plock_mixer_lanes |= 1u << (PL_U16P(player, 0x40)[0] >> 4 & 7u);   /* macro 1 id = 16 * track */
        } else if (hit) value = lane_at(player, pattern, lane)[step * 6u];   /* every lane: spice writes without the editor's has-data mask;
                                                                               a lock belongs to its step's sound: none without a trigger */
        if (value != 0xffu) {
            if (held[lane] == PL_UNSET) held[lane] = (uint16_t)plock_live(plock_map(PL_U16P(player, 0x40)[lane]));
            plock_send(player, lane, value);
            S->plock_locks++;
            S->plock_last = value | lane << 8 | step << 12 | 1u << 20;
        } else if (!hit) {
            /* no sound starts here: the last hit's lock carries on over its tail */
        } else if (held[lane] != PL_UNSET) {
            const unsigned base = held[lane];
            held[lane] = PL_UNSET;
            if (base < 0x100u) { plock_send(player, lane, base); S->plock_reverts++; }
            last[lane] = PL_UNSET;
        } else last[lane] = PL_UNSET;
    }
    if (tick == 0 && hit) gfx_hit(player, PL_U16P(player, 0x40)[0] >> 4 & 7u, pattern, step);   /* Grid FX locks (gridfx.c) */
    return 0;
}

/* Locks on steps without a trigger (the step was removed, or cleared) are
   erased: one track pattern per call (all 64 in 64 calls), on every track
   whose player has been seen. Track base = player [+0x50] (as the stock note
   step 0x0804AEEE): pattern p at +0xA28 p, step s at +8 s, lane l at
   +0x128 + 192 l + 6 s. Only micro-step 0 marks a lock. */
void lock_sweep(void) {
    if (S->magic != MAGIC) return;
    const unsigned i = S->plock_sweep_at++ & 63u;
    void *player = S->players[i >> 3];
    if (!player || !PL_PTR(player, 0x50)) return;
    uint8_t *base = (uint8_t *)PL_PTR(player, 0x50) + 0xa28u * (i & 7u);
    for (unsigned s = 0; s < 32u; s++) {
        if (base[8u * s + 2u] & 0x3fu) continue;
        for (unsigned l = 0; l < 12u; l++) {
            uint8_t *b = base + 0x128u + 192u * l + 6u * s;
            if (*b == 0xffu) continue;
            for (unsigned m = 0; m < 6u; m++) b[m] = 0xffu;
            S->plock_swept++;
        }
    }
}

/* The knob router, saved once when the stock boot constructs it. */
void plock_router_set(void *router) {
    if (S->magic == MAGIC) S->plock_router = router;
}

/* Knob router vt0 (router, id, value, mode). Mode 0 = a live turn: stock
   sends it through the real setter [router+8] and mirrors it into the live
   stores [router+0x24] (0x08038A08). For macros 1-4 on tracks 1-6 the value
   goes to the mixer/FX parameter instead, and is mirrored under both ids, so
   the knob keeps its value and a lock's base is the parameter's real value.
   Mode 1 (a step is held) is stock: the value is that step's lock in the
   macro's lane, played through the same mapping by plock_tick. */
unsigned plock_knob(void *router, unsigned id, unsigned value, unsigned mode) {
    if (S->magic == MAGIC && mode == 0u) {
        const unsigned out = plock_map(id);
        if (out != id) {
#ifndef FM_HOST
            void *setter = PL_PTR(router, 0x08);
            (**(void (***)(void *, unsigned, unsigned))setter)(setter, out, value);
            void (*mirror)(void *, unsigned, unsigned) = (void (*)(void *, unsigned, unsigned))0x08038a09u;
            mirror(PL_PTR(router, 0x24), id, value);
            mirror(PL_PTR(router, 0x24), out, value);
#endif
            S->plock_mixer_edits++;
            return 1;
        }
    }
    return plock_knob_original(router, id, value, mode);
}
