/* SPDX-License-Identifier: GPL-3.0-only
 * Per-step Grid FX locks on one track (Rhythm FM 0.3.0; 0.3.1: crusher presets, stock colours).
 *
 * The stock Grid FX engine (group 0x20 params 0x0A-0x24; FX engine 0x080393FC)
 * has seven effect types, each with an on/off param and its settings:
 *   type 0 looper    on 0x0A, 0x0B length index (0-8), 0x0C level
 *   type 1 reverser  on 0x0D, 0x0E length index (0-3)
 *   type 2 gater     on 0x0F, 0x10 rate index (tempo-synced)
 *   type 3 phaser    on 0x11, 0x12-0x17
 *   type 4 crusher   on 0x18, 0x19 bit reduction index (0 off), 0x1A sample hold
 *   type 5 filter    on 0x1B, 0x1C rate index, 0x1D, 0x1E, 0x1F (auto-filter)
 *   type 6 vinyl     on 0x20, 0x21-0x24
 * Setter ids are 0xA9 + (param - 0x0A) (setter table 0x08063708). A pad of
 * the stock Grid FX view writes its slot's settings, then the type's on param
 * (0x080397C0, 0x08039770); here the same params are set through the knob
 * router's setter, and the type's DSP writer is called at once (as the
 * drain would later), so the effect is in place for the note.
 *
 * Stock runs the Grid FX on the whole mix. The DSP core-0 patch (tools/
 * rhythm_fm_fxbus.py) adds an FX bus: a track whose route word Y:$3770A+t is
 * 0 goes through the Grid FX, 1 bypasses them. Default all 0 (stock).
 *
 * A step's FX locks live in micro-steps 1-5 of its level/pan lanes (10, 11),
 * which no lock uses and which the step editor no longer overwrites (build:
 * 0x08047E32.. nop.w): byte k of the 7 = 0x80 | preset (0-7), 0xFF none.
 * Deleting the step erases them (lock_sweep), copying a step copies them.
 *
 * Playback (tick 0 of a hit, before its note): a hit with FX locks takes the
 * bus: its track alone is routed through the Grid FX, its effects are set and
 * switched on, every other effect is switched off (newest lock wins: the
 * earlier track goes back to dry). The owner's next hit without FX locks, or
 * transport stop, switches its effects off and routes every track back to
 * the bus (stock).
 */
#define GFX_TYPES 7u
#define GFX_PRESETS 8u
#define GFX_ROUTE 0x3770au
#define GFX_NONE 0xffu
static const uint8_t gfx_on[GFX_TYPES] = {0x0a, 0x0d, 0x0f, 0x11, 0x18, 0x1b, 0x20};
static const uint8_t gfx_count[GFX_TYPES] = {2, 1, 1, 6, 2, 4, 4};
static const uint32_t gfx_writer[GFX_TYPES] = {0x08030989u, 0x080309dbu, 0x08030a11u, 0x08030a57u, 0x08030b0fu, 0x08030b97u, 0x08030c19u};
/* 8 presets per type, left to right: more intense / faster (rates are the
   engine's tempo-synced indices). Settings in param order. */
static const uint8_t gfx_preset[GFX_TYPES][GFX_PRESETS][6] = {
    /* looper: length index long -> short, full level */
    {{8, 0}, {7, 0}, {6, 0}, {5, 0}, {4, 0}, {3, 0}, {2, 0}, {1, 0}},
    /* reverser: only four lengths, long -> short */
    {{3}, {3}, {2}, {2}, {1}, {1}, {0}, {0}},
    /* gater (stutter): rate index slow -> fast */
    {{0}, {1}, {2}, {3}, {4}, {5}, {7}, {9}},
    /* phaser: mode, centre, depth, sync, rate index, feedback */
    {{0, 64, 40, 0, 0, 30}, {0, 64, 52, 0, 1, 42}, {0, 64, 64, 0, 2, 54}, {0, 64, 76, 0, 3, 66},
     {0, 64, 88, 0, 4, 78}, {0, 64, 100, 0, 5, 90}, {0, 64, 114, 0, 6, 100}, {0, 64, 127, 0, 7, 110}},
    /* bit crusher: bit reduction 0-127 (DSP P:$504: quantiser step = (value/2 + 1) * 2^-11,
       i.e. about 6.5 bits at 48 down to 5 bits at 127), sample hold 0-127 samples (P:$4E4).
       0.3.0 used 3-10 (about 10 bits): barely audible on hardware. Profiles alternate
       grit (bits) and aliasing (hold), all heavier to the right. */
    {{48, 1}, {40, 4}, {80, 2}, {64, 7}, {100, 4}, {88, 12}, {118, 10}, {127, 32}},
    /* auto-filter: rate index slow -> fast, depth, resonance, offset (factory 10,75,80,63 / 8,82,114,43) */
    {{2, 60, 60, 63}, {3, 66, 69, 60}, {4, 72, 78, 57}, {5, 77, 87, 54},
     {6, 82, 96, 51}, {7, 88, 104, 48}, {8, 94, 112, 45}, {10, 100, 120, 42}},
    /* vinyl: four amounts (factory 63,80,65,65 / 105,127,127,127) */
    {{30, 40, 40, 40}, {41, 52, 52, 52}, {52, 64, 64, 64}, {63, 76, 76, 76},
     {74, 88, 88, 88}, {85, 100, 100, 100}, {95, 114, 114, 114}, {105, 127, 127, 127}},
};

/* The step's lock byte for type k (micro-steps 1-5 of lane 10, then 1-2 of lane 11). */
static uint8_t *gfx_byte(uint8_t *lanes, unsigned step, unsigned k) {
    return k < 5u ? lanes + 192u * 10u + 6u * step + 1u + k : lanes + 192u * 11u + 6u * step + 1u + (k - 5u);
}
static unsigned gfx_get(uint8_t *lanes, unsigned step, unsigned k) {
    const unsigned v = *gfx_byte(lanes, step, k);
    return (v & 0xf8u) == 0x80u ? (v & 7u) : GFX_NONE;
}

static void gfx_set(unsigned param, unsigned value) {
    void *router = S->plock_router;
    if (!router || !PL_PTR(router, 0x08)) return;
    void *setter = PL_PTR(router, 0x08);
    (**(void (***)(void *, unsigned, unsigned))setter)(setter, 0xa9u + (param - 0x0au), value);
}
static void gfx_write(unsigned k) {
#ifndef FM_HOST
    ((void (*)(void))gfx_writer[k])();
#else
    (void)k;
#endif
}
static void gfx_route(unsigned owner) {   /* owner 0-7, or 8: every track through the bus (stock) */
    for (unsigned t = 0; t < TRACKS; t++) {
        const unsigned dry = owner < TRACKS && t != owner;
        if (((S->gfx_routes >> t) & 1u) == dry) continue;
        S->gfx_routes ^= 1u << t;
#ifndef FM_HOST
        ((void (*)(unsigned, unsigned, unsigned))0x08031229u)(0, dry << 8, GFX_ROUTE + t);
#endif
    }
}
static void gfx_off(unsigned k) {
    gfx_set(gfx_on[k], 0); gfx_write(k);
    S->gfx_active &= ~(1u << k);
}
/* Every effect off, every track back through the bus. gfx_owner = track + 1 (0: none). */
void gfx_release(void) {
    if (S->magic != MAGIC || !S->gfx_owner) return;
    for (unsigned k = 0; k < GFX_TYPES; k++) if (S->gfx_active >> k & 1u) gfx_off(k);
    S->gfx_owner = 0;
    gfx_route(TRACKS);
    S->gfx_releases++;
}
/* Transport stop/reset of track t's player: its FX end. */
void gfx_release_track(unsigned t) {
    if (S->magic == MAGIC && S->gfx_owner == t + 1u) gfx_release();
}
/* A hit of track t (tick 0, before its note): its FX locks take the bus;
   the owner's hit without them releases it. */
void gfx_hit(void *player, unsigned t, unsigned pattern, unsigned step) {
    if (S->magic != MAGIC || t >= TRACKS) return;
    uint8_t *(*lane_at)(void *, unsigned, unsigned) = (*(uint8_t *(***)(void *, unsigned, unsigned))player)[5];
    uint8_t *lanes = lane_at(player, pattern, 0);
    unsigned set = 0, presets[GFX_TYPES];
    for (unsigned k = 0; k < GFX_TYPES; k++) if ((presets[k] = gfx_get(lanes, step, k)) != GFX_NONE) set |= 1u << k;
    if (!set) { if (S->gfx_owner == t + 1u) gfx_release(); return; }
    for (unsigned k = 0; k < GFX_TYPES; k++) {
        if (!(set >> k & 1u)) { if (S->gfx_active >> k & 1u) gfx_off(k); continue; }
        const unsigned p = presets[k];
        if ((S->gfx_active >> k & 1u) && (S->gfx_preset >> (4u * k) & 15u) == p) continue;   /* already playing */
        for (unsigned i = 0; i < gfx_count[k]; i++) gfx_set(gfx_on[k] + 1u + i, gfx_preset[k][p][i]);
        gfx_set(gfx_on[k], 1);
        gfx_write(k);
        S->gfx_active |= 1u << k;
        S->gfx_preset = (S->gfx_preset & ~(15u << (4u * k))) | p << (4u * k);
    }
    if (S->gfx_owner != t + 1u) S->gfx_claims++;
    S->gfx_owner = t + 1u;
    gfx_route(t);
    S->gfx_hits++;
}
