/* SPDX-License-Identifier: GPL-3.0-only
 * Circuit Rhythm port of Vascular's poundhard FM drum voice. See poundhard.h
 * for what is kept and what changed. Freestanding: no libm, its own small
 * approximations below. Single-precision floats (Cortex-M7 FPU).
 */
#include "poundhard.h"
#include "generated/poundhard_tables.h"

/* Strike-rate and control-rate code is compiled for size (it runs once per
   strike or every 16 samples); the per-sample path for speed. */
#define PH_SMALL __attribute__((minsize, noinline))
/* The Rhythm's own rate: one voice is rendered at a time (audition, or a
   generated sample), so the full 48 kHz is affordable again. */
#define PH_SR 48000.0f
#define PH_DT (1.0f / PH_SR)
#define PH_CTL 16u
#define PH_PI 3.14159265f
#define PH_TWO_PI 6.28318531f

/* ------------------------------------------------------------ small math --- */
static float ph_clamp(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static float ph_abs(float x) { return x < 0 ? -x : x; }
static float ph_floor(float x) { int i = (int)x; return (float)(x < (float)i ? i - 1 : i); }
/* 2^x, |x| < 126: the nearest integer into the exponent, the remainder in
   [-0.5, 0.5] by a 7th-order series (< 2e-8 relative; exact to float near 0,
   where the per-sample decay multipliers live). */
static float ph_exp2(float x)
{
    if (x < -126.0f) return 0.0f;
    if (x > 126.0f) x = 126.0f;
    float fi = ph_floor(x + 0.5f), f = x - fi;
    float p = 1.0f + f * (0.693147181f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f
              + f * (0.00133335581f + f * (0.000154035304f + f * 0.0000152527338f))))));
    union { float f; uint32_t u; } v = {p};
    v.u += (uint32_t)((int32_t)fi << 23);
    return v.f;
}
static float ph_exp(float x) { return ph_exp2(x * 1.44269504f); }
/* log2 of x > 0 (~1e-5). */
static float ph_log2(float x)
{
    union { float f; uint32_t u; } v = {x};
    float e = (float)((int32_t)((v.u >> 23) & 255u) - 127);
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;   /* mantissa in [1, 2) */
    float m = v.f, t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    return e + 2.88539008f * t * (1.0f + t2 * (0.333333333f + t2 * (0.2f + t2 * (0.142857143f + t2 * 0.111111111f))));
}
static float ph_pow(float a, float b) { return a > 0 ? ph_exp2(b * ph_log2(a)) : 0.0f; }
static float ph_tanh(float x)
{
    if (x > 4.97f) return 1.0f;
    if (x < -4.97f) return -1.0f;
    float x2 = x * x;     /* Lambert's continued fraction, 7 terms: < 2e-4 */
    return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) / (135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f)));
}
/* sin and cos on [-pi, pi] by odd/even Taylor series in x/2, doubled: < 1e-6 near pi/2. */
static float ph_tan(float x)    /* 0 <= x < 1.42 (the filters' pi * hz / sr, hz <= 0.45 sr) */
{
    float h = 0.5f * x, h2 = h * h;
    float s = h * (1.0f - h2 * (0.166666667f - h2 * (0.00833333333f - h2 * 0.000198412698f)));
    float c = 1.0f - h2 * (0.5f - h2 * (0.0416666667f - h2 * 0.00138888889f));
    return 2.0f * s * c / (c * c - s * s);   /* tan(2h) */
}
static float ph_g(float hz) { return ph_tan(PH_PI * ph_clamp(hz, 5.0f, PH_SR * 0.45f) / PH_SR); }
/* sin of a 2^32-per-cycle phase, from the FM core's 1025-point Q15 table (in ITCM). */
static float ph_sin(uint32_t phase)
{
    const int16_t *s = &SINE[phase >> 22];
    int32_t a = s[0], d = s[1] - a, f = (int32_t)((phase >> 6) & 0xffffu);
    return (float)(a + ((d * f) >> 16)) * (1.0f / 32768.0f);
}

/* --------------------------------------------------------- measured laws --- */
PH_SMALL static float ph_lookup(const float *at, const float *val, int n, float v, int logInterp)
{
    if (v <= at[0]) return val[0];
    for (int i = 1; i < n; ++i)
        if (v <= at[i]) {
            float t = (v - at[i - 1]) / (at[i] - at[i - 1]);
            if (logInterp && val[i - 1] > 0 && val[i] > 0 && val[i] < 1e8f)
                return val[i - 1] * ph_pow(val[i] / val[i - 1], t);
            return val[i - 1] + (val[i] - val[i - 1]) * t;
        }
    return val[n - 1];
}
static float ph_decay60(float v) { return v >= 126.5f ? 1e9f : ph_lookup(ph_decay_at, ph_decay_t60, 17, v, 1); }
static float ph_hold(float v) { return ph_lookup(ph_hold_at, ph_hold_seconds, 9, v, 0); }
static float ph_sweep_time(float v) { return ph_lookup(ph_sweep_at, ph_sweep_tau, 9, v, 1); }
static float ph_cutoff_hz(float v) { return 328.0f * ph_exp2((v - 64.0f) * 0.0951f); }
static float ph_base_hz(float v) { return ph_clamp(ph_cutoff_hz(v), 5.0f, 8500.0f); }
static float ph_top_knob(float base, float w)
{
    float k = base + 25 + 0.8f * w + 15 * (w / 127) * (w / 127);
    return k > 51 ? k : 51;
}
static float ph_sq(float v) { float x = ph_clamp(v, 0, 127) / 127.0f; return x * x; }
/* Per-sample decay multiplier for a -60 dB time (exp(-6.9078 dt / t60)). */
static float ph_mul60(float t60) { return t60 >= 1e8f ? 1.0f : ph_exp(-6.9078f * PH_DT / t60); }

/* ------------------------------------------------------------- the seeds --- */
typedef struct { uint64_t s; } PhRng;
static uint64_t ph_next(PhRng *r) { r->s ^= r->s << 13; r->s ^= r->s >> 7; r->s ^= r->s << 17; return r->s; }
static void ph_seed(PhRng *r, uint64_t v)
{
    r->s = v * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
    if (!r->s) r->s = 1;
    for (int i = 0; i < 3; ++i) ph_next(r);
}
static float ph_unit(PhRng *r) { return (float)(uint32_t)(ph_next(r) >> 40) * (1.0f / 16777216.0f); }
static float ph_range(PhRng *r, float a, float b) { return a + (b - a) * ph_unit(r); }
static int ph_chance(PhRng *r, float p) { return ph_unit(r) < p; }
static int ph_pick(PhRng *r, int n) { return (int)(ph_unit(r) * (float)n) % n; }
/* audio noise: 32-bit xorshift, uniform -1..1 */
static float ph_noise(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* ---------------------------------------------------------- the patches --- */
PH_SMALL unsigned ph_role_kind(unsigned role, uint32_t seed)
{
    PhRng r;
    ph_seed(&r, (uint64_t)seed ^ 0x52484f4cull);
    switch (role & 7u) {
    case PH_ROLE_KICK: return PH_KICK;
    case PH_ROLE_SNARE: return PH_SNARE;
    case PH_ROLE_HAT: return PH_HIHAT;
    case PH_ROLE_CLAP: return PH_CLAP;
    case PH_ROLE_TOM: return PH_TOM;
    case PH_ROLE_METAL: { static const uint8_t k[3] = {PH_CLANG, PH_METAL, PH_ANVIL}; return k[ph_pick(&r, 3)]; }
    case PH_ROLE_NOISE: return PH_RUST;
    default: return ph_pick(&r, 2) ? PH_GLITCH : PH_ZAP;
    }
}

enum { F_MULTI, F_LP4, F_LEGACY, F_COMB_MINUS, F_COMB_PLUS, F_EQ };

/* poundhard `design`, the ranges each kind is drawn in, with the LFO seed 0. */
PH_SMALL void ph_design(PhPatch *p, unsigned kind, uint32_t seed)
{
    PhRng rr, *r = &rr;
    ph_seed(r, (uint64_t)seed ^ (0xD0D0ull + (uint64_t)kind * 977));
#define R(a, b) ph_range(r, (float)(a), (float)(b))
    PhPatch d = {0};
    d.stim = 30; d.ratioA = 4; d.decA = 40; d.ratioB = 8; d.decB = 40; d.decay = 60; d.level = 100;
    d.noiseDecay = 40; d.noiseWidth = 127; d.cutoff = 127; d.reg = 1;
    *p = d;
    p->kind = (uint8_t)kind;
    /* common: a struck amp envelope; the draws for reverb and delay are kept so
       every later draw is the one poundhard makes, though the sends are gone */
    p->aHold = R(40, 90); p->aDec = R(60, 100); p->volume = 110;
    p->filter = F_MULTI; p->cutoff = 127; p->fH = 0; p->fG = 0;
    (void)R(30, 90); (void)R(0, 30); (void)R(50, 120);
    if (!ph_chance(r, 0.5f)) (void)ph_chance(r, 0.5f);
    (void)R(20, 70);
    switch (kind) {
    case PH_KICK: {
        p->reg = 1;
        p->algo = (uint8_t)ph_pick(r, 4); p->waveC = ph_chance(r, 0.7f) ? 0 : R(0, 30); p->waveAB = R(0, 40);
        p->sdep = R(45, 95); p->stim = R(18, 55);
        p->ratioA = (float)(2 + ph_pick(r, 6)); p->modA = R(0, 60); p->decA = R(10, 40);
        p->ratioB = (float)(4 + ph_pick(r, 12)); p->modB = R(0, 40); p->decB = R(5, 30);
        p->feedback = R(0, 40); p->foldAmt = ph_chance(r, 0.5f) ? R(0, 50) : 0;
        p->hold = R(0, 20); p->decay = R(45, 85);
        p->transient = R(0, 22); p->transientLevel = R(40, 95);
        p->noiseLevel = ph_chance(r, 0.4f) ? R(10, 40) : 0; p->noiseDecay = R(5, 25); p->noiseBase = R(40, 80);
        p->overdrive = ph_chance(r, 0.5f) ? R(0, 60) : 0;
        if (ph_chance(r, 0.2f)) (void)R(0, 30);
        {   /* two kicks in five hard: driven and folded */
            PhRng hh, *h = &hh;
            ph_seed(h, (uint64_t)seed ^ 0x6ABBE5ull);
            if (ph_chance(h, 0.4f)) {
                float fb;
                p->overdrive = ph_range(h, 85, 127);
                p->foldAmt = ph_range(h, 25, 75);
                fb = ph_range(h, 20, 60);
                if (fb > p->feedback) p->feedback = fb;
                p->odPost = (uint8_t)ph_chance(h, 0.5f);
                p->volume = 100;
            }
        }
        break;
    }
    case PH_SNARE:
        p->reg = 2;
        p->algo = (uint8_t)ph_pick(r, 7); p->waveC = R(0, 60); p->waveAB = R(0, 80);
        p->sdep = R(10, 50); p->stim = R(5, 30);
        p->ratioA = R(4, 30); p->modA = R(20, 80); p->decA = R(10, 40);
        p->ratioB = R(6, 60); p->modB = R(20, 80); p->decB = R(10, 40);
        p->feedback = R(0, 60); p->foldAmt = R(0, 60);
        p->decay = R(25, 55);
        p->noiseLevel = R(70, 105); p->noiseDecay = R(30, 62); p->noiseHold = R(0, 20);
        p->noiseBase = R(50, 85); p->noiseWidth = R(50, 110); p->grain = ph_chance(r, 0.3f) ? R(60, 90) : 0;
        p->transient = R(8, 34); p->transientLevel = R(30, 80);
        if (ph_chance(r, 0.4f)) (void)R(10, 60);
        break;
    case PH_HIHAT: {
        int open;
        p->reg = 8;
        p->algo = (uint8_t)(4 + ph_pick(r, 3)); p->waveC = R(40, 127); p->waveAB = R(30, 127);
        p->ratioA = R(20, 127); p->modA = R(60, 127); p->decA = R(20, 60); p->endA = R(0, 60);
        p->ratioB = R(20, 127); p->modB = R(40, 100); p->decB = R(10, 40);
        p->feedback = R(30, 100); p->foldAmt = R(20, 90); p->level = R(40, 80);
        open = ph_chance(r, 0.35f);
        p->decay = open ? R(45, 70) : R(10, 35);
        p->noiseLevel = R(80, 110); p->noiseDecay = open ? R(48, 72) : R(15, 40);
        p->noiseBase = R(85, 112); p->noiseWidth = R(60, 127);
        p->filter = F_MULTI; p->cutoff = R(80, 110); p->fH = R(64, 127); p->fG = R(0, 50);
        p->transient = R(51, 70); p->transientLevel = R(0, 50);
        break;
    }
    case PH_CLAP:
        p->reg = 4;
        p->level = R(0, 40); p->decay = R(10, 30);
        p->noiseLevel = R(85, 110); p->noiseDecay = R(30, 58); p->grain = R(50, 95);
        p->noiseBase = R(60, 85); p->noiseWidth = R(30, 80);
        p->transient = R(21, 32); p->transientLevel = R(20, 60);
        p->lfoOn = 1; p->lfoSpeed = 32; p->lfoMult = (float)(7 + ph_pick(r, 2)); p->lfoDepth = -R(30, 60);
        (void)R(20, 70);
        break;
    case PH_TOM:
        p->reg = 2;
        p->algo = (uint8_t)ph_pick(r, 4); p->waveC = R(0, 30); p->waveAB = R(0, 50);
        p->sdep = R(20, 60); p->stim = R(30, 70);
        p->ratioA = (float)(4 + ph_pick(r, 8)); p->modA = R(0, 60); p->decA = R(20, 50);
        p->decay = R(50, 80); p->foldAmt = R(0, 40);
        p->transient = R(0, 20); p->transientLevel = R(20, 70);
        p->noiseLevel = R(0, 40); p->noiseDecay = R(10, 30);
        break;
    case PH_CLANG:
        p->reg = 4;
        p->algo = (uint8_t)(4 + ph_pick(r, 3)); p->waveC = R(0, 127); p->waveAB = R(0, 127);
        p->ratioA = R(5, 60); p->modA = R(50, 110); p->decA = R(40, 80); p->endA = R(0, 50);
        p->ratioB = R(5, 60); p->modB = R(60, 110); p->decB = R(40, 90); p->endB = R(0, 40);
        p->feedback = R(20, 90); p->foldAmt = R(20, 80);
        p->decay = R(55, 90); p->transient = R(13, 34); p->transientLevel = R(30, 80);
        p->filter = ph_chance(r, 0.5f) ? F_COMB_PLUS : F_MULTI; p->cutoff = R(60, 110); p->fG = R(30, 90); p->fH = R(60, 120);
        (void)R(10, 60);
        break;
    case PH_METAL:
        p->reg = 4;
        p->algo = (uint8_t)(4 + ph_pick(r, 3)); p->waveC = R(60, 127); p->waveAB = R(0, 127);
        p->ratioA = R(5, 127); p->modA = R(40, 90); p->decA = R(60, 100); p->endA = R(20, 80);
        p->ratioB = R(5, 127); p->modB = R(50, 100); p->decB = R(70, 110); p->endB = R(20, 80);
        p->feedback = R(0, 60); p->foldAmt = R(0, 50);
        p->decay = R(75, 110); p->noiseLevel = R(0, 40); p->noiseDecay = R(20, 50); p->noiseBase = R(90, 115);
        (void)R(20, 80); if (ph_chance(r, 0.3f)) (void)R(10, 50);
        break;
    case PH_ANVIL:
        p->reg = 8;
        p->algo = (uint8_t)ph_pick(r, 7); p->waveC = R(0, 127); p->waveAB = R(30, 127);
        p->ratioA = R(10, 127); p->modA = R(70, 127); p->decA = R(10, 40);
        p->ratioB = R(10, 127); p->modB = R(70, 127); p->decB = R(10, 40);
        p->feedback = R(40, 110); p->foldAmt = R(50, 127);
        p->decay = R(30, 60); p->transient = R(21, 34); p->transientLevel = R(60, 110);
        p->overdrive = R(20, 80); p->filter = F_MULTI; p->cutoff = R(70, 115); p->fH = R(40, 90); p->fG = R(20, 80);
        break;
    case PH_RUST:
        p->reg = 2;
        p->algo = (uint8_t)ph_pick(r, 7); p->waveC = R(0, 127); p->waveAB = R(0, 127);
        p->ratioA = R(1, 127); p->modA = R(60, 127); p->decA = R(20, 70);
        p->ratioB = R(1, 127); p->modB = R(60, 127); p->decB = R(20, 70);
        p->feedback = R(80, 127); p->foldAmt = R(30, 110);
        p->decay = R(30, 70); p->noiseLevel = R(50, 100); p->grain = R(70, 120); p->noiseDecay = R(30, 70);
        p->noiseRing = (uint8_t)ph_chance(r, 0.5f); p->noiseBase = R(30, 90);
        p->filter = F_COMB_MINUS; p->cutoff = R(50, 100); p->fG = R(40, 100); p->fH = R(60, 120);
        p->bits = ph_chance(r, 0.4f) ? R(30, 70) : 0;
        break;
    case PH_ZAP:
        p->reg = 2;
        p->algo = (uint8_t)ph_pick(r, 4); p->waveC = R(0, 80); p->waveAB = R(0, 80);
        p->sdep = R(90, 127); p->stim = R(5, 40);
        p->ratioA = R(2, 40); p->modA = R(30, 100); p->decA = R(5, 30);
        p->feedback = R(0, 80); p->foldAmt = R(0, 90);
        p->decay = R(20, 50); p->transient = R(8, 20); p->transientLevel = R(0, 60);
        if (ph_chance(r, 0.4f)) (void)R(20, 60);
        break;
    default: /* glitch */
        p->kind = PH_GLITCH;
        p->reg = (float)(1 << ph_pick(r, 4));
        p->algo = (uint8_t)ph_pick(r, 7); p->waveC = R(0, 127); p->waveAB = R(0, 127);
        p->sdep = R(0, 100); p->stim = R(0, 40);
        p->ratioA = R(1, 127); p->modA = R(0, 127); p->decA = R(0, 40);
        p->ratioB = R(1, 127); p->modB = R(0, 127); p->decB = R(0, 40);
        p->feedback = R(0, 127); p->foldAmt = R(0, 127);
        p->decay = R(10, 50); p->noiseLevel = R(0, 90); p->grain = R(60, 127); p->noiseDecay = R(5, 40);
        p->bits = R(20, 90); p->srr = R(10, 80); p->srrPost = (uint8_t)ph_chance(r, 0.5f); p->overdrive = R(0, 70);
        p->transient = R(0, 127); p->transientLevel = R(0, 90);
        break;
    }
#undef R
}

/* ------------------------------------------------------- the character --- */
/* Treatments: each pushes a few of the patch's knobs into a region poundhard's
   own draws rarely or never reach. Archetypes combine them per role. */
enum {
    T_FOLD = 1u << 0, T_DRIVE = 1u << 1, T_CRUSH = 1u << 2, T_DECIM = 1u << 3,
    T_COMB = 1u << 4, T_LADDER = 1u << 5, T_RING = 1u << 6, T_CHAOS = 1u << 7,
    T_INHARM = 1u << 8, T_GRAIN = 1u << 9, T_REG = 1u << 10, T_WAVE = 1u << 11,
    T_PEAK = 1u << 12, T_SHORT = 1u << 13, T_LONG = 1u << 14, T_DARK = 1u << 15,
    T_BODY = 1u << 16, T_GATE = 1u << 17, T_BRIGHT = 1u << 18,
};
/* eight archetypes per role; 0 = poundhard's own design */
static const uint32_t ph_archetype[PH_ROLES][8] = {
    /* kick: used on the gnarly row only (the plain kick stays poundhard's) */
    { T_FOLD | T_DRIVE, T_CRUSH | T_BODY, T_DECIM | T_DRIVE, T_COMB | T_LONG,
      T_LADDER | T_DRIVE, T_CHAOS | T_FOLD, T_RING | T_GRAIN, T_INHARM | T_FOLD },
    /* snare */
    { 0, T_SHORT | T_BRIGHT, T_BODY | T_LONG, T_RING | T_WAVE, T_INHARM | T_SHORT, T_COMB, T_DRIVE | T_FOLD, T_GATE },
    /* hat */
    { 0, T_SHORT, T_CRUSH | T_DECIM, T_LONG, T_INHARM | T_WAVE, T_RING, T_COMB | T_BRIGHT, T_DARK },
    /* clap */
    { 0, T_SHORT, T_LONG | T_GRAIN, T_CRUSH, T_COMB, T_DRIVE, T_RING | T_BODY, T_DARK },
    /* tom */
    { 0, T_BODY | T_LONG, T_REG | T_SHORT, T_INHARM, T_FOLD, T_COMB, T_LADDER, T_DECIM },
    /* metal */
    { 0, T_COMB, T_DRIVE | T_FOLD, T_CRUSH, T_REG, T_RING, T_PEAK, T_CHAOS },
    /* noise */
    { 0, T_LADDER, T_DECIM | T_CRUSH, T_GRAIN | T_RING, T_CHAOS | T_FOLD, T_PEAK, T_GATE, T_REG | T_DARK },
    /* click */
    { 0, T_DECIM, T_COMB, T_CHAOS, T_FOLD | T_DRIVE, T_LADDER, T_REG, T_INHARM },
};
/* extra treatments: mild ones may join a character drum, harsh ones a gnarly one */
static const uint32_t ph_mild[8] = { T_WAVE, T_REG, T_GRAIN, T_COMB, T_DRIVE, T_FOLD, T_DARK, T_BRIGHT };
static const uint32_t ph_harsh[12] = { T_FOLD, T_DRIVE, T_CRUSH, T_DECIM, T_COMB, T_LADDER,
                                       T_RING, T_CHAOS, T_INHARM, T_GRAIN, T_REG, T_PEAK };

PH_SMALL static void ph_treat(PhPatch *p, PhRng *r, uint32_t t, unsigned role)
{
    /* the lowest cutoff a dark filter may take: high voices keep their band */
    const float floor = role == PH_ROLE_HAT ? 62 : role == PH_ROLE_CLAP || role == PH_ROLE_SNARE ? 50 : 35;
#define R(a, b) ph_range(r, (float)(a), (float)(b))
#define MAX(field, v) do { const float m_ = (v); if (p->field < m_) p->field = m_; } while (0)
    if (t & T_SHORT) { p->decay = R(5, 25); p->aHold = R(10, 40); p->aDec = R(35, 65); p->noiseDecay = R(8, 28); }
    if (t & T_LONG) { MAX(decay, R(70, 100)); p->aHold = R(70, 100); p->aDec = R(90, 112); MAX(noiseDecay, R(55, 82)); }
    if (t & T_GATE) { p->noiseHold = R(45, 80); p->noiseDecay = R(4, 18); p->aHold = R(75, 100); p->aDec = R(15, 35); MAX(noiseLevel, R(75, 105)); }
    if (t & T_BODY) { p->sdep = R(35, 85); p->stim = R(10, 45); MAX(level, R(60, 100)); MAX(decay, R(40, 75)); }
    if (t & T_WAVE) { p->waveC = R(50, 127); p->waveAB = R(50, 127); }
    if (t & T_INHARM) {   /* ratios off the harmonic grid, deeper modulation */
        p->ratioA = R(1, 127); p->ratioB = R(1, 127);
        MAX(modA, R(60, 120)); MAX(modB, R(50, 120)); MAX(level, R(50, 90));
        p->algo = (uint8_t)ph_pick(r, 7);
    }
    if (t & T_CHAOS) {    /* both modulators fed back hard */
        p->feedback = R(96, 127); p->algo = ph_chance(r, 0.5f) ? 1 : 6;
        MAX(modA, R(70, 127)); MAX(level, R(50, 90));
    }
    if (t & T_RING) { p->noiseRing = 1; MAX(noiseLevel, R(65, 105)); MAX(level, R(30, 70)); }
    if (t & T_GRAIN) { p->grain = R(88, 124); MAX(noiseLevel, R(70, 105)); }
    if (t & T_REG) { static const float k[4] = {0.5f, 0.75f, 1.5f, 2.0f}; p->reg *= k[ph_pick(r, 4)]; }
    if (t & T_FOLD) { p->foldAmt = R(70, 127); MAX(level, R(70, 105)); p->volume = 100; }
    if (t & T_DRIVE) { p->overdrive = R(80, 127); p->odPost = (uint8_t)ph_chance(r, 0.5f); p->volume = 100; }
    if (t & T_CRUSH) { p->bits = R(30, 64); p->volume = 124; }   /* louder, or the steps swallow quiet voices */
    if (t & T_DECIM) { p->srr = R(15, 110); p->srrPost = (uint8_t)ph_chance(r, 0.5f); }
    /* filters: the last one drawn wins */
    if (t & T_DARK) { p->filter = F_MULTI; p->cutoff = R(floor, floor + 35); p->fH = R(0, 30); p->fG = R(20, 70); }
    if (t & T_BRIGHT) { p->filter = F_MULTI; p->cutoff = R(70, 100); p->fH = R(80, 127); p->fG = R(0, 50); MAX(noiseBase, R(85, 112)); }
    if (t & T_PEAK) { p->filter = F_EQ; p->cutoff = R(40, 110); p->fG = R(100, 127); p->fH = R(60, 127); }
    if (t & T_COMB) { p->filter = ph_chance(r, 0.5f) ? F_COMB_PLUS : F_COMB_MINUS; p->cutoff = R(floor < 40 ? 30 : floor, 105); p->fG = R(70, 124); p->fH = R(50, 120); }
    if (t & T_LADDER) { p->filter = F_LP4; p->cutoff = R(floor < 45 ? 45 : floor, 100); p->fG = R(80, 127); }
    /* the bit reducer acts after the filter and the amplitude: under a dark
       filter coarse steps round the whole drum to zero, so keep >= ~12 steps there */
    if (p->bits > 0.5f) {
        const float most = p->filter == F_LP4 || p->cutoff < 64 ? 48 : 64;
        if (p->bits > most) p->bits = most;
    }
#undef MAX
#undef R
}

/* force >= 0 picks the archetype (kits spread repeats of a role over
   different ones); -1 draws it. */
PH_SMALL static void ph_character_a(PhPatch *p, unsigned pad, uint32_t seed, int force)
{
    PhRng rr, *r = &rr;
    const unsigned role = pad & 7u, gnarly = pad >= PH_GNARLY;
    ph_seed(r, (uint64_t)seed ^ (0xC4A2ull + (uint64_t)pad * 7919));
    if (!gnarly) {
        if (role == PH_ROLE_KICK) return;           /* the kick is good as poundhard draws it */
        unsigned a = (unsigned)ph_pick(r, 8);
        if (force >= 0) a = (unsigned)force & 7u;
        uint32_t t = ph_archetype[role][a];
        if (ph_chance(r, 0.3f)) t |= ph_mild[ph_pick(r, 8)];
        p->reg *= ph_exp2(ph_range(r, -7, 7) / 12.0f);   /* tuned anywhere within a fifth */
        ph_treat(p, r, t, role);
        return;
    }
    {
        unsigned a = role == PH_ROLE_KICK ? (unsigned)ph_pick(r, 8) : 1u + (unsigned)ph_pick(r, 7);
        if (force >= 0) a = role == PH_ROLE_KICK ? (unsigned)force & 7u : 1u + (unsigned)force % 7u;
        uint32_t t = ph_archetype[role][a];
        const int extra = 1 + ph_pick(r, 2);
        for (int i = 0; i < extra; ++i) t |= ph_harsh[ph_pick(r, 12)];
        if (role != PH_ROLE_KICK) p->reg *= ph_exp2(ph_range(r, -12, 12) / 12.0f);   /* within an octave */
        ph_treat(p, r, t, role);
    }
}
PH_SMALL void ph_character(PhPatch *p, unsigned pad, uint32_t seed) { ph_character_a(p, pad, seed, -1); }

/* ------------------------------------------------------- the families --- */
/* The generator's eight families for rhythmic noise, power noise and
   industrial techno: no toms, congas or cowbells. Each draw picks a pad role
   (with character, or its gnarly version) and a kind, then designs it. */
PH_SMALL unsigned ph_family(PhPatch *p, unsigned family, uint32_t seed)
{
    PhRng rr, *r = &rr;
    ph_seed(r, (uint64_t)seed ^ (0xFA111ull + (uint64_t)family * 104729));
    const float u = ph_unit(r);
    unsigned pad, kind;
    switch (family) {
    case PH_FAM_KICK:     /* the widest pool: plain, with character, gnarly */
        pad = u < 0.4f ? PH_ROLE_KICK : PH_ROLE_KICK | PH_GNARLY;
        break;
    case PH_FAM_SNARE:    /* snares, sometimes clap-built */
        pad = (ph_chance(r, 0.2f) ? PH_ROLE_CLAP : PH_ROLE_SNARE) | (u < 0.35f ? PH_GNARLY : 0);
        break;
    case PH_FAM_HAT:      /* hats and cymbals */
        pad = PH_ROLE_HAT | (u < 0.3f ? PH_GNARLY : 0);
        break;
    case PH_FAM_METAL:
        pad = PH_ROLE_METAL | (u < 0.4f ? PH_GNARLY : 0);
        break;
    case PH_FAM_NOISE:
        pad = PH_ROLE_NOISE | (u < 0.4f ? PH_GNARLY : 0);
        break;
    case PH_FAM_GLITCH:
        pad = PH_ROLE_CLICK | (u < 0.4f ? PH_GNARLY : 0);
        break;
    case PH_FAM_IMPACT: { /* industrial hits: a struck body, then driven and folded */
        static const uint8_t body[3] = {PH_ROLE_TOM, PH_ROLE_KICK, PH_ROLE_METAL};
        pad = body[ph_pick(r, 3)] | PH_GNARLY;
        break;
    }
    default:              /* gnarly: any role, always destroyed */
        pad = (unsigned)ph_pick(r, 8) | PH_GNARLY;
        break;
    }
    kind = ph_role_kind(pad & 7u, seed);
    if (family == PH_FAM_IMPACT && (pad & 7u) == PH_ROLE_METAL) kind = PH_ANVIL;
    ph_design(p, kind, seed);
    ph_character(p, pad, seed);
    if (family == PH_FAM_IMPACT) {
        /* weight and dirt on every impact */
        p->overdrive = ph_range(r, 70, 127); p->odPost = (uint8_t)ph_chance(r, 0.5f);
        if (p->foldAmt < 40) p->foldAmt = ph_range(r, 40, 110);
        if (p->decay < 40) p->decay = ph_range(r, 40, 80);
        p->volume = 100;
    } else if (family == PH_FAM_HAT && ph_chance(r, 0.3f)) {
        /* a cymbal: long and open */
        p->decay = ph_range(r, 70, 100); p->noiseDecay = ph_range(r, 65, 90);
        p->aHold = ph_range(r, 70, 100); p->aDec = ph_range(r, 95, 115);
    } else if (family == PH_FAM_GNARLY) {
        static const uint32_t extra[6] = { T_CRUSH, T_DECIM, T_CHAOS, T_FOLD | T_DRIVE, T_COMB, T_RING };
        ph_treat(p, r, extra[ph_pick(r, 6)], pad & 7u);
    }
    return pad;
}

/* ------------------------------------------------------------- the voice --- */
enum { E_IDLE, E_DELAY, E_ATTACK, E_HOLD, E_DECAY };

static void ph_env_trigger(PhEnv *e) { e->level = 0; e->stage = E_DELAY; e->t = 0; }
/* poundhard Envelope::tick for no delay; attack in seconds (0: instant); hold
   in seconds; `mul` the per-tick decay multiplier toward `sus` (adsr) or 0. */
static float ph_env_tick(PhEnv *e, float dt, float atk, float hold, float mul, float sus, int adsr)
{
    switch (e->stage) {
    case E_DELAY:
        e->t += dt;
        if (e->t >= 0) { e->stage = E_ATTACK; e->t = 0; }
        break;
    case E_ATTACK:
        e->level += atk <= 0.0005f ? 1.0f : dt / atk;
        if (e->level >= 1) { e->level = 1; e->stage = adsr ? E_DECAY : E_HOLD; e->t = 0; }
        break;
    case E_HOLD:
        e->t += dt;
        if (e->t >= hold) e->stage = E_DECAY;
        break;
    case E_DECAY: {
        float target = adsr ? sus : 0;
        e->level = target + (e->level - target) * mul;
        if (!adsr && e->level < 1e-5f) { e->level = 0; e->stage = E_IDLE; }
        break;
    }
    default: break;
    }
    return e->level;
}

/* poundhard's SVF with its coefficients worked out once per strike */
static PhSvfC ph_svfc(float g, float k)
{
    PhSvfC c;
    c.a1 = 1 / (1 + g * (g + k)); c.a2 = g * c.a1; c.a3 = g * c.a2; c.k = k;
    return c;
}
/* An envelope over a control period: poundhard's per-sample stage machine
   advanced n samples at once (in DECAY by the multiplier to the n-th power),
   returning the level before; the caller ramps linearly to e->level. Within
   16 samples (0.33 ms) the fastest decay moves less than 0.5 dB. */
static float ph_env_block(PhEnv *e, unsigned n, float atk, float hold, float mul, float mulN)
{
    const float start = e->level;
    unsigned left = n;
    while (left && e->stage != E_DECAY && e->stage != E_IDLE) { ph_env_tick(e, PH_DT, atk, hold, mul, 0, 0); --left; }
    if (left && e->stage == E_DECAY) {
        e->level *= left == PH_CTL ? mulN : ph_pow(mul, (float)left);
        if (e->level < 1e-5f) { e->level = 0; e->stage = E_IDLE; }
    }
    return start;
}

static void ph_svf(PhSvf *restrict s, const PhSvfC *restrict c, float x, float *lp, float *bp, float *hp)
{
    float v3 = x - s->ic2, v1 = c->a1 * s->ic1 + c->a2 * v3, v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2 * v1 - s->ic1;
    s->ic2 = 2 * v2 - s->ic2;
    *lp = v2; *bp = v1; *hp = x - c->k * v1 - v2;
}

PH_SMALL static void ph_cycle(int16_t *out, const int16_t (*table)[PH_WAVE_LEN], float knob)
{
    float pos = ph_clamp(knob, 0, 127) * 0.25f;
    int f0 = (int)pos;
    if (f0 > PH_WAVE_FRAMES - 2) f0 = PH_WAVE_FRAMES - 2;
    float t = pos - (float)f0;
    if (t > 1) t = 1;
    for (unsigned i = 0; i < PH_WAVE_LEN; ++i)
        out[i] = (int16_t)((float)table[f0][i] + ((float)table[f0 + 1][i] - (float)table[f0][i]) * t);
}
/* a cycle at a phase (2^32 per cycle), linear between its 128 points */
static float ph_wave(const int16_t *c, uint32_t phase)
{
    uint32_t i = phase >> 25, f = (phase >> 9) & 0xffffu;
    int32_t a = c[i], b = c[(i + 1) & 127u];
    return (float)(a + (((b - a) * (int32_t)f) >> 16)) * PH_WAVE_SCALE;
}
static uint32_t ph_inc(float hz) { return (uint32_t)(ph_clamp(hz, 0, PH_SR * 0.49f) * (4294967296.0f / PH_SR)); }
/* a phase offset in cycles, wrapped into one cycle (indices reach several cycles) */
/* a phase offset in cycles as a wrapping 2^32 phase: Q24 then shifted, so
   offsets of many cycles wrap exactly (|offset| < 128 cycles) */
static uint32_t ph_pm(float cycles) { return (uint32_t)(int32_t)(cycles * 16777216.0f) << 8; }

PH_SMALL static void ph_setup(PhVoice *v);
PH_SMALL void ph_strike(PhVoice *v, const PhPatch *p, float hz, float velocity)
{
    float last = v->active ? v->lastOut : 0;
    uint32_t noise = v->noise ? v->noise : 0x1234567u;
    for (unsigned i = 0; i < sizeof(*v) / 4u; i++) ((uint32_t *)v)[i] = 0;
    v->p = *p;
    v->noise = noise;
    v->baseHz = hz;
    v->velocity = ph_clamp(velocity, 0, 1);
    /* the previous strike's last value fades out over ~3 ms: no step, no click */
    v->declick = last;
    ph_cycle(v->carrier, ph_carrier, p->waveC);
    ph_cycle(v->modulator, ph_modulator, p->waveAB);
    ph_env_trigger(&v->body); ph_env_trigger(&v->modA); ph_env_trigger(&v->modB);
    ph_env_trigger(&v->noiseEnv); ph_env_trigger(&v->amp);
    ph_setup(v);
    v->active = 1;
}

/* Everything that does not move during a strike: poundhard recomputes these
   every 16 samples from knobs that only its sliders and LFOs (here fixed or
   absent) could change. */
PH_SMALL static void ph_setup(PhVoice *v)
{
    const PhPatch *p = &v->p;
    const float dtc = (float)PH_CTL * PH_DT;
    v->holdBody = ph_hold(p->hold); v->holdNoise = ph_hold(p->noiseHold); v->holdAmp = ph_hold(p->aHold);
    v->mulBody = ph_mul60(ph_decay60(p->decay));
    v->mulNoise = ph_mul60(ph_decay60(ph_clamp(p->noiseDecay, 0, 127)));
    v->mulAmp = ph_mul60(ph_decay60(p->aDec));
    v->mulBodyN = ph_pow(v->mulBody, (float)PH_CTL); v->mulNoiseN = ph_pow(v->mulNoise, (float)PH_CTL);
    v->mulAmpN = ph_pow(v->mulAmp, (float)PH_CTL);
    v->mulModA = ph_exp(-6.9078f * dtc / (ph_decay60(ph_clamp(p->decA, 0, 126)) * 0.82f));
    v->mulModB = ph_exp(-6.9078f * dtc / (ph_decay60(ph_clamp(p->decB, 0, 126)) * 0.82f));
    v->susA = ph_clamp(p->endA, 0, 127) / 127; v->susB = ph_clamp(p->endB, 0, 127) / 127;
    v->sweepDepth = 15.9f * (p->sdep / 127) * (p->sdep / 127);
    v->sweepTau = ph_sweep_time(p->stim);
    v->modA2 = 4 * ph_sq(p->modA); v->modB2 = ph_sq(p->modB);
    v->fbAmt = 6.2f * ph_sq(p->feedback);
    v->level = ph_sq(p->level) * 0.92f;
    {
        const float g = p->grain;
        v->grainP = g <= 64 ? 1 : 1 - ((g - 64) / 63) * ((g - 64) / 63) * 0.97f;
    }
    v->trLevel = ph_sq(p->transientLevel);
    v->volume = ph_sq(p->volume) * 1.4f;
    /* the folder: its fold amount is fixed for the strike, so the two measured
       curves around it are blended once into this voice's own curve */
    {
        const float f = ph_clamp(p->foldAmt, 0, 127);
        int k = 0;
        while (k < 17 - 2 && f > ph_fold_at[k + 1]) ++k;
        const float t = (f - ph_fold_at[k]) / (ph_fold_at[k + 1] - ph_fold_at[k]);
        for (int i = 0; i < 65; ++i) {
            const float y = ph_fold_curve[k][i] + (ph_fold_curve[k + 1][i] - ph_fold_curve[k][i]) * t;
            v->fold[i] = (int16_t)(y * 16384.0f + (y < 0 ? -0.5f : 0.5f));
        }
        v->fold[65] = v->fold[64];
    }
    /* the noise band */
    v->noiseHpOn = p->noiseBase > 0.5f;
    v->cNoiseHp = ph_svfc(ph_g(ph_base_hz(p->noiseBase)), 1.414f);
    {
        const float top = ph_top_knob(p->noiseBase, p->noiseWidth);
        v->noiseLpOn = top < 136;
        v->cNoiseLp = ph_svfc(ph_g(ph_cutoff_hz(top)), 1.414f);
    }
    /* the filter (no filter envelope: poundhard's designs leave its depth 0) */
    {
        const float fhz = ph_cutoff_hz(ph_clamp(p->cutoff, 0, 127));
        const float reso = p->fG / 127.0f;
        switch (p->filter) {
        case F_MULTI: {
            /* type: low-pass to band-pass by about 48, band-pass to high-pass by about 80 */
            const float t = p->fH;
            v->cFilt = ph_svfc(ph_g(fhz), 2.0f / (0.5f + reso * reso * 17.0f));
            if (t <= 48) { const float m = t / 48; v->mixLp = 1 - m; v->mixBp = m; }
            else if (t <= 80) { const float m = (t - 48) / 32; v->mixBp = 1 - m; v->mixHp = m; }
            else v->mixHp = 1;
            break;
        }
        case F_LP4: v->gLp4 = 1 - ph_exp(-PH_TWO_PI * ph_clamp(fhz, 10, PH_SR * 0.45f) / PH_SR); v->filtFb = reso * 3.9f; break;
        case F_LEGACY: v->cFilt = ph_svfc(ph_g(fhz), 2.0f / (0.6f + reso * reso * 12.0f)); break;
        case F_COMB_MINUS: case F_COMB_PLUS:
            v->filtFb = ph_clamp(reso, 0, 0.98f);
            v->combPeriod = ph_clamp(PH_SR / ph_clamp(fhz, 20, 8000), 2, (float)PH_COMB - 2);
            v->combLpA = 1 - ph_exp(-PH_TWO_PI * ph_clamp(ph_cutoff_hz(p->fH), 5, PH_SR * 0.45f) / PH_SR);
            break;
        default: {
            const float q = 0.3f + p->fH / 127.0f * 12.0f;
            v->cFilt = ph_svfc(ph_g(fhz), 1.0f / q);
            v->eqGain = (ph_exp2((p->fG - 64) / 64.0f * 18.0f / 20.0f * 3.32192809f) - 1) / q;
            break;
        }
        }
    }
    /* drive and crush */
    v->odDrive = 1 + (p->overdrive / 127.0f) * (p->overdrive / 127.0f) * 30;
    v->odNorm = 0.35f / ph_tanh(v->odDrive * 0.35f);
    v->bitSteps = p->bits > 0.5f ? ph_exp2(ph_exp2(4.0f * (1 - p->bits / 127.0f)) - 1) : 0;
    v->srrStep = (1 + p->srr * 0.75f) * (PH_SR / 48000.0f);   /* poundhard counts 48 kHz samples */
    /* the transient's row, by knob; its envelope as a running product */
    {
        const float pos = ph_clamp(p->transient, 0, 127) / 2.0f;
        const int i0 = (int)pos < 63 ? (int)pos : 63, i1 = i0 + 1 < 64 ? i0 + 1 : i0;
        const float f = pos - (float)(int)pos;
        float row[4];
        for (int k = 0; k < 4; ++k) row[k] = ph_transient[i0][k] + (ph_transient[i1][k] - ph_transient[i0][k]) * f;
        v->trTau = row[0] * 0.001f / 1.5f;
        v->trTonal = ph_clamp((row[3] - 12) / 16, 0, 1);
        v->incTr = ph_inc(row[1]);
        v->cTr = ph_svfc(ph_g(row[2]), 1.0f);
        v->trEnv = 1;
        v->trMul = ph_exp(-PH_DT / v->trTau);
        v->trEnd = v->trTau * 12;
    }
}

/* every 16 samples: the clap's burst, the pitch sweep, the modulators' envelopes */
static void ph_control(PhVoice *v)
{
    const PhPatch *p = &v->p;
    const float dt = (float)PH_CTL * PH_DT;
    float mod = 0;
    if (p->lfoOn) {   /* the clap's bursts: a saw on noise level, one cycle (T_ONE), then held where it stopped */
        const float stepsPerSecond = 120.0f * 4 / 60, cyclesPerStep = p->lfoSpeed * (float)(1 << (int)p->lfoMult) / 2048.0f;
        if (v->lfoPhase < 1.0f) v->lfoPhase += cyclesPerStep * stepsPerSecond * dt;
        mod = (1 - 2 * (v->lfoPhase - ph_floor(v->lfoPhase))) * p->lfoDepth / 64.0f;
    }
    {
        const float nl = ph_clamp(p->noiseLevel + mod * 127, 0, 127) / 127.0f;
        v->noiseLevel = 2.5f * nl * nl * nl;
    }
    const float hzC = v->baseHz * p->reg * (1 + v->sweepDepth * ph_exp(-v->t / v->sweepTau));
    v->incC = ph_inc(hzC);
    v->incA = ph_inc(hzC * p->ratioA / 4.0f);
    v->incB = ph_inc(hzC * p->ratioB / 4.0f);
    const float envA = ph_env_tick(&v->modA, dt, 0, 0, v->mulModA, v->susA, 1);
    const float envB = ph_env_tick(&v->modB, dt, 0, 0, v->mulModB, v->susB, 1);
    v->idxA = v->modA2 * envA;
    v->idxB = 4 * v->modB2 * envB;
    v->outB = v->modB2 * envB;
}

static float ph_overdrive(const PhVoice *v, float x) { return v->p.overdrive <= 0.5f ? x : ph_tanh(x * v->odDrive) * v->odNorm; }
static float ph_srr(const PhVoice *v, float x, float *hold, float *count)
{
    if (v->p.srr <= 0.5f) return x;
    if (*count <= 0) { *hold = x; *count += v->srrStep; }
    *count -= 1;
    return *hold;
}
static float ph_fold_at_k(const PhVoice *v, float x)
{
    const float u = ph_clamp(x, -1, 1) * 32.0f + 32.0f;
    const int i0 = (int)u;
    const float a = (float)v->fold[i0];
    return (a + ((float)v->fold[i0 + 1] - a) * (u - (float)i0)) * (1.0f / 16384.0f);
}

/* The filter over a block, the type chosen once outside the loop. */
static void ph_filter_block(PhVoice *restrict v, float *restrict x, unsigned n)
{
    const PhPatch *p = &v->p;
    switch (p->filter) {
    case F_MULTI: {
        PhSvf s = v->svfA;
        const PhSvfC c = v->cFilt;
        const float wl = v->mixLp, wb = v->mixBp, wh = v->mixHp;
        _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) {
            const float v3 = x[i] - s.ic2, v1 = c.a1 * s.ic1 + c.a2 * v3, v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
            s.ic1 = 2 * v1 - s.ic1; s.ic2 = 2 * v2 - s.ic2;
            x[i] = v2 * wl + v1 * wb + (x[i] - c.k * v1 - v2) * wh;
        }
        v->svfA = s;
        break;
    }
    case F_LP4: {
        float l0 = v->ladder[0], l1 = v->ladder[1], l2 = v->ladder[2], l3 = v->ladder[3];
        const float g = v->gLp4, fbk = v->filtFb, gain = 1 + fbk * 0.25f;
        _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) {
            const float in = ph_tanh(x[i] - fbk * l3);
            l0 += g * (in - l0); l1 += g * (l0 - l1); l2 += g * (l1 - l2); l3 += g * (l2 - l3);
            x[i] = l3 * gain;
        }
        v->ladder[0] = l0; v->ladder[1] = l1; v->ladder[2] = l2; v->ladder[3] = l3;
        break;
    }
    case F_LEGACY: {
        PhSvf s = v->svfA;
        const PhSvfC c = v->cFilt;
        const int high = p->fH >= 64;
        _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) {
            const float in = ph_tanh(x[i] * 1.2f);
            const float v3 = in - s.ic2, v1 = c.a1 * s.ic1 + c.a2 * v3, v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
            s.ic1 = 2 * v1 - s.ic1; s.ic2 = 2 * v2 - s.ic2;
            x[i] = high ? in - c.k * v1 - v2 : v2;
        }
        v->svfA = s;
        break;
    }
    case F_COMB_MINUS: case F_COMB_PLUS: {
        const float sign = p->filter == F_COMB_PLUS ? 1.0f : -1.0f, fbk = v->filtFb, la = v->combLpA, out = 1 - fbk * 0.5f;
        float z = v->combLpZ;
        unsigned at = v->combAt;
        _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) {
            /* the comb's delay line, read `combPeriod` behind the write position */
            float pos = (float)at - v->combPeriod;
            if (pos < 0) pos += (float)PH_COMB;
            const unsigned i0 = (unsigned)pos, i1 = (i0 + 1u) & (PH_COMB - 1u);
            const float t = pos - (float)i0;
            const float d = ((float)v->comb[i0] + ((float)v->comb[i1] - (float)v->comb[i0]) * t) * (1.0f / 16384.0f);
            z += la * (d - z);
            const float y = x[i] + sign * fbk * z;
            v->comb[at] = (int16_t)(ph_tanh(y) * 16384.0f);
            at = (at + 1u) & (PH_COMB - 1u);
            x[i] = y * out;
        }
        v->combLpZ = z; v->combAt = (uint16_t)at;
        break;
    }
    default: {
        PhSvf s = v->svfA;
        const PhSvfC c = v->cFilt;
        const float gain = v->eqGain;
        _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) {
            const float v3 = x[i] - s.ic2, v1 = c.a1 * s.ic1 + c.a2 * v3, v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
            s.ic1 = 2 * v1 - s.ic1; s.ic2 = 2 * v2 - s.ic2;
            x[i] += gain * v1;
        }
        v->svfA = s;
        break;
    }
    }
}

/* One control period (16 samples), stage by stage over the period: each loop
   keeps its few values in registers (the per-sample version spilled most of
   the voice to memory every sample). Same arithmetic per sample as poundhard. */
static void ph_period(PhVoice *restrict v, float *restrict out, unsigned n)
{
    const PhPatch *p = &v->p;
    float x[PH_CTL], cs[PH_CTL];
    /* 1a. the operators: the carrier into cs[], the second carrier into x[] */
    {
        const int16_t *mc = v->carrier, *mm = v->modulator;
        const float tw = 1.0f / PH_TWO_PI, idxA = v->idxA * tw, idxB = v->idxB * tw, outB = v->outB;
        const float fbt = v->fbAmt * tw * 0.5f;
        uint32_t phA = v->phA, phB = v->phB, phC = v->phC;
        const uint32_t incA = v->incA, incB = v->incB, incC = v->incC;
        float fbA = v->fbA, fbA1 = v->fbA1, fbB = v->fbB, fbB1 = v->fbB1;
        switch (p->algo) {
#define LOOP(BODY) _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) { float a, b, c, cb = 0; BODY; \
                fbA1 = fbA; fbA = a; fbB1 = fbB; fbB = b; phC += incC; phA += incA; phB += incB; cs[i] = c; x[i] = cb; }
        case 0: LOOP(b = ph_wave(mm, phB + ph_pm(fbt * (fbB + fbB1))); a = ph_wave(mm, phA);
                     c = ph_wave(mc, phC + ph_pm(idxA * a + idxB * b))) break;
        case 1: LOOP(b = ph_wave(mm, phB + ph_pm(fbt * (fbB + fbB1))); a = ph_wave(mm, phA + ph_pm(fbt * (fbA + fbA1)));
                     c = ph_wave(mc, phC + ph_pm(idxA * a + idxB * b))) break;
        case 2: LOOP(b = ph_wave(mm, phB + ph_pm(fbt * (fbB + fbB1))); a = ph_wave(mm, phA + ph_pm(idxB * b));
                     c = ph_wave(mc, phC + ph_pm(idxA * a + idxB * b))) break;
        case 3: LOOP(b = ph_wave(mm, phB + ph_pm(fbt * (fbB + fbB1))); a = ph_wave(mm, phA + ph_pm(idxB * b));
                     c = ph_wave(mc, phC + ph_pm(idxA * a))) break;
        case 4: LOOP(b = ph_wave(mm, phB + ph_pm(fbt * (fbB + fbB1))); a = ph_wave(mm, phA);
                     c = ph_wave(mc, phC + ph_pm(idxA * a)); cb = b * outB) break;
        case 5: LOOP(b = ph_wave(mm, phB); a = ph_wave(mm, phA + ph_pm(fbt * (fbA + fbA1)));
                     c = ph_wave(mc, phC + ph_pm(idxA * a)); cb = b * outB) break;
        default: LOOP(b = ph_wave(mm, phB + ph_pm(fbt * (fbB + fbB1))); a = ph_wave(mm, phA + ph_pm(fbt * (fbA + fbA1)));
                     c = ph_wave(mc, phC + ph_pm(idxA * a)); cb = b * outB) break;
#undef LOOP
        }
        v->phA = phA; v->phB = phB; v->phC = phC;
        v->fbA = fbA; v->fbA1 = fbA1; v->fbB = fbB; v->fbB1 = fbB1;
    }
    /* 1b. the body envelope, the folder and level */
    {
        const float e0 = ph_env_block(&v->body, n, 0, v->holdBody, v->mulBody, v->mulBodyN);
        const float de = (v->body.level - e0) / (float)n, level = v->level * (1.0f / 16384.0f);
        const int16_t *fold = v->fold;
        float env = e0;
        _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) {
            env += de;
            float u = (cs[i] * env + x[i]) * 32.0f + 32.0f;
            u = u < 0 ? 0 : (u > 64 ? 64 : u);
            const int i0 = (int)u;
            const float f0 = (float)fold[i0];
            x[i] = (f0 + ((float)fold[i0 + 1] - f0) * (u - (float)i0)) * level;
        }
    }
    /* 2. noise and transient, through the band and the noise envelope */
    {
        const float t0 = v->t;
        const int noise = v->noiseLevel > 0, transient = v->trLevel > 0 && t0 <= v->trEnd;
        const int ringing = v->noiseHp.ic1 != 0 || v->noiseLp.ic1 != 0 || v->noiseHp.ic2 != 0 || v->noiseLp.ic2 != 0;
        const float e0 = ph_env_block(&v->noiseEnv, n, 0, v->holdNoise, v->mulNoise, v->mulNoiseN);
        const float de = (v->noiseEnv.level - e0) / (float)n;
        if (noise || transient || ringing) {
            const float nl = v->noiseLevel, grainP = v->grainP, trLevel = v->trLevel;
            float last = 0, env = e0;
            _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) {
                float band = 0;
                if (noise) {
                    /* one draw: its low bits gate the grain, the value is the sample */
                    const float r = ph_noise(&v->noise);
                    float nz = (float)(v->noise & 0xffffu) * (1.0f / 65536.0f) < grainP ? r * nl : 0;
                    if (p->noiseRing) nz *= cs[i] * 2;
                    band = nz;
                }
                if (transient && t0 + (float)i * PH_DT <= v->trEnd) {
                    const float env = v->trEnv;
                    v->trEnv *= v->trMul;
                    v->trPhase += v->incTr;
                    float lp, bp, hp;
                    ph_svf(&v->trSvf, &v->cTr, ph_noise(&v->noise), &lp, &bp, &hp);
                    band += (ph_sin(v->trPhase) * v->trTonal + bp * 2.2f * (1 - v->trTonal)) * env * trLevel;
                }
                float lp, bp, hp;
                if (v->noiseHpOn) { ph_svf(&v->noiseHp, &v->cNoiseHp, band, &lp, &bp, &hp); band = hp; }
                if (v->noiseLpOn) { ph_svf(&v->noiseLp, &v->cNoiseLp, band, &lp, &bp, &hp); band = lp; }
                env += de;
                x[i] += band * env;
                last = band;
            }
            if (!noise && !transient && last * last < 1e-14f)
                v->noiseHp.ic1 = v->noiseHp.ic2 = v->noiseLp.ic1 = v->noiseLp.ic2 = 0;
        }
        v->t = t0 + (float)n * PH_DT;
    }
    /* 3. drive and crush before the filter, the filter, the amplitude, after it */
    if (!p->srrPost && p->srr > 0.5f) for (unsigned i = 0; i < n; ++i) x[i] = ph_srr(v, x[i], &v->srrHold, &v->srrCount);
    if (!p->odPost && p->overdrive > 0.5f) for (unsigned i = 0; i < n; ++i) x[i] = ph_tanh(x[i] * v->odDrive) * v->odNorm;
    ph_filter_block(v, x, n);
    {
        const float g = v->volume * v->velocity;
        const float e0 = ph_env_block(&v->amp, n, 0.042f / 6.9f, v->holdAmp, v->mulAmp, v->mulAmpN) * g;
        const float de = (v->amp.level * g - e0) / (float)n;
        float env = e0;
        _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) { env += de; x[i] *= env; }
    }
    if (p->odPost && p->overdrive > 0.5f) for (unsigned i = 0; i < n; ++i) x[i] = ph_tanh(x[i] * v->odDrive) * v->odNorm;
    if (p->srrPost && p->srr > 0.5f) for (unsigned i = 0; i < n; ++i) x[i] = ph_srr(v, x[i], &v->srrHoldPost, &v->srrCountPost);
    if (v->bitSteps > 0) {
        const float steps = v->bitSteps, inv = 1.0f / steps;
        _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) { const float s = x[i] * steps; x[i] = (float)(int)(s + (s < 0 ? -0.5f : 0.5f)) * inv; }
    }
    /* a soft ceiling; the previous strike's last value fades out over ~3 ms */
    float dc = v->declick, y = 0;
    _Pragma("clang loop unroll(disable)") for (unsigned i = 0; i < n; ++i) {
        const float ax = ph_abs(x[i]);
        y = (ax < 0.8f ? x[i] : (x[i] > 0 ? 1 : -1) * (0.8f + 0.2f * ph_tanh((ax - 0.8f) * 5))) + dc;
        dc *= 0.993f;
        out[i] += y;
    }
    v->declick = dc; v->lastOut = y;
}

void ph_render(PhVoice *restrict v, float *restrict out, unsigned n)
{
    if (!v->active) return;
    for (unsigned at = 0; at < n; at += PH_CTL) {
        ph_control(v);
        ph_period(v, out + at, n - at < PH_CTL ? n - at : PH_CTL);
    }
    /* done when the amplitude envelope has ended and the declick has faded */
    if (v->amp.stage == E_IDLE && ph_abs(v->declick) < 1e-6f) v->active = 0;
}
