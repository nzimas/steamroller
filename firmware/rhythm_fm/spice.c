/* SPDX-License-Identifier: GPL-3.0-only
 * "Spice": Shift + a track button (outside the Sample Rec view's generate
 * mode) adds parameter locks to that track's current pattern, shaped by the
 * pattern's structure. They are ordinary locks in the stock lanes (plock.c
 * plays them; they save with the project). Lanes used (4-11): distortion,
 * HPF, LPF, resonance (macros 5-8), reverb send, delay send, level, pan.
 * Pitch, decay, attack and start/end (macros 1-4) are never touched.
 *
 * Analysis (sp_analyse): the pattern's start/end, its hits (step byte 2 =
 * micro-step trigger mask), velocities (byte 3), rolls (several micro-steps),
 * the shortest period (4, 8 or 16 steps) the hits repeat at, which hits are
 * the repeating backbone and which vary (fills), the room after each hit, and
 * how many other tracks hit on the same step.
 *
 * Spice (spice_track): a palette of 3-4 parameters is drawn, weighted by the
 * pattern (dense: pan/level/HPF; sparse: delay/reverb throws, distortion), with
 * at least one of pan/level and one tone parameter. Each parameter has a role
 * rule: backbone downbeats are left alone, ghosts move most, fills build
 * (crescendo, filter riser), throws go on hits with room after them.
 * Decisions for repeating hits are made per position in the period, so every
 * repeat gets the same move and the groove keeps its shape. Locks go on hits
 * only; plock.c holds each over the sound's tail until the track's next hit.
 *
 * The user's own locks are kept. Pressing again on the unchanged pattern
 * re-rolls: the lanes go back to how they were before the first press, then
 * get new spice. Track players are captured as they play (plock_seen).
 */
#define SP_LANE0 4u
#define SP_LANES 8u
enum { SP_DIST, SP_HPF, SP_LPF, SP_RESO, SP_REVERB, SP_DELAY, SP_LEVEL, SP_PAN };
#define SF_HIT 1u
#define SF_ACCENT 2u
#define SF_GHOST 4u
#define SF_ROLL 8u
#define SF_BACKBONE 16u
#define SF_FILL 32u
#define SF_SPACE 64u                      /* >= 3 steps to the next hit */
#define SF_LAST 128u                      /* the pattern's last hit */
typedef struct {
    uint8_t len, start, period, hits, fill_from, contrast, rolls, rsv;
    uint8_t flag[32], gap[32], busy[32], order[32];
} SpiceMap;

static uint32_t entropy(void);
static uint8_t *(*sp_lane_fn(void *player))(void *, unsigned, unsigned) {
    return (*(uint8_t *(***)(void *, unsigned, unsigned))player)[5];
}
/* The pattern the track player is on, and its base (steps at +0, start/end
   at +0x101/+0x100, lanes at +0x128). */
static uint8_t *sp_pattern(void *player, unsigned *pattern) {
    *pattern = *plock_store(PL_PTR(player, 0x08), 0) & 7u;
    return sp_lane_fn(player)(player, *pattern, 0) - 0x128;
}
static unsigned sp_hit(const uint8_t *pat, unsigned step) { return (pat[8u * (step & 31u) + 2u] & 0x3fu) != 0; }
static uint32_t sp_hash(uint32_t seed, unsigned a, unsigned b) {
    uint32_t h = seed ^ (a * 0x9e3779b9u) ^ (b * 0x85ebca6bu);
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}
static uint32_t sp_rnd(uint32_t *r) { *r ^= *r << 13; *r ^= *r >> 17; *r ^= *r << 5; return *r; }

static __attribute__((noinline)) void sp_analyse(SpiceMap *m, const uint8_t *pat, unsigned track) {
    unsigned end = pat[0x100] & 31u, start = pat[0x101] & 31u, len = ((end - start) & 31u) + 1u;
    unsigned vmin = 255, vmax = 0, n = 0, rolls = 0;
    uint32_t hit = 0;
    memset(m, 0, sizeof(*m));
    m->start = (uint8_t)start; m->len = (uint8_t)len;
    for (unsigned p = 0; p < len; p++) {
        const uint8_t *st = pat + 8u * ((start + p) & 31u);
        if (!(st[2] & 0x3fu)) continue;
        hit |= 1u << p; n++;
        m->flag[p] = SF_HIT;
        if (st[2] & 0x3fu & (st[2] - 1u)) { m->flag[p] |= SF_ROLL; rolls++; }
        if (st[3] < vmin) vmin = st[3];
        if (st[3] > vmax) vmax = st[3];
    }
    m->hits = (uint8_t)n; m->rolls = (uint8_t)rolls;
    m->contrast = vmax > vmin + 8u;
    if (!n) return;
    /* the shortest period the hits repeat at (7 of 8 steps agree) */
    unsigned period = len;
    for (unsigned P = 4; P < len; P *= 2) {
        if (len % P) continue;
        unsigned same = 0;
        for (unsigned p = 0; p < len; p++) same += !((hit >> p ^ hit >> ((p + P) % len)) & 1u);
        if (same * 8u >= len * 7u) { period = P; break; }
    }
    m->period = (uint8_t)period;
    m->fill_from = (uint8_t)(period < len ? len - period : len - len / 4u);
    /* other tracks hitting on the same step */
    for (unsigned u = 0; u < TRACKS; u++) {
        if (u == track || !S->players[u]) continue;
        unsigned pattern;
        const uint8_t *other = sp_pattern(S->players[u], &pattern);
        for (unsigned p = 0; p < len; p++) m->busy[p] += (uint8_t)sp_hit(other, start + p);
    }
    unsigned last = 0, order = 0;
    for (unsigned p = 0; p < len; p++) {
        if (!(hit >> p & 1u)) continue;
        const unsigned v = pat[8u * ((start + p) & 31u) + 3u];
        unsigned f = m->flag[p], backbone = 1;
        for (unsigned q = p % period; q < len; q += period) backbone &= hit >> q & 1u;
        if (backbone && period < len) f |= SF_BACKBONE;
        if (period == len && (p & 3u) == 0) f |= SF_BACKBONE;   /* no repeat found: the beats carry it */
        if (m->contrast) {
            if (v * 4u >= vmax * 3u + vmin) f |= SF_ACCENT;
            else if (v * 4u <= vmin * 3u + vmax) f |= SF_GHOST;
        } else if ((p & 3u) == 0) f |= SF_ACCENT;
        else if (p & 1u) f |= SF_GHOST;
        if ((period < len && !backbone) || (period == len && p >= m->fill_from && !(f & SF_BACKBONE))) f |= SF_FILL;
        unsigned gap = 1;
        while (gap < len && !(hit >> ((p + gap) % len) & 1u)) gap++;
        m->gap[p] = (uint8_t)gap;
        if (gap >= 3u) f |= SF_SPACE;
        if (p % period == 0) order = 0;
        m->order[p] = (uint8_t)order++;
        m->flag[p] = (uint8_t)f;
        last = p;
    }
    m->flag[last] |= SF_LAST;
}

typedef struct { uint32_t seed; uint8_t palette, shape, amount[SP_LANES]; } SpiceRoll;

/* The palette: 3-4 parameters, weighted by the pattern; at least one of
   pan/level and one tone parameter (not resonance alone). */
static __attribute__((noinline)) unsigned sp_palette(const SpiceMap *m, uint32_t *r) {
    uint8_t w[SP_LANES] = {3, 3, 4, 2, 2, 3, 4, 5};
    const unsigned density = m->hits * 16u / m->len;
    if (density >= 8u) { w[SP_PAN] += 4; w[SP_LEVEL] += 3; w[SP_HPF] += 2; w[SP_DELAY] = 1; w[SP_REVERB] = 1; }
    else if (density <= 4u) { w[SP_DELAY] += 3; w[SP_REVERB] += 2; w[SP_DIST] += 2; w[SP_PAN] -= 2; }
    if (m->rolls) { w[SP_LPF] += 2; w[SP_PAN] += 2; }
    if (m->contrast) w[SP_DIST] += 2;
    static const uint8_t pools[3] = {1u << SP_PAN | 1u << SP_LEVEL, 1u << SP_DIST | 1u << SP_HPF | 1u << SP_LPF, 0xffu};
    unsigned palette = 0, count = 3u + (sp_rnd(r) & 1u);
    for (unsigned i = 0; i < count; i++) {
        const unsigned pool = pools[i < 2u ? i : 2u] & ~palette;
        unsigned total = 0;
        for (unsigned k = 0; k < SP_LANES; k++) if (pool >> k & 1u) total += w[k];
        unsigned pick = sp_rnd(r) % total;
        for (unsigned k = 0; k < SP_LANES; k++) if (pool >> k & 1u) {
            if (pick < w[k]) { palette |= 1u << k; break; }
            pick -= w[k];
        }
    }
    return palette;
}

/* A hit's move for parameter k (a delta from the knob's base), 0 = none. */
static __attribute__((noinline)) int sp_move(const SpiceMap *m, const SpiceRoll *R, unsigned p, unsigned k, unsigned base, unsigned palette) {
    const unsigned f = m->flag[p], period = m->period, len = m->len;
    const unsigned anchor = (f & SF_BACKBONE) && !(f & SF_GHOST) && ((p & 3u) == 0 || (f & SF_ACCENT));
    const unsigned fill = f & SF_FILL, ghost = f & SF_GHOST, accent = f & SF_ACCENT;
    /* repeating hits decide per position in the period, fills per step */
    const uint32_t h = sp_hash(R->seed, k, fill ? 32u + p : p % period);
    const unsigned chance = h & 255u, mag = h >> 8, shape = R->shape >> k & 1u;
    const unsigned ramp = fill && len > m->fill_from ? (p - m->fill_from + 1u) * 100u / (len - m->fill_from) : 0;
    int d = 0;
    switch (k) {
    case SP_PAN:
        if (anchor) return 0;
        if (!shape) {                                     /* ping-pong within the period */
            d = 14 + (int)(mag % 24u);
            if ((m->order[p] ^ (R->seed >> 3)) & 1u) d = -d;
            if (accent) d /= 2;
            if (fill) d = d * 3 / 2;
        } else {                                          /* an arc across the period */
            const int W = 18 + (int)(R->seed >> 9 & 15u), q = (int)(p % period), P = (int)period;
            d = 2 * q < P ? -W + 4 * W * q / P : 3 * W - 4 * W * q / P;
            if (R->seed & 1u) d = -d;
        }
        if (chance > 220u) return 0;
        break;
    case SP_LEVEL:
        if (fill) { d = -(int)(28u * (100u - ramp) / 100u) - 4; break; }   /* crescendo into the loop */
        if (anchor || accent) return 0;
        if (!shape) d = ghost ? -(12 + (int)(mag % 20u)) : chance < 128u ? -(4 + (int)(mag % 10u)) : 0;
        else d = (p & 1u) ? -(14 + (int)(mag % 14u)) : (p & 3u) == 2u ? -(6 + (int)(mag % 8u)) : 0;
        break;
    case SP_LPF:
        if (fill) { d = base < 112u ? 10 + (int)(mag % 24u) : -(10 + (int)(ramp * 30u / 100u)); break; }
        if (anchor) return 0;
        if (base >= 48u) {
            if (!shape) d = ghost ? -(18 + (int)(mag % 34u)) : chance < 120u ? -(8 + (int)(mag % 20u)) : 0;
            else d = -(int)((p % period) * (20u + mag % 30u) / period);   /* closing over the period */
        } else if (accent || chance < 90u) d = 18 + (int)(mag % 40u);
        break;
    case SP_HPF:
        if (fill) { d = 10 + (int)(ramp * (30u + mag % 30u) / 100u); break; }   /* riser */
        if (anchor || base >= 90u) return 0;
        d = ghost ? 15 + (int)(mag % 35u) : chance < 70u ? 8 + (int)(mag % 16u) : 0;
        break;
    case SP_RESO:
        if (palette & (1u << SP_LPF | 1u << SP_HPF)) { if (anchor || chance > 170u) return 0; d = 10 + (int)(mag % 30u); }
        else if ((fill || accent) && chance < 150u) d = 10 + (int)(mag % 25u);
        break;
    case SP_DIST:
        if (!shape) { if (accent || fill || ((f & SF_SPACE) && chance < 140u)) d = 15 + (int)(mag % 40u); }
        else if (ghost && chance < 200u) d = 20 + (int)(mag % 40u);   /* dirty ghosts */
        break;
    }
    if ((k == SP_PAN || k == SP_LEVEL) && m->busy[p] >= 3u) d /= 2;   /* a busy moment: keep the groove tight */
    return d * R->amount[k] / 100;
}

/* Delay/reverb throws: the hits with the most room after them, the phrase's
   last hit and fills first; a few per pattern (fewer when dense). */
static __attribute__((noinline)) uint32_t sp_throws(const SpiceMap *m, const SpiceRoll *R, unsigned k, unsigned count) {
    uint32_t chosen = 0;
    for (unsigned c = 0; c < count; c++) {
        unsigned best = 0, best_p = 32;
        for (unsigned p = 0; p < m->len; p++) {
            const unsigned f = m->flag[p];
            if (!(f & SF_HIT) || (chosen >> p & 1u) || !(f & (SF_SPACE | SF_LAST))) continue;
            unsigned score = 1u + m->gap[p] * 4u + (f & SF_FILL ? 6u : 0) + (f & SF_LAST ? 8u : 0) + (m->busy[p] ? 0 : 4u)
                + (sp_hash(R->seed, k, p) & 7u);
            if (k == SP_REVERB && (f & SF_ACCENT)) score += 4u;
            if (score > best) { best = score; best_p = p; }
        }
        if (best_p < 32u) chosen |= 1u << best_p;
    }
    return chosen;
}

static uint32_t sp_sum(uint8_t *lanes) {
    uint32_t h = 2166136261u;
    for (unsigned l = 0; l < SP_LANES; l++) for (unsigned s = 0; s < 32u; s++) h = (h ^ lanes[192u * (SP_LANE0 + l) + 6u * s]) * 16777619u;
    return h;
}

static unsigned spice_players(void) {
    unsigned mask = 0;
    for (unsigned t = 0; t < TRACKS; t++) if (S->players[t]) mask |= 1u << t;
    return mask;
}

static const uint8_t sp_default[SP_LANES] = {0, 0, 127, 0, 0, 0, 100, 64};
/* The parameter id of lane l on track t (the stock lane id table 0x0804ACC4). */
static unsigned sp_lane_id(unsigned t, unsigned l) {
    static const uint8_t mix[4] = {0x80, 0x88, 0x91, 0x99};
    return l < 8u ? 16u * t + l : mix[l - 8u] + t;
}

/* Lock the hits of pattern pat on track t (lanes 4-11), leaving any lock
   already there. boost (the pattern generator): one more palette (4-6
   parameters). Returns the number of locks written; *palette_out the lanes. */
static __attribute__((noinline)) unsigned sp_apply(unsigned t, uint8_t *pat, uint32_t r, unsigned boost, unsigned *palette_out) {
    uint8_t *lanes = pat + 0x128;
    SpiceMap m;
    sp_analyse(&m, pat, t);
    unsigned written = 0, palette = 0;
    if (m.hits) {
        SpiceRoll R;
        if (!r) r = 1;
        R.seed = sp_rnd(&r); R.shape = (uint8_t)sp_rnd(&r);
        for (unsigned k = 0; k < SP_LANES; k++) R.amount[k] = (uint8_t)(60u + sp_rnd(&r) % 41u);   /* (the generator too: bolder made it muffled) */
        palette = sp_palette(&m, &r);
        if (boost) palette |= sp_palette(&m, &r);          /* (two palettes: 4-6 parameters) */
        const unsigned dense = m.hits * 16u / m.len;
        for (unsigned k = 0; k < SP_LANES; k++) {
            if (!(palette >> k & 1u)) continue;
            unsigned base = plock_live(sp_lane_id(t, SP_LANE0 + k));
            if (base > 127u) base = sp_default[k];
            uint8_t *lane = lanes + 192u * (SP_LANE0 + k);
            const uint32_t throws = k == SP_DELAY || k == SP_REVERB
                ? sp_throws(&m, &R, k, (k == SP_DELAY ? (dense >= 8u ? 1u : dense >= 4u ? 2u : 3u) : (dense >= 8u ? 1u : 2u)) + boost) : 0;
            for (unsigned p = 0; p < m.len; p++) {
                if (!(m.flag[p] & SF_HIT)) continue;
                const unsigned s = (m.start + p) & 31u;
                if (lane[6u * s] != 0xffu) continue;            /* the user's own lock */
                int d;
                if (k == SP_DELAY || k == SP_REVERB) {
                    if (!(throws >> p & 1u)) continue;
                    const int target = (k == SP_DELAY ? 55 : 50) + (int)(sp_hash(R.seed, k, 64u + p) % 40u);
                    d = target > (int)base ? target - (int)base : 0;
                } else d = sp_move(&m, &R, p, k, base, palette);
                int v = (int)base + d;
                if (k == SP_LEVEL && v < 20 && d < 0) v = (int)base < 20 ? (int)base : 20;   /* never near-silent */
                if (v < 0) v = 0;
                if (v > 127) v = 127;
                if (v - (int)base < 3 && (int)base - v < 3) continue;
                lane[6u * s] = (uint8_t)v;                     /* plock.c holds it until the next hit */
                written++;
            }
        }
    }
    *palette_out = palette | (unsigned)m.period << 16;
    return written;
}

/* Shift + track t: spice its current pattern. Returns the number of locked steps written. */
__attribute__((noinline)) unsigned spice_track(unsigned t) {
    void *player = S->players[t];
    if (!player) { S->spice_refusals++; return 0; }
    unsigned pattern;
    uint8_t *pat = sp_pattern(player, &pattern), *lanes = pat + 0x128;
    /* re-roll: the same pattern, untouched since our spice -> back to before it */
    const unsigned reroll = S->spice_lanes == lanes && sp_sum(lanes) == S->spice_sum;
    for (unsigned l = 0; l < SP_LANES; l++) for (unsigned s = 0; s < 32u; s++) {
        uint8_t *b = &lanes[192u * (SP_LANE0 + l) + 6u * s];
        if (reroll) *b = S->spice_snap[l][s]; else S->spice_snap[l][s] = *b;
    }
    S->spice_lanes = lanes;
    unsigned palette;
    const unsigned written = sp_apply(t, pat, entropy() ^ S->spices * 0x9e3779b9u ^ (uint32_t)(uintptr_t)lanes ^ 0x5bd1e995u, 0, &palette);
    S->spice_sum = sp_sum(lanes);
    S->spices++;
    S->spice_last = t | pattern << 3 | reroll << 6 | (palette & 0xffu) << 8 | (written > 255u ? 255u : written) << 16 | (palette >> 16) << 24;
    return written;
}
