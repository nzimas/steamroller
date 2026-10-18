/* SPDX-License-Identifier: GPL-3.0-only
 * BAYAREA for SteamRoller: a West Coast voice, ported from Tonitor's sixth
 * engine (mvave-fm1 firmware/src/bayarea.c, a fixed-point port of the bayarea
 * opcode of Vascular, BayareaCore.h; same author). Per voice:
 *   a carrier morphing sine -> triangle -> square -> saw (SHAPE), its frequency
 *   modulated linearly (through zero) by two sines at ratios of it (each ratio
 *   wandering a little, slowly); a wavefolder (FOLDS) into +-0.5; a 12 kHz
 *   Butterworth smoothing filter, a breath of noise; then a low-pass gate (a
 *   resonant low-pass and an amplifier struck together by one-shot envelopes)
 *   or a sustaining envelope over two resonant low-passes; a saturating output
 *   (tanh, then a soft clip); a bank of three resonators (508, 270, 1153 Hz,
 *   ringing 1 .. 1.4 s) beside it; one LFO on eight destinations.
 *
 * Changed from Tonitor for the Circuit Rhythm (48 kHz, sound generation):
 *   - 48 kHz: the control rate (1500 blocks/s), the phase steps (2^32 / 48000),
 *     the 12 kHz filter, the resonators (same pitch and ring time), the filter's
 *     tan table and the smoothing constants are recomputed
 *     (tools/gen_bayarea_tables.py); SINE is SteamRoller's own (same layout);
 *   - no live macros (EDIT), no track envelope (hold), no pool: a single voice
 *     renders a sound for the generator; the note is a MIDI note (Hz Q8);
 *   - the loudness probe (ba_measure_on) is left out: SteamRoller levels every
 *     generated sound itself (audition gain, -1 dBFS when stored);
 *   - a 10 Hz DC blocker at the output: the folder and the floored filters
 *     leave an offset (up to 5 %) that would click where a sample ends.
 * All integer: rendered on the host and the Rhythm bit for bit.
 */
#include "generated/bayarea_tables.h"

#define BA_Q15 32768
#define BA_Q16 65536
#define BA_CTL 32                                  /* samples per control block */
#define BA_PH 89478u                               /* 2^32 / 48000: phase per Hz */
#define BA_LAG5 19723                              /* the destinations' smoothing (5 ms) per block */
#define BA_LAG10 12093                             /* the resonators' mix (10 ms) */
#define BA_LFO 2863312u                            /* Hz Q16 -> phase per block: 2^32 / 1500 */
#define BA_WANDER 4369                             /* the ratios' wander, Q16 per block */

static inline __attribute__((always_inline)) int32_t ba_clamp(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline __attribute__((always_inline)) int32_t ba_mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline __attribute__((always_inline)) int32_t ba_abs(int32_t x) { return x < 0 ? -x : x; }
/* 2^(x / 65536), Q16 in and out */
static uint32_t ba_exp2(int32_t x)
{
    int32_t i = x >> 16;
    uint32_t f = (uint32_t)x & 0xFFFFu, k = f >> 8, a = EXP2_FRAC[k];
    uint32_t m = a + (((EXP2_FRAC[k + 1u] - a) * (f & 0xFFu)) >> 8);
    if (i >= 15) return 0x7FFFFFFFu;
    if (i >= 0) return m << i;
    if (i < -24) return 0;
    return m >> -i;
}
static inline __attribute__((always_inline)) int32_t ba_tanh(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i, f, y;
    if (a >= 4 * BA_Q15) y = TANH_Q15[512];
    else { i = a >> 8; f = a & 255; y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * f) >> 8); }
    return x < 0 ? -y : y;
}
static inline __attribute__((always_inline)) int32_t ba_sin(uint32_t ph)
{
    const int16_t *s = &SINE[ph >> 22];
    return s[0] + (((s[1] - s[0]) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}
static uint32_t ba_div64(uint32_t hi, uint32_t lo, uint32_t d)   /* (hi:lo) / d, saturating */
{
    uint32_t r, q = 0, carry;
    if (!d || hi >= d) return 0xFFFFFFFFu;
    r = hi;
    for (int i = 31; i >= 0; i--) {
        carry = r >> 31; r = (r << 1) | ((lo >> i) & 1u); q <<= 1;
        if (carry || r >= d) { r -= d; q |= 1u; }
    }
    return q;
}
static int32_t ba_tan_g(int32_t hz)                /* tan(pi f / FS), Q24 */
{
    uint32_t x, i, f8;
    hz = ba_clamp(hz, 16, TANG_FMAX);
    x = (uint32_t)hz * 512u; i = x / TANG_FMAX;
    if (i >= 512u) return (int32_t)TAN_G[512];
    f8 = (x - i * TANG_FMAX) * 256u / TANG_FMAX;
    return (int32_t)(TAN_G[i] + (((TAN_G[i + 1u] - TAN_G[i]) * f8) >> 8));
}
typedef struct { uint32_t s; } ba_rnd_t;            /* xorshift32, as Tonitor's */
static inline __attribute__((always_inline)) uint32_t ba_next(ba_rnd_t *r)
{
    uint32_t x = r->s ? r->s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return r->s = x;
}
static void ba_seed(ba_rnd_t *r, uint32_t seed)
{
    r->s = seed * 2654435761u + 0x6A09E667u;
    if (!r->s) r->s = 1;
    for (int i = 0; i < 4; i++) ba_next(r);
}
static inline __attribute__((always_inline)) int32_t ba_q15(ba_rnd_t *r) { return (int32_t)(ba_next(r) >> 17); }
static inline __attribute__((always_inline)) int32_t ba_bi(ba_rnd_t *r) { return (int32_t)(ba_next(r) >> 16) - 32768; }
static int32_t ba_below(ba_rnd_t *r, int32_t n) { return n <= 1 ? 0 : (int32_t)(((uint64_t)ba_next(r) * (uint32_t)n) >> 32); }
static int ba_chance(ba_rnd_t *r, int32_t pct) { return ba_below(r, 1000) < pct * 10; }
static int32_t ba_in(ba_rnd_t *r, int32_t a, int32_t b) { return a + (int32_t)(((int64_t)(b - a) * ba_q15(r)) >> 15); }

enum { BK_BONGO, BK_PLUCK, BK_CHIME, BK_FOLDED, BK_SWELL, BK_DRONE, BK_N };
enum { BA_OPEN, BA_AMBIENT, BA_EXP };               /* Tonitor's generative paths */
static const uint8_t BK_W[3][BK_N] = {{1, 1, 1, 1, 1, 1}, {3, 1, 3, 0, 3, 4}, {1, 3, 1, 4, 1, 1}};
/* each kind's ranges, slider positions x 100: shape, folds, FM low, FM high, peak, decay, drive, spring */
static const uint8_t BK_R[BK_N][8][2] = {
    {{0, 30}, {0, 15}, {0, 30}, {0, 20}, {20, 50}, {10, 35}, {10, 30}, {0, 30}},    /* BONGO */
    {{30, 80}, {0, 40}, {0, 40}, {0, 30}, {50, 90}, {15, 45}, {10, 40}, {0, 40}},   /* PLUCK */
    {{0, 40}, {0, 20}, {0, 30}, {30, 80}, {60, 100}, {40, 70}, {5, 25}, {20, 60}},  /* CHIME */
    {{20, 70}, {50, 100}, {0, 50}, {0, 50}, {40, 90}, {30, 60}, {15, 50}, {0, 40}}, /* FOLDED */
    {{0, 70}, {0, 50}, {0, 40}, {0, 40}, {40, 80}, {50, 80}, {5, 25}, {10, 50}},    /* SWELL */
    {{0, 80}, {0, 60}, {20, 60}, {0, 50}, {20, 70}, {55, 85}, {5, 30}, {10, 60}},   /* DRONE */
};

typedef struct {
    uint32_t seed;
    uint8_t kind, path, asr, lfo_shape;   /* asr: the sustaining envelope (else the low-pass gate) */
    int32_t shape, folds, fm1, fm2;       /* Q15 sliders */
    int32_t fm1r, fm2r;                   /* the modulators' ratios, Q16 */
    int32_t attack, decay;                /* ms */
    int32_t peak;                         /* Hz */
    int32_t drive;                        /* x 0 .. 11, Q12 */
    int32_t spring;                       /* Q15 */
    int32_t lfo_hz;                       /* Q16 */
    int32_t to[8];                        /* the LFO's amounts: shape, folds, FM low, FM high, attack, peak, decay, spring */
    int32_t out;                          /* Q12 */
    int32_t res[3][2];                    /* this sound's resonators: c1, c2 (Q30) */
} ba_design_t;

typedef struct { int32_t from, to, c, n, at, run; } ba_seg_t;
typedef struct { int32_t a0, b1, b2; } ba_rc_t;
typedef struct {
    uint8_t live, gate, stage_c, stage_a, stage_g, primed, rsv[2];
    int32_t vel, f;                       /* Q15; the note, Hz Q8 */
    ba_rnd_t r;
    uint32_t lfo_ph, rph;
    int32_t lfo_rand;
    int32_t lag[9];
    int32_t n1[3], n2[3], nu1, nu2;
    int32_t pm1, pm2, pamp1, pamp2, pxf, pfold, pev, pmix;
    int32_t m1, m2, amp1, amp2, xf, fold, ev, mix;
    uint32_t ph1, ph2, ph;
    int32_t bx1, bx2, by1, by2, pk;
    ba_seg_t cut, amp, gte;
    int32_t cl0, al0, gl0;
    ba_rc_t rc0, rc1;
    int32_t lv1, lv2, sv1[2], sv2[2];
    int32_t rs[3][2];
    int32_t decay_ms, attack_ms, quiet;
    int32_t dcx, dcy;                     /* the DC blocker: last input; last output, Q23 */
} ba_voice_t;

/* a curved segment from y0 to y1 over ms (Env's law: (1 - e^(c t)) / (1 - e^c)) */
static void ba_seg(ba_seg_t *s, int32_t y0, int32_t y1, int32_t ms, int32_t c)
{
    s->from = y0; s->to = y1; s->c = c;
    s->n = ba_clamp(ms * 3 / 2, 1, 1 << 20);       /* blocks: 1.5 per ms */
    s->at = 0; s->run = 1;
}
static int32_t ba_seg_tick(ba_seg_t *s)
{
    int32_t t, e1, et, g;
    if (!s->run) return s->to;
    if (++s->at >= s->n) { s->run = 0; return s->to; }
    t = (int32_t)((uint32_t)s->at * 32768u / (uint32_t)s->n);
    et = (int32_t)ba_exp2((int32_t)(((int64_t)s->c * t * 94548) >> 15));
    e1 = (int32_t)ba_exp2(s->c * 94548);
    {
        uint32_t nm = (uint32_t)ba_abs(BA_Q16 - et), dn = (uint32_t)ba_abs(BA_Q16 - e1);
        g = dn ? (int32_t)ba_div64(nm >> 17, nm << 15, dn) : BA_Q15;
    }
    return s->from + (int32_t)(((int64_t)(s->to - s->from) * g) >> 15);
}
/* SuperCollider's RLPF at hz, rq (Q12): coefficients, Q29 */
static void ba_rlpf(ba_rc_t *k, int32_t hz, int32_t rq12)
{
    int32_t d = ba_tan_g(ba_clamp((int32_t)(((int64_t)hz * rq12) >> 12), 16, TANG_FMAX)), c;
    uint32_t w = (uint32_t)((uint64_t)(uint32_t)ba_clamp(hz, 1, TANG_FMAX) * BA_PH);
    int32_t cw = ba_sin(w + 0x40000000u);
    {
        uint32_t nm = (uint32_t)ba_abs((1 << 24) - d);
        c = (int32_t)ba_div64(nm >> 3, nm << 29, (uint32_t)((1 << 24) + d));
        c = d > (1 << 24) ? -c : c;
    }
    k->b1 = (int32_t)(((int64_t)((1 << 29) + c) * cw) >> 15);
    k->b2 = -c;
    k->a0 = ((1 << 29) + c - k->b1) >> 2;
}
static inline __attribute__((always_inline)) int32_t ba_rlpf_tick(int32_t *v1, int32_t *v2, int32_t a0, int32_t b1, int32_t b2, int32_t x)
{
    int32_t v = (int32_t)(((int64_t)a0 * x + (int64_t)b1 * *v1 + (int64_t)b2 * *v2) >> 29);
    int32_t y = v + 2 * *v1 + *v2;
    *v2 = *v1; *v1 = v;
    return y;
}
static inline __attribute__((always_inline)) int32_t ba_blep(int32_t t, int32_t dt, uint32_t inv)
{
    int32_t x;
    if (t < dt) {
        x = (int32_t)(((uint64_t)(uint32_t)t * inv) >> 16);
        return (2 * x - (int32_t)(((int64_t)x * x) >> 16) - BA_Q16) >> 1;
    }
    if (t > BA_Q16 - dt) {
        x = -(int32_t)(((uint64_t)(uint32_t)(BA_Q16 - t) * inv) >> 16);
        return ((int32_t)(((int64_t)x * x) >> 16) + 2 * x + BA_Q16) >> 1;
    }
    return 0;
}
static int32_t ba_quad(const int32_t *p, int32_t u)
{
    int32_t v = BA_Q16 - u, a = (int32_t)(((int64_t)v * v) >> 17), c = (int32_t)(((int64_t)u * u) >> 17);
    int32_t b = BA_Q16 - a - c;
    return (int32_t)(((int64_t)a * p[0] + (int64_t)b * p[1] + (int64_t)c * p[2]) >> 16);
}

/* a note: MIDI note -> Hz Q8 (A4 = 440) */
static int32_t ba_note_hz8(int32_t note) { return (int32_t)(((uint64_t)(440u << 8) * ba_exp2(((note - 69) << 16) / 12)) >> 16); }

static void ba_note_on(ba_voice_t *v, uint32_t seed, const ba_design_t *d, int32_t note, int32_t vel127)
{
    __builtin_memset(v, 0, sizeof *v);
    ba_seed(&v->r, seed);
    v->n1[0] = ba_bi(&v->r), v->n1[1] = ba_bi(&v->r), v->n1[2] = ba_bi(&v->r);
    v->n2[0] = ba_bi(&v->r), v->n2[1] = ba_bi(&v->r), v->n2[2] = ba_bi(&v->r);
    v->lfo_rand = ba_bi(&v->r);
    v->f = ba_note_hz8(note);
    v->vel = ba_clamp(vel127, 1, 127) * 258;
    v->live = v->gate = 1;
    if (d->asr) { ba_seg(&v->gte, 0, BA_Q15, d->attack, -4); v->stage_g = 1; }
    else { ba_seg(&v->cut, 0, BA_Q15, 3, 4); ba_seg(&v->amp, 0, BA_Q15, 2, 4); v->stage_c = v->stage_a = 1; }
}
static void ba_note_off(ba_voice_t *v, const ba_design_t *d)
{
    v->gate = 0;
    if (d->asr && v->stage_g) { ba_seg(&v->gte, v->gl0, 0, v->decay_ms ? v->decay_ms : d->decay, -4); v->stage_g = 3; }
}

/* a control block: the LFO, the destinations smoothed, the envelopes stepped, the filters set */
static __attribute__((noinline)) void ba_control(ba_voice_t *v, const ba_design_t *d)
{
    int32_t L, a1, a2, at, pk, dc, sp, lo, top, fc, vel = v->vel, want[9];
    uint32_t sq = v->lfo_ph, step = (uint32_t)(((uint64_t)(uint32_t)d->lfo_hz * BA_LFO) >> 16);
    v->lfo_ph += step;
    switch (d->lfo_shape & 3u) {
    case 0: { uint32_t p = v->lfo_ph >> 17;
              L = p < 8192u ? (int32_t)p * 4 : p < 24576u ? 2 * BA_Q15 - (int32_t)p * 4 : (int32_t)p * 4 - 4 * BA_Q15; break; }
    case 1: L = (int32_t)((v->lfo_ph + 0x80000000u) >> 16) - BA_Q15; break;
    case 2: L = sq < 0x80000000u ? BA_Q15 : 0; break;
    default: L = v->lfo_rand; break;
    }
    if ((v->rph += 2u * step) < 2u * step) v->lfo_rand = ba_bi(&v->r);
    a1 = ba_clamp(d->fm1 + ba_mul(L, d->to[2]) / 2, 0, BA_Q15);
    a2 = ba_clamp(d->fm2 + ba_mul(L, d->to[3]) / 2, 0, BA_Q15);
    want[0] = d->fm1r; want[1] = d->fm2r;
    want[2] = ba_mul(a1, a1); want[3] = ba_mul(a2, a2);
    want[4] = ba_clamp(d->shape + ba_mul(L, d->to[0]), 0, BA_Q15);
    want[5] = ba_clamp(d->folds + ba_mul(L, d->to[1]) * 2 / 3, 0, BA_Q15);
    at = ba_clamp(d->attack + ba_mul(L, d->to[4]) * 2200 / BA_Q15, 3, 8000);
    pk = ba_clamp((int32_t)(((int64_t)d->peak * ba_exp2(ba_mul(L, d->to[5]) * 4)) >> 16), 100, 10000);
    dc = ba_clamp(d->decay + ba_mul(L, d->to[6]) * 2200 / BA_Q15, 10, 8000);
    sp = ba_clamp(d->spring + ba_mul(L, d->to[7]), 0, BA_Q15);
    want[6] = at; want[7] = pk; want[8] = dc;
    if (!v->primed) for (int k = 0; k < 9; k++) v->lag[k] = want[k];
    for (int k = 0; k < 9; k++) v->lag[k] += (int32_t)(((int64_t)(want[k] - v->lag[k]) * BA_LAG5) >> 15);
    v->attack_ms = v->lag[6]; v->decay_ms = v->lag[8];
    v->nu1 += BA_WANDER; v->nu2 += BA_WANDER;
    if (v->nu1 >= BA_Q16) { v->nu1 -= BA_Q16; v->n1[0] = v->n1[1], v->n1[1] = v->n1[2], v->n1[2] = ba_bi(&v->r); }
    if (v->nu2 >= BA_Q16) { v->nu2 -= BA_Q16; v->n2[0] = v->n2[1], v->n2[1] = v->n2[2], v->n2[2] = ba_bi(&v->r); }
    v->pm1 = v->m1, v->pm2 = v->m2, v->pamp1 = v->amp1, v->pamp2 = v->amp2;
    v->pxf = v->xf, v->pfold = v->fold, v->pev = v->ev, v->pmix = v->mix;
    v->m1 = (int32_t)(((int64_t)v->f * v->lag[0]) >> 16);
    v->m1 += (int32_t)(((int64_t)v->m1 * ba_quad(v->n1, v->nu1)) >> 25);
    v->m2 = (int32_t)(((int64_t)v->f * v->lag[1]) >> 16);
    v->m2 += (int32_t)(((int64_t)v->m2 * ba_quad(v->n2, v->nu2)) >> 23);
    v->amp1 = (int32_t)(((int64_t)v->m1 * v->lag[2] * 22) >> 15);
    v->amp2 = (int32_t)(((int64_t)v->m2 * v->lag[3] * 12) >> 15);
    v->xf = v->lag[4] * 3;
    v->fold = 4096 + (int32_t)(((int64_t)v->lag[5] * 8192 * 3) >> 15);
    v->ev = v->lag[4] <= 21845 ? 0 : (v->lag[4] - 21845) * 3;
    v->mix += (int32_t)(((int64_t)(sp - v->mix) * BA_LAG10) >> 15);
    if (!v->primed) {
        v->pm1 = v->m1, v->pm2 = v->m2, v->pamp1 = v->amp1, v->pamp2 = v->amp2;
        v->pxf = v->xf, v->pfold = v->fold, v->pev = v->ev, v->pmix = v->mix = sp;
        v->primed = 1;
    }
    v->cl0 = v->stage_c ? ba_seg_tick(&v->cut) : 0;
    v->al0 = v->stage_a ? ba_seg_tick(&v->amp) : 0;
    if (v->stage_c == 1 && !v->cut.run) { ba_seg(&v->cut, BA_Q15, 0, v->decay_ms, -20); v->stage_c = 2; }
    else if (v->stage_c == 2 && !v->cut.run) v->stage_c = 0;
    if (v->stage_a == 1 && !v->amp.run) { ba_seg(&v->amp, BA_Q15, 0, v->decay_ms, -10); v->stage_a = 2; }
    else if (v->stage_a == 2 && !v->amp.run) v->stage_a = 0;
    if (v->stage_g == 1 || v->stage_g == 3) {
        v->gl0 = ba_seg_tick(&v->gte);
        if (!v->gte.run) v->stage_g = v->stage_g == 1 ? 2 : 0;
    }
    top = (int32_t)(((int64_t)v->lag[7] * (16384 + (vel >> 1))) >> 15);
    lo = top < 300 ? top : 300;
    v->rc0 = v->rc1;
    if (d->asr) { fc = lo + (int32_t)(((int64_t)(top - lo) * v->gl0) >> 15); ba_rlpf(&v->rc1, fc, 3891); }
    else { fc = lo + (int32_t)(((int64_t)(top - lo) * v->cl0) >> 15); ba_rlpf(&v->rc1, fc, 3686); }
    if (v->primed == 1) { v->rc0 = v->rc1; v->primed = 2; }
}

/* one block (32 samples) into out, Q15; 0 once the voice has ended */
static __attribute__((noinline)) int ba_render(ba_voice_t *v, const ba_design_t *d, int32_t *out)
{
    int32_t g0, g1, pk = 0, dt, drive = (int32_t)(((int64_t)d->drive * 6144) >> 12);
    uint32_t inv;
    if (!v->live) return 0;
    {
        int32_t was_a = v->al0, was_g = v->gl0;
        ba_control(v, d);
        g0 = d->asr ? was_g : was_a;
        g1 = d->asr ? v->gl0 : v->al0;
    }
    dt = (int32_t)(((uint64_t)(uint32_t)v->f * BA_PH) >> 24);
    dt = dt < 1 ? 1 : dt;
    inv = 0xFFFFFFFFu / (uint32_t)dt;
    for (int i = 0; i < BA_CTL; i++) {
        int32_t t = (i + 1) * (BA_Q15 / BA_CTL);
        int32_t m1 = v->pm1 + ba_mul(v->m1 - v->pm1, t), m2 = v->pm2 + ba_mul(v->m2 - v->pm2, t);
        int32_t a1 = v->pamp1 + ba_mul(v->amp1 - v->pamp1, t), a2 = v->pamp2 + ba_mul(v->amp2 - v->pamp2, t);
        int32_t fc, x, s, sine, tri, sq, saw, add, fold, u, y, g;
        uint32_t p;
        fc = v->f + ba_mul(ba_sin(v->ph1), a1) + ba_mul(ba_sin(v->ph2), a2);
        v->ph1 += (uint32_t)(((uint64_t)(uint32_t)m1 * BA_PH) >> 8);
        v->ph2 += (uint32_t)(((uint64_t)(uint32_t)m2 * BA_PH) >> 8);
        v->ph += (uint32_t)(int32_t)(((int64_t)fc * (int32_t)BA_PH) >> 8);
        p = v->ph;
        sine = ba_sin(p) >> 1;
        { int32_t q = (int32_t)(p >> 17); tri = (q < 16384 ? 4 * q - BA_Q15 : 3 * BA_Q15 - 4 * q) >> 1; }
        s = v->pxf + ba_mul(v->xf - v->pxf, t);
        if (s > BA_Q15) {
            int32_t ph16 = (int32_t)(p >> 16), ev = v->pev + ba_mul(v->ev - v->pev, t);
            int32_t ph2 = (ph16 + 32768) & 0xFFFF;
            sq = (ph16 < 32768 ? BA_Q15 : -BA_Q15) + ba_blep(ph16, dt, inv) - ba_blep(ph2, dt, inv);
            saw = ph16 - BA_Q15 - ba_blep(ph16, dt, inv);
            sq = ba_mul(sq, 25736);
            saw = -ba_mul(saw, 51472);
            add = sq + ba_mul(saw - sq, ev);
        } else add = 0;
        if (s <= BA_Q15) x = sine + ba_mul(tri - sine, s);
        else if (s <= 2 * BA_Q15) x = tri + ba_mul(add - tri, s - BA_Q15);
        else x = add;
        fold = v->pfold + (int32_t)(((int64_t)(v->fold - v->pfold) * t) >> 15);
        u = ((int32_t)(((int64_t)x * fold) >> 12) + 16384) & 0xFFFF;
        x = (u < BA_Q15 ? u : 2 * BA_Q15 - u) - 16384;
        y = (int32_t)(((int64_t)BA_BW[0] * x + (int64_t)BA_BW[1] * v->bx1 + (int64_t)BA_BW[2] * v->bx2 - (int64_t)BA_BW[3] * v->by1 -
                       (int64_t)BA_BW[4] * v->by2) >> 29);
        v->bx2 = v->bx1, v->bx1 = x, v->by2 = v->by1, v->by1 = y;
        v->pk += (ba_bi(&v->r) - v->pk) >> 3;
        x = y + (v->pk * 3 >> 9);
        {
            int32_t a0 = v->rc0.a0 + ba_mul(v->rc1.a0 - v->rc0.a0, t), b1 = v->rc0.b1 + ba_mul(v->rc1.b1 - v->rc0.b1, t);
            int32_t b2 = v->rc0.b2 + ba_mul(v->rc1.b2 - v->rc0.b2, t);
            if (d->asr) x = ba_rlpf_tick(&v->sv1[1], &v->sv2[1], a0, b1, b2, ba_rlpf_tick(&v->sv1[0], &v->sv2[0], a0, b1, b2, x));
            else x = ba_rlpf_tick(&v->lv1, &v->lv2, a0, b1, b2, x);
        }
        g = g0 + ba_mul(g1 - g0, t);
        x = ba_mul(x, g);
        x = ba_tanh((int32_t)(((int64_t)x * drive) >> 12));
        if (ba_abs(x) > 16384)
            x = x > 0 ? BA_Q15 - (int32_t)(0x10000000u / (uint32_t)x) : -(BA_Q15 - (int32_t)(0x10000000u / (uint32_t)(-x)));
        {
            int32_t bank = 0, mix = v->pmix + ba_mul(v->mix - v->pmix, t);
            for (int k = 0; k < 3; k++) {
                int32_t *r = v->rs[k];
                int32_t vv = x + (int32_t)(((int64_t)d->res[k][0] * r[0] + (int64_t)d->res[k][1] * r[1]) >> 30);
                bank += ba_mul(vv - r[1], BA_RES[k][2]);
                r[1] = r[0]; r[0] = vv;
            }
            x += ba_mul(ba_tanh(bank), ba_mul(mix, 11469));
        }
        x = (int32_t)(((int64_t)x * d->out) >> 12);
        v->dcy += ((x - v->dcx) << 8) - (int32_t)(((int64_t)v->dcy * 43) >> 15);   /* 10 Hz high-pass */
        v->dcx = x;
        x = v->dcy >> 8;
        out[i] = x;
        pk = ba_abs(x) > pk ? ba_abs(x) : pk;
    }
    if (!(d->asr ? v->stage_g : (v->stage_a || v->stage_c)))
        v->quiet = pk < 8 ? v->quiet + 1 : 0;
    if (v->quiet > 64) v->live = 0;
    return 1;
}

/* a design of kind (or drawn from the path's weights, kind < 0) from seed */
static __attribute__((noinline, minsize)) void ba_design(ba_design_t *d, int32_t kind, uint32_t seed, uint32_t path)
{
    ba_rnd_t rr, *r = &rr;
    int32_t sl[8];
    path = path <= BA_EXP ? path : BA_OPEN;
    ba_seed(r, seed ^ 0xBA7A4Eu);
    if (kind < 0 || kind >= BK_N) {
        uint32_t total = 0, x;
        for (int k = 0; k < BK_N; k++) total += BK_W[path][k];
        x = (uint32_t)ba_below(r, (int32_t)total);
        for (kind = 0; kind + 1 < BK_N && x >= BK_W[path][kind]; kind++) x -= BK_W[path][kind];
    }
    __builtin_memset(d, 0, sizeof *d);
    d->seed = seed; d->kind = (uint8_t)kind; d->path = (uint8_t)path;
    for (int k = 0; k < 8; k++) sl[k] = ba_in(r, BK_R[kind][k][0] * BA_Q15 / 100, BK_R[kind][k][1] * BA_Q15 / 100);
    d->shape = sl[0]; d->folds = sl[1]; d->fm1 = sl[2]; d->fm2 = sl[3];
    d->peak = (int32_t)((100u * ba_exp2((int32_t)(((int64_t)sl[4] * 435411) >> 15))) >> 16);   /* 100 x 100^v */
    d->decay = (int32_t)((10u * ba_exp2((int32_t)(((int64_t)sl[5] * 632071) >> 15))) >> 16);   /* 10 ms x 800^v */
    d->drive = (int32_t)(((int64_t)sl[6] * 11 * 4096) >> 15) + 410;
    d->spring = sl[7];
    d->fm1r = 43254; d->fm2r = 216269;            /* 0.66, 3.3 */
    if (ba_chance(r, path == BA_EXP ? 60 : 30)) { d->fm1r = ba_in(r, 6554, BA_Q16); d->fm2r = ba_in(r, BA_Q16, 10 * BA_Q16); }
    d->asr = (uint8_t)(kind == BK_SWELL || kind == BK_DRONE);
    { int32_t x = ba_q15(r), x2 = ba_mul(x, x); d->attack = 3 + ba_mul(ba_mul(x2, x2), 8000); }
    if (kind == BK_SWELL) d->attack = ba_in(r, 300, 3000);
    else if (kind == BK_DRONE) d->attack = ba_in(r, 500, 4000);
    d->lfo_shape = (uint8_t)ba_below(r, 4);
    d->lfo_hz = ba_in(r, 66, 10 * BA_Q16);
    if (path == BA_AMBIENT) d->lfo_hz = ba_in(r, 66, BA_Q16);
    for (int k = 0; k < 8; k++) d->to[k] = ba_chance(r, k == 2 || k == 3 ? 25 : 35) ? ba_q15(r) : 0;
    if (path == BA_EXP) { d->drive += 4096; d->folds = ba_clamp(d->folds + ba_in(r, 0, BA_Q15 / 3), 0, BA_Q15); }
    d->out = 4096;
    /* the resonators: three of this sound's own (Tonitor's fixed 508/270/1153 Hz gave every sound the same
       ring): f0 120-3000 Hz, then x 1.3-2.7 and x 2.2-5.4 (inharmonic), ringing 0.1-1.2 s each.
       Pole radius r = 1 - ln 1000 / (T60 FS) (Q30: 2^30 - 154527000 / T60 ms); c1 = 2 r cos w, c2 = -r^2. */
    {
        int32_t f = ba_in(r, 120, 3000);
        for (int k = 0; k < 3; k++) {
            int32_t rq = (1 << 30) - 154527000 / ba_in(r, 100, 1200), cw = ba_sin((uint32_t)ba_clamp(f, 20, TANG_FMAX) * BA_PH + 0x40000000u);
            d->res[k][0] = (int32_t)(((int64_t)rq * cw) >> 14);
            d->res[k][1] = -(int32_t)(((int64_t)rq * rq) >> 30);
            f = (int32_t)(((int64_t)f * ba_in(r, k ? 143360 : 85197, k ? 354419 : 176947)) >> 16);
        }
    }
}

/* ------------------------------------------------------ for the Rhythm --- */
/* What SteamRoller asks BAYAREA for: textures and strange noises first
   (poundhard covers the plainly percussive), a few industrial hits kept.
   Eight families (track buttons 1-8 in generate mode) and the kits' blip:
   - clunk: a short, folded knock;
   - scrape: deep inharmonic FM (noise-like) held, its filter and folds swept
     by a slow LFO;
   - hiss: high inharmonic FM static, short to medium;
   - rumble: a low folded drone, slowly moving;
   - whir: a held tone whose shape and folds an LFO churns (3-10 Hz);
   - clang: inharmonic metal, its own resonators;
   - chop: a held fold whose timbre a square LFO jumps (4-10 Hz);
   - gnarly: any kind on the experimental path.
   Each sound has resonators of its own (ba_design), and most families none
   at all. No pitch moves within a sound: the modulators are never below the
   note and the LFO never moves the FM depths. A family is one row of
   BA_FAM; the held families are let go after their hold (within the
   generator's 2 s). */
enum { BAF_CLUNK, BAF_SCRAPE, BAF_HISS, BAF_RUMBLE, BAF_WHIR, BAF_CLANG, BAF_CHOP, BAF_GNARLY, BAF_BLIP, BAF_N };
typedef struct { ba_voice_t v; ba_design_t d; int32_t hold; } BaSlot;   /* hold: blocks to note-off, -1 none */
enum { BF_INHARM = 1, BF_SQUARE = 2, BF_EXP = 4, BF_DRIVE = 8 };
/* kind (0xFF any), note lo/hi, decay lo/hi (x 10 ms; 0: the design's), peak lo/hi (x 100 Hz; 0: the design's),
   folds and FM depth at least (x 128, Q15), resonators (x 128; 0: none), hold lo/hi (x 10 ms; 0: not held),
   LFO lo/hi (x 0.1 Hz; 0: the design's), LFO targets (bits: 0 shape, 1 folds, 5 peak), flags */
static const uint8_t BA_FAM[BAF_N][16] = {
    {BK_BONGO, 43, 65, 2, 8, 9, 35, 90, 0, 0, 0, 0, 0, 0, 0, BF_DRIVE},                      /* clunk */
    {BK_FOLDED, 40, 70, 20, 60, 10, 60, 40, 200, 30, 30, 90, 5, 60, 0x22, BF_INHARM},       /* scrape */
    {BK_PLUCK, 72, 96, 6, 40, 40, 100, 60, 220, 0, 0, 0, 0, 0, 0, BF_INHARM},               /* hiss */
    {BK_DRONE, 24, 40, 30, 70, 2, 8, 120, 0, 40, 40, 100, 1, 10, 0x22, 0},                   /* rumble */
    {BK_SWELL, 36, 64, 15, 50, 10, 50, 0, 0, 0, 40, 110, 30, 100, 0x03, 0},                  /* whir */
    {BK_CHIME, 50, 78, 8, 45, 30, 90, 20, 90, 200, 0, 0, 0, 0, 0, BF_INHARM},               /* clang */
    {BK_FOLDED, 36, 70, 10, 30, 0, 0, 60, 60, 0, 30, 90, 40, 100, 0x23, BF_SQUARE},         /* chop */
    {0xFF, 33, 61, 0, 0, 0, 0, 0, 0, 160, 0, 0, 0, 0, 0, BF_EXP},                            /* gnarly */
    {BK_PLUCK, 64, 89, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},                               /* blip */
};
static __attribute__((noinline, minsize)) void ba_strike(BaSlot *s, unsigned baf, uint32_t seed)
{
    ba_rnd_t rr, *r = &rr;
    ba_design_t *d = &s->d;
    baf = baf < BAF_N ? baf : BAF_CLUNK;
    const uint8_t *f = BA_FAM[baf];
    ba_seed(r, seed ^ 0xB1A5u);
    ba_design(d, f[0] == 0xFF ? -1 : f[0], seed, f[15] & BF_EXP ? BA_EXP : BA_OPEN);
    int32_t note = ba_in(r, f[1], f[2]), hold = f[10] ? ba_in(r, f[10], f[11]) * 10 : 0;
    if (f[3]) d->decay = ba_in(r, f[3], f[4]) * 10;
    if (f[5]) d->peak = ba_in(r, f[5], f[6]) * 100;
    if (d->folds < f[7] * 128) d->folds = ba_in(r, f[7] * 128, BA_Q15);
    if (f[8]) { d->fm1 = ba_in(r, f[8] * 128, BA_Q15); d->fm2 = ba_in(r, f[8] * 128, BA_Q15); }
    d->spring = f[9] ? ba_in(r, f[9] * 32, f[9] * 128) : 0;
    if (!f[9]) d->to[7] = 0;
    if (f[12]) d->lfo_hz = ba_in(r, f[12] * 6554, f[13] * 6554);
    if (f[15] & BF_SQUARE) d->lfo_shape = 2;
    if (f[14]) for (int k = 0; k < 8; k++) d->to[k] = f[14] >> k & 1 ? ba_in(r, 16384, BA_Q15) : 0;
    if (f[15] & BF_INHARM) { d->fm1r = ba_in(r, 91750, 235930); d->fm2r = ba_in(r, 176947, 622592); }   /* 1.4-3.6, 2.7-9.5 */
    if (d->fm1r < BA_Q16) d->fm1r = ba_in(r, BA_Q16, 2 * BA_Q16);   /* (Tonitor's 0.66 and below: vibrato on low notes) */
    if (f[15] & BF_DRIVE) d->drive += 2048;
    d->to[2] = d->to[3] = 0;
    s->hold = -1;
    if (hold) d->asr = 1;
    if (d->asr) {
        d->attack = ba_in(r, 20, d->kind == BK_DRONE || d->kind == BK_SWELL ? 600 : 150);
        d->decay = ba_clamp(d->decay, 60, 700);
        d->to[4] >>= 2; d->to[6] >>= 2;                   /* (the LFO moves them less) */
        s->hold = (d->attack + (hold ? hold : ba_in(r, 150, 400))) * 3 / 2;
    }
    ba_note_on(&s->v, seed, d, note, 127);
}
/* One 32-sample block, Q15; 0 once the sound has ended. */
static int ba_block(BaSlot *s, int32_t *out)
{
    if (s->hold >= 0 && s->hold-- == 0) ba_note_off(&s->v, &s->d);
    return ba_render(&s->v, &s->d, out);
}
