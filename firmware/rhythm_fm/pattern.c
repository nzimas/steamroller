/* SPDX-License-Identifier: GPL-3.0-only
 * Pattern generator: with Record on, Shift + a track button writes a whole new
 * pattern into that track's current pattern (the one its editor shows): its
 * length, hits, velocities, gates, micro-steps (ratchets, flams, nudges),
 * probabilities, parameter locks (spice.c's engine, bolder) and Grid FX locks.
 * Never the pitch: every step plays at the sample's own pitch, note 72 (step
 * byte 0). Notes live in 36-108 (the octave shifts refuse below 48 / from 97,
 * 0x08057870 / 0x080578D0), centred on 72; hardware: 60 was an octave low
 * (0.6.1), 12 farts (0.6.0), 0 (played as 48, 0x0804AFE6) too low (0.5.0).
 * Writing 72 also repairs patterns those versions made.
 *
 * Two paths (pads 25/26 of the secondary Sample Rec view, extension.c):
 *   - one sound: the track's own sample on every hit (no sample flips);
 *   - the pool: a drum part on one track from the pack's samples, by role.
 *     Each slot's role comes from its tag (the recipe that made it: kick,
 *     snare/clap, hat/cymbal, metal/impact/pluck/folded as percussion,
 *     noise/destroyed, click); a sample without a tag by its place on its page
 *     (pads 1-4 kick, 5-8 snare, 9-12 hat, 13-16 percussion). Which slots hold
 *     a sample comes from the Sample Rec view's own map (router[+0x58]
 *     store [+0x30], a bit per slot, 0x0805985C), so the flips never depend
 *     on the tag reads (0.6.1 hardware: no flips at all when they failed). Each role gets
 *     a Euclidean layer of its own (the kick on the beats, the snare on the
 *     backbeat, hats off the beat...), merged by priority (the snare first:
 *     the backbeat wins over a four-on-the-floor kick); every hit flips to
 *     one of two samples drawn for its role (sample flip, step byte 5).
 *
 * Four styles, drawn at each press:
 *   - industrial techno: 16 or 32 steps, a repeating 8- or 16-step cell
 *     (25-45 % of steps), accents on the beats, straight, fills at the end;
 *   - power noise: the densest (35-60 %), long gates on the accents, six-fold
 *     ratchets, heavy Grid FX (crusher, stutter, looper);
 *   - IDM: odd lengths (7-31, one cycle) or 20/28/32 (two halves), nudged
 *     micro-steps, odd ratchet shapes, reverser and looper;
 *   - broken: 12, 24 or 32 steps, sparse (18-33 %), swung or straight, long
 *     gates, throws.
 * The pack path plays kick, snare and hats with at most one or two of
 * percussion, noise and clicks, thinned to a cap per cell (44-69 %).
 * Hits are Euclidean (k of n, rotated) per cell. Every decision for a hit
 * (velocity by the metre, its one ratchet spot, swing, nudges, gate, ghosts,
 * the sample it flips to, a recurring Grid FX spot) is made per position in
 * the cell, so the cell repeats exactly; only the last cell, the fill, adds
 * hits, ratchets and effects of its own. Ghosts and fill hits play on a
 * probability.
 *
 * Writing a pattern (stock layout, docs/RHYTHM-AUTOMATION-PLOCKS.md 1.2):
 * steps at +8 s {note, gate (6 = a step), micro-step mask, velocity,
 * probability (7 = always), sample flip (0xFF), flag, 0}, end/start at
 * +0x100/+0x101, lanes at +0x128. The track editor keeps its own copy (two
 * step stores and a settings store) and writes it back over the pattern on
 * every edit, so after writing, the editor reloads it: 0x08048932(editor),
 * stock's own reload when the pattern changes, which notifies the views and
 * the player. The editor is the knob router's per-track object less 0x1C
 * (the object steplock.c reads: its +0x24/+0xC/+0 are the editor's step-held
 * flag, {pattern, held step} store and first step store).
 */
#define PG_RELOAD 0x08048933u
enum { PG_TECHNO, PG_POWER, PG_IDM, PG_BROKEN, PG_STYLES };

static uint32_t pg_in(uint32_t *r, unsigned a, unsigned b) { return a + sp_rnd(r) % (b - a + 1u); }
static unsigned pg_chance(uint32_t *r, unsigned pct) { return sp_rnd(r) % 100u < pct; }
static uint32_t pg_euclid(unsigned k, unsigned n, unsigned rot) {
    uint32_t m = 0;
    for (unsigned i = 0; i < n; i++) if (i * k % n < k) m |= 1u << (i + rot) % n;
    return m;
}
static void *pg_editor(unsigned t) {
    void *R = S->plock_router;
    if (!R || !PL_PTR(R, 0x34)) return 0;
    uint8_t *X = *(uint8_t **)((uint8_t *)PL_PTR(R, 0x34) + t * *(uint32_t *)((uint8_t *)R + 0x2c));
    return X ? X - 0x1c : 0;
}
#ifdef FM_HOST
extern void host_editor_reload(void *editor);
#endif
enum { PR_SNARE, PR_KICK, PR_NOISE, PR_PERC, PR_HAT, PR_CLICK, PG_ROLES };   /* (priority order: the backbeat beats the kick) */
/* each kit category's role (BAYAREA's rumbles and whirs are not drums) */
static const uint8_t pg_role_of[C_COUNT] = {PR_KICK, PR_KICK, PR_SNARE, PR_SNARE, PR_SNARE, PR_PERC, PR_HAT, PR_HAT, PR_PERC,
                                           PR_PERC, PR_NOISE, PR_CLICK, PR_NOISE, PR_NOISE, 0xff, 0xff, PR_NOISE};   /* (scrape, hiss: noise; rumble, whir: not drums) */
/* Two slots per role from the current pack (drawn evenly); returns the roles found. */
static unsigned pg_pool(uint8_t pool[][2], uint32_t *r, void *router) {
    KitCensus census;
    uint8_t cat[128];
    unsigned roles = 0, n[PG_ROLES] = {0};
    memset(&census, 0, sizeof census);
    kit_tags(&census, 128u, cat);
    void *srv = router ? PL_PTR(router, 0x58) : 0, *map = srv ? PL_PTR(srv, 0x30) : 0;
    for (unsigned slot = 0; slot < 128u; slot++) {
        if (cat[slot] == 0xffu && map && (*(uint32_t *)plock_store(map, slot >> 5) >> (slot & 31u) & 1u)) cat[slot] = 0xfeu;
        static const uint8_t by_place[4] = {PR_KICK, PR_SNARE, PR_HAT, PR_PERC};
        const unsigned c = cat[slot], role = c == 0xfeu ? by_place[(slot & 15u) >> 2] : c < C_COUNT ? pg_role_of[c] : 0xffu;
        if (role >= PG_ROLES) continue;
        if (!n[role]++) { pool[role][0] = pool[role][1] = (uint8_t)slot; roles |= 1u << role; }
        else if (sp_rnd(r) % n[role] == 0) pool[role][0] = (uint8_t)slot;
        else if (sp_rnd(r) % n[role] == 0) pool[role][1] = (uint8_t)slot;
    }
    return roles;
}

/* Record + Shift + track t (router: the UI router, for the slot map; 0 none).
   Returns the number of hits written (0: refused). */
__attribute__((noinline)) unsigned pattern_generate(unsigned t, void *router) {
    uint8_t *ed = pg_editor(t);
    uint8_t *base = ed ? (uint8_t *)PL_PTR(ed, 4) : 0;
    if (!base || !PL_PTR(ed, 0x28) || (S->players[t] && PL_PTR(S->players[t], 0x50) != base)) { S->pg_refusals++; return 0; }
    const unsigned pattern = *plock_store(PL_PTR(ed, 0x28), 0) & 7u;
    uint8_t *pat = base + 0xa28u * pattern, *lanes = pat + 0x128;
    uint32_t r = entropy() ^ S->patterns * 0x9e3779b9u ^ t * 0x85ebca6bu ^ 0x2545f491u;
    if (!r) r = 1;
    static const uint8_t sw[PG_STYLES] = {3, 3, 3, 2};
    unsigned style = sp_rnd(&r) % 11u;
    for (unsigned i = 0; i < PG_STYLES; i++) { if (style < sw[i]) { style = i; break; } style -= sw[i]; }
    /* length and repeating cell (the groove repeats cell by cell; only the last cell, the fill, varies) */
    static const uint8_t idm_len[14] = {7, 9, 11, 13, 15, 19, 21, 23, 27, 29, 31, 20, 28, 32};
    static const uint8_t pow_len[5] = {16, 16, 12, 24, 32}, brk_len[4] = {24, 32, 32, 12};
    static const uint8_t k_pct[PG_STYLES][2] = {{25, 45}, {35, 60}, {25, 45}, {18, 33}};
    unsigned len, cell;
    switch (style) {
    case PG_TECHNO: len = pg_chance(&r, 35) ? 32u : 16u; cell = pg_chance(&r, 50) ? 8u : 16u; break;
    case PG_POWER: len = pow_len[sp_rnd(&r) % 5u]; cell = len % 8u ? len : pg_chance(&r, 50) ? 8u : len < 16u ? len : 16u; break;
    case PG_IDM: len = idm_len[sp_rnd(&r) % 14u]; cell = len & 1u ? len : len / 2u; break;   /* (odd: one cycle against the other tracks) */
    default: len = brk_len[sp_rnd(&r) % 4u]; cell = len == 32u ? 16u : 12u; break;
    }
    unsigned k = pg_in(&r, cell * k_pct[style][0] / 100u, cell * k_pct[style][1] / 100u);
    if (k < 2u) k = 2u;
    const uint32_t seed = sp_rnd(&r);
#define PH(salt, x) sp_hash(seed, (salt), (x))                   /* a decision for position x: the same in every cell */
    /* the pool: kick, snare and hats always; at most one or two of percussion, noise and clicks */
    static const uint8_t layer_of[PG_STYLES][PG_ROLES][2] = {
        {{2, 4}, {4, 0}, {0, 0}, {2, 0xff}, {4, 2}, {2, 0xff}}, {{2, 0xff}, {4, 0}, {2, 0xff}, {3, 0xff}, {4, 0xff}, {2, 0xff}},
        {{2, 0xff}, {3, 0xff}, {1, 0xff}, {3, 0xff}, {3, 0xff}, {3, 0xff}}, {{2, 4}, {3, 0}, {1, 0xff}, {2, 0xff}, {3, 0xff}, {2, 0xff}}};
    static const uint8_t extras[PG_STYLES] = {1, 2, 2, 1}, cap_pct[PG_STYLES] = {50, 60, 45, 38};
    uint8_t pool[PG_ROLES][2], rolec[32];
    uint32_t layer[PG_ROLES], hits = 0;
    unsigned roles = S->pg_mode ? pg_pool(pool, &r, router) : 0;
    if (roles) {
        unsigned optional = roles & (1u << PR_PERC | 1u << PR_NOISE | 1u << PR_CLICK);
        while (__builtin_popcount(optional) > extras[style]) optional &= ~(1u << (PR_NOISE + sp_rnd(&r) % 3u));
        roles = (roles & (1u << PR_SNARE | 1u << PR_KICK | 1u << PR_HAT)) | optional;
    }
    for (unsigned i = 0; i < PG_ROLES; i++) {
        const unsigned kk = layer_of[style][i][0] * cell / 16u, rr = layer_of[style][i][1];
        layer[i] = roles >> i & 1u && layer_of[style][i][0] ? pg_euclid(kk ? kk : 1u, cell, rr == 0xffu ? sp_rnd(&r) % cell : rr % cell) : 0;
    }
    if (roles) {                                         /* merged by priority, then thinned to the cap (the last roles first) */
        for (unsigned c = 0; c < cell; c++) {
            unsigned role = 0;
            while (role < PG_ROLES && !(layer[role] >> c & 1u)) role++;
            rolec[c] = (uint8_t)role;
            if (role < PG_ROLES) hits |= 1u << c;
        }
        const unsigned cap = cap_pct[style] * cell / 100u;
        for (unsigned role = PG_ROLES; role-- > PR_NOISE && (unsigned)__builtin_popcount(hits) > cap;)
            for (unsigned n = 0; n < cell && (unsigned)__builtin_popcount(hits) > cap; n++) {
                const unsigned c = (seed + n) % cell;
                if (rolec[c] == role) hits &= ~(1u << c);
            }
    } else hits = pg_euclid(k, cell, style == PG_TECHNO ? 0 : sp_rnd(&r) % cell);
    const uint32_t accents = style == PG_IDM ? pg_euclid(k / 3u + 1u, cell, seed % cell) : 0x11111111u;
    static const uint8_t ratchet_pct[PG_STYLES] = {40, 70, 70, 40}, ghost_pct[PG_STYLES] = {8, 4, 10, 10}, ghost_max[PG_STYLES] = {1, 1, 2, 2};
    uint32_t ghosts = 0;                                 /* fixed ghost spots: at most one or two per cell */
    for (unsigned n = 0; n < cell; n++) {
        const unsigned c = (seed >> 5) % cell + n < cell ? (seed >> 5) % cell + n : (seed >> 5) % cell + n - cell;
        if (!(hits >> c & 1u) && PH(2, c) % 100u < ghost_pct[style] >> (roles != 0) && (unsigned)__builtin_popcount(ghosts) < ghost_max[style]) ghosts |= 1u << c;
    }
    static const uint8_t shapes[PG_STYLES][4] = {{0x09, 0x15, 0x09, 0x03}, {0x3f, 0x15, 0x09, 0x3f}, {0x15, 0x2d, 0x0b, 0x3f}, {0x09, 0x03, 0x09, 0x15}};
    const unsigned fill_from = len > cell ? len - cell : len - len / 4u;
    /* the cell's one ratchet spot (a hit, not a kick), its swing, its gate */
    unsigned rspot = 0xff, swing = style == PG_BROKEN && pg_chance(&r, 60), g0 = pg_in(&r, 4, 6), ga = pg_in(&r, 6, 18);
    if (pg_chance(&r, ratchet_pct[style]))
        for (unsigned n = 0; n < cell; n++) {
            const unsigned c = (seed + n) % cell;
            if (hits >> c & 1u && (!roles || rolec[c] != PR_KICK) && c % 4u) { rspot = c; break; }
        }
    unsigned written = 0;
    for (unsigned s = 0; s < 32u; s++) {
        uint8_t *st = pat + 8u * s;
        const unsigned c = s % cell, fill = s >= fill_from && len > 4u, key = fill ? 64u + s : c;
        unsigned hit = s < len && (hits >> c & 1u), ghost = 0, extra = 0, role = roles ? rolec[c] : PR_PERC;
        if (s < len && !hit && fill && style != PG_IDM && PH(1, s) % 100u < 20u) { hit = extra = 1; role = roles >> PR_NOISE & 1u ? PR_NOISE : PR_SNARE; }
        if (s < len && !hit && ghosts >> c & 1u) { hit = ghost = 1; role = roles >> PR_CLICK & 1u ? PR_CLICK : PR_HAT; }
        if (roles && hit && !(roles >> role & 1u)) role = __builtin_ctz(roles);   /* (a role the pack lacks) */
        unsigned gate = 6, mask = 0, vel = 0, prob = 7;
        if (hit) {
            /* velocity by the metre: beats strong, eighths medium, sixteenths soft (a small fixed lean per position) */
            const unsigned strong = roles ? role <= PR_KICK || (c & 3u) == 0 : style == PG_IDM ? accents >> c & 1u : (c & 3u) == 0;
            vel = ghost ? 34u + PH(3, c) % 18u : strong ? 106u + PH(3, key) % 18u : (c & 1u) == 0 ? 84u + PH(3, key) % 14u : 66u + PH(3, key) % 14u;
            if (roles && role >= PR_HAT && !ghost) vel -= 12u;
            mask = 1;
            if (style == PG_IDM && PH(4, c) % 100u < 25u) mask = 2u << PH(4, c) % 3u;          /* nudged late, the same each time */
            else if (swing && (c & 1u)) mask = 4;                                             /* swung off-steps */
            const unsigned ratchet = !ghost && (c == rspot || (fill && (!roles || role != PR_KICK) && PH(5, s) % 100u < ratchet_pct[style] / 3u));
            if (ratchet) mask = shapes[style][PH(6, key) & 3u];
            switch (style) {
            case PG_TECHNO: gate = g0; break;
            case PG_POWER: gate = strong ? ga : g0; break;
            case PG_IDM: gate = 3u + PH(7, c) % 10u; break;
            default: gate = 4u + PH(7, c) % 21u; break;
            }
            if (roles && role >= PR_HAT) gate = 3;          /* (at least half a step: shorter cuts the sample to a blip) */
            if (ratchet) gate = 1;
            if (ghost || extra || (style == PG_IDM && PH(8, c) % 100u < 15u)) prob = 4u + PH(8, key) % 3u;
            written++;
        }
        st[0] = 72;                                      /* (the pitch: see the top) */
        st[1] = (uint8_t)gate; st[2] = (uint8_t)mask; st[3] = (uint8_t)vel;
        st[4] = (uint8_t)prob; st[5] = hit && roles ? pool[role][(PH(9, key) & 3u) == 0] : 0xff; st[6] = 0; st[7] = 0;
    }
    pat[0x100] = (uint8_t)(len - 1u); pat[0x101] = 0;     /* end, start */
    for (unsigned i = 0; i < 12u * 192u; i++) lanes[i] = 0xff;
    unsigned palette;
    const unsigned locks = sp_apply(t, pat, sp_rnd(&r), 1, &palette);
    /* Grid FX: one recurring spot (the same hit in every cell, the same effect) and a few in the fill */
    static const uint8_t fx_fill[PG_STYLES] = {1, 3, 3, 2}, fx_w[PG_STYLES][GFX_TYPES] = {{1, 1, 3, 2, 2, 4, 1}, {2, 1, 4, 0, 6, 1, 2}, {4, 4, 4, 1, 3, 1, 1}, {3, 3, 2, 2, 2, 2, 3}};
    unsigned fx = 0, motif = style == PG_POWER || style == PG_IDM ? (pg_chance(&r, 60) ? (rspot != 0xffu ? rspot : seed >> 8) % cell : 0xffu) : 0xffu;
    unsigned nfill = pg_in(&r, 1, fx_fill[style]);
    for (unsigned s = 0; s < len; s++) {
        const uint8_t *st = pat + 8u * s;
        const unsigned c = s % cell, at_motif = c == motif && s < fill_from, in_fill = s >= fill_from && nfill && PH(10, s) % 100u < 45u;
        if (!st[2] || !(at_motif || in_fill)) continue;
        if (in_fill && !at_motif) nfill--;
        const unsigned key = at_motif ? 0u : s;
        unsigned total = 0, type = 0, pick;
        for (unsigned i = 0; i < GFX_TYPES; i++) total += fx_w[style][i];
        pick = PH(12, key) % total;
        while (pick >= fx_w[style][type]) pick -= fx_w[style][type++];
        *gfx_byte(lanes, s, type) = (uint8_t)(0x80u | (style == PG_POWER ? 4u : 2u) + PH(13, key) % (style == PG_POWER ? 4u : 6u));
        fx++;
    }
#undef PH
    S->spice_lanes = 0;                                  /* (not a spice to re-roll) */
#ifndef FM_HOST
    ((void (*)(void *))PG_RELOAD)(ed);
#else
    host_editor_reload(ed);
#endif
    S->patterns++;
    S->pg_last = t | pattern << 3 | style << 6 | (roles != 0) << 14 | len << 8 | (written > 255u ? 255u : written) << 16 | (fx > 15u ? 15u : fx) << 24 | (locks > 15u ? 15u : locks) << 28;
    return written;
}
