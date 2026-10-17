/* SPDX-License-Identifier: GPL-3.0-only
 * Circuit Rhythm FM: the Tonitor patch generator, adapted.
 *
 * Source: TONITOR firmware for the M-VAVE FM-1 (GPL-3.0-only), local checkout
 * /Users/nzimas/development/mvave-fm1 at commit 209a1d3ed9a827dd32568312c455184c309c87a0:
 *   firmware/src/fm.c  -- family tables, fm6_max/fm6_sanitize (after SLOOP's eng_fm6.c, Leo Kuroshita,
 *                         GPL-3.0), and everything from fm_op through fm_generate, verbatim except that
 *                         fm_generate writes a bare 156-byte voice and returns the family it drew;
 *   firmware/src/dsp.h -- clampi/maxi and the xorshift rnd_* helpers, verbatim.
 * Not ported: fm_measure (its 3445-block offline loudness pass would starve Rhythm's 5.33 ms audio FIFO),
 * fm_render's macros/saturation, and the FM-1 layers/session code. Rhythm keeps its own pinned FM6 core
 * (vendor/fm6_core.c); the generator only needs its FP_* layout and fm6_carriers.
 */
/* Generation runs at most once per button press, outside the per-sample path:
   optimize it for flash size (the extension window ends at 0x08074000). */
#pragma clang attribute push (__attribute__((minsize, noinline)), apply_to = function)
#define PATH_OPEN 0u
#define PATH_AMBIENT 1u
#define PATH_EXP 2u
#define PATH_N 3u
#define AINL static inline
AINL int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
AINL int32_t maxi(int32_t a, int32_t b) { return a > b ? a : b; }

/* a small fast random generator (xorshift32): per sample noise, the generators' draws */
typedef struct { uint32_t s; } rnd_t;
AINL uint32_t rnd_next(rnd_t *r)
{
    uint32_t x = r->s ? r->s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}
static inline void rnd_seed(rnd_t *r, uint32_t seed)
{
    uint32_t i;
    r->s = seed * 2654435761u + 0x6A09E667u;
    if (!r->s)
        r->s = 1;
    for (i = 0; i < 4u; i++)
        rnd_next(r);
}
AINL int32_t rnd_q15(rnd_t *r) { return (int32_t)(rnd_next(r) >> 17); }               /* 0 .. 32767 */
AINL int32_t rnd_bi(rnd_t *r) { return (int32_t)(rnd_next(r) >> 16) - 32768; }        /* -32768 .. 32767 */
static inline int32_t rnd_below(rnd_t *r, int32_t n) { return n <= 1 ? 0 : (int32_t)(((uint64_t)rnd_next(r) * (uint32_t)n) >> 32); }
static inline int rnd_chance(rnd_t *r, int32_t p_pct) { return rnd_below(r, 1000) < p_pct * 10; }   /* p in percent */
/* one of n, each as likely as its weight */
static inline uint32_t rnd_weighted(rnd_t *r, const uint8_t *w, uint32_t n)
{
    uint32_t i, t = 0, x;
    for (i = 0; i < n; i++)
        t += w[i];
    x = (uint32_t)rnd_below(r, (int32_t)t);
    for (i = 0; i + 1u < n && x >= w[i]; i++)
        x -= w[i];
    return i;
}
static inline int rnd_chance_q15(rnd_t *r, int32_t p) { return rnd_q15(r) < p; }
/* uniform in [a, b] (any integer unit) */
static inline int32_t rnd_in(rnd_t *r, int32_t a, int32_t b)
{
    return a + (int32_t)(((int64_t)(b - a) * rnd_q15(r)) >> 15);
}

enum { FF_METAL, FF_DRONE, FF_BRUIT, FF_IMPACT, FF_ITERATIF, FF_GLISSE, FF_SPECTRE, FF_ABYSS, FF_FRICTION,
       FF_N0,                    /* (generation 0 draws these nine only) */
       FF_CHOIR = FF_N0, FF_BOWED, FF_HOLLOW, FF_BLOOM, FF_TIDE, FF_SHARDS, FF_STATIC, FF_WIND,
       FF_N1,                    /* (generations 1 and 2 draw these seventeen) */
       FF_PLUCK = FF_N1, FF_MALLET, FF_WARP, FF_GLITCH, FF_RUBBER, FF_N };
/* the families' weights on each path: generation 0 (the open path: rnd_below, all alike), generation 1 */
static const uint8_t FF_W[3][FF_N0] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 1},
    {3, 4, 0, 0, 1, 1, 4, 3, 0},
    {2, 1, 4, 3, 3, 2, 0, 1, 4},
};
static const uint8_t FF_W1[3][FF_N1] = {
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    {3, 4, 0, 0, 1, 1, 4, 3, 0, 4, 4, 4, 3, 4, 0, 0, 2},
    {2, 1, 4, 3, 3, 2, 0, 1, 4, 0, 0, 1, 2, 0, 4, 4, 3},
};
static const uint8_t FF_W3[3][FF_N] = {             /* generation 3: plucks, mallets; warps, glitches, rubber */
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 3, 3, 3, 3},
    {3, 4, 0, 0, 1, 1, 4, 3, 0, 4, 4, 4, 3, 4, 0, 0, 2, 3, 4, 0, 0, 0},
    {2, 1, 4, 3, 3, 2, 0, 1, 4, 0, 0, 1, 2, 0, 4, 4, 3, 2, 2, 5, 5, 4},
};
static const char *const FF_NAME[FF_N] = {"METAL", "DRONE", "BRUIT", "IMPACT", "ITERATIF", "GLISSE", "SPECTRE",
                                          "ABYSS", "FRICTION", "CHOIR", "BOWED", "HOLLOW", "BLOOM", "TIDE",
                                          "SHARDS", "STATIC", "WIND", "PLUCK", "MALLET", "WARP", "GLITCH", "RUBBER"};

/* every byte of the 155-byte voice inside its range (from SLOOP's eng_fm6.c, Leo Kuroshita, GPL-3.0) */
static uint32_t fm6_max(uint32_t i)
{
    static const uint8_t OPMAX[21] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 3, 3, 7, 3, 7, 99, 1, 31, 99, 14};
    static const uint8_t VMAX[19] = {99, 99, 99, 99, 99, 99, 99, 99, 31, 7, 1, 99, 99, 99, 99, 1, 5, 7, 48};
    if (i < 126u)
        return OPMAX[i % 21u];
    if (i < FP_NAME)
        return VMAX[i - 126u];
    return 126u;
}
static void fm6_sanitize(uint8_t *v)
{
    uint32_t i;
    for (i = 0; i < FP_SIZE; i++) {
        if (i >= FP_NAME)
            v[i] = v[i] < 32u || v[i] > 126u ? ' ' : v[i];
        else if (v[i] > fm6_max(i))
            v[i] = (uint8_t)fm6_max(i);
    }
    v[FP_SIZE] = 0;
}

/* ---------------------------------------------------------- the patch --- */
/* operator n (1..6, as the DX7 numbers them) of patch p */
static uint8_t *fm_op(uint8_t *p, int n) { return p + (6 - n) * FP_OP; }

static void fm_env(uint8_t *op, int r1, int r2, int r3, int r4, int l1, int l2, int l3, int l4)
{
    op[FP_R1] = (uint8_t)clampi(r1, 0, 99);
    op[FP_R1 + 1] = (uint8_t)clampi(r2, 0, 99);
    op[FP_R1 + 2] = (uint8_t)clampi(r3, 0, 99);
    op[FP_R1 + 3] = (uint8_t)clampi(r4, 0, 99);
    op[FP_L1] = (uint8_t)clampi(l1, 0, 99);
    op[FP_L1 + 1] = (uint8_t)clampi(l2, 0, 99);
    op[FP_L1 + 2] = (uint8_t)clampi(l3, 0, 99);
    op[FP_L1 + 3] = (uint8_t)clampi(l4, 0, 99);
}
/* a ratio as the DX7 has it: coarse (0 = 0.5, 1..31) x (1 + fine / 100); x in hundredths. Generations 0 and 1
 * worked the fine out 100 times too large, so nearly every ratio became (its coarse + 1) x 1.99 -- 1.00 and 1.41
 * both 3.98: the patches two octaves up and most of their ratios lost. Kept so for them (their sounds are drawn
 * again exactly); generation 2 has the ratios as they were meant */
/* Rhythm: fm_gen_cur lives in the ITCM state (extension.c defines it); no .bss here. */
static void fm_ratio(uint8_t *op, int32_t x100)
{
    int32_t c, f, k = fm_gen_cur >= 2u ? 1 : 100;
    op[FP_MODE] = 0;
    if (x100 < 100) {
        op[FP_FC] = 0;
        op[FP_FF] = (uint8_t)clampi((x100 - 50) * 2, 0, 99);
        return;
    }
    c = clampi(x100 / 100, 1, 31);
    f = (x100 * k / c) - 100;                       /* the fine that makes c x (1 + f / 100) = x */
    if (f > 99) {
        c = clampi(c + 1, 1, 31);
        f = (x100 * k / c) - 100;
    }
    op[FP_FC] = (uint8_t)c;
    op[FP_FF] = (uint8_t)clampi(f, 0, 99);
}
/* a fixed frequency: 10^decade x 10^(fine / 100) Hz (decade 0..3: 1, 10, 100, 1000 Hz) */
static void fm_fixed(uint8_t *op, int decade, int fine)
{
    op[FP_MODE] = 1;
    op[FP_FC] = (uint8_t)clampi(decade, 0, 3);
    op[FP_FF] = (uint8_t)clampi(fine, 0, 99);
}

static uint32_t fm_carriers_of(const uint8_t *p) { return fm6_carriers(p[FP_ALG] & 31u); }
static int fm_is_carrier(const uint8_t *p, int n) { return (fm_carriers_of(p) >> (6 - n)) & 1u; }
static int fm_ncarriers(const uint8_t *p)
{
    int n, c = 0;
    for (n = 1; n <= 6; n++)
        c += fm_is_carrier(p, n);
    return c;
}

static void fm_name(uint8_t *p, const char *s)
{
    int i;
    for (i = 0; i < 10; i++)
        p[FP_NAME + i] = *s ? (uint8_t)*s++ : ' ';
}

/* the voice's defaults: no pitch envelope, a quiet LFO, no transpose */
static void fm_voice_defaults(uint8_t *p)
{
    int i;
    memset(p, 0, FP_SIZE + 1u);
    for (i = 0; i < 4; i++) {
        p[FP_PR1 + i] = 99;
        p[FP_PL1 + i] = 50;
    }
    p[FP_OKS] = 1;
    p[FP_LFS] = 35;
    p[FP_LFW] = 4;
    p[FP_LPMS] = 3;
    p[FP_TRNSP] = 24;
    for (i = 1; i <= 6; i++) {
        uint8_t *op = fm_op(p, i);
        fm_env(op, 99, 99, 99, 99, 99, 99, 99, 0);
        op[FP_BP] = 39;
        op[FP_DET] = 7;
        op[FP_FC] = 1;
        op[FP_OL] = 0;
    }
}

/* algorithm sets (0-based: the DX7's number - 1) */
static const uint8_t ALG_MANY[6] = {31, 30, 24, 23, 21, 28};     /* mostly carriers: additive, bells */
static const uint8_t ALG_PAIRS[6] = {4, 5, 21, 20, 18, 0};       /* pairs and stacks */
static const uint8_t ALG_DEEP[6] = {0, 1, 2, 3, 6, 15};          /* long modulator chains */

static int32_t inharm_ratio(rnd_t *r)              /* hundredths: the ratios of bells, plates and gongs */
{
    static const int16_t R[14] = {141, 173, 200, 224, 276, 300, 317, 350, 411, 541, 593, 713, 864, 1100};
    int32_t x = R[rnd_below(r, 14)];
    return x + rnd_in(r, -8, 8);
}

static void fm_gen_metal(uint8_t *p, rnd_t *r)
{
    int n, k = 0;
    p[FP_ALG] = ALG_MANY[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_below(r, 4);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            int32_t ratio = k == 0 ? 100 : inharm_ratio(r) * (rnd_chance(r, 25) ? 2 : 1);
            int32_t dec = 30 + rnd_below(r, 25) - (ratio > 400 ? 12 : 0) - k * 3;   /* high partials die first */
            fm_ratio(op, ratio);
            fm_env(op, 99, dec, dec - rnd_below(r, 15), 20 + rnd_below(r, 30), 99, 70 + rnd_below(r, 25), 0, 0);
            op[FP_OL] = (uint8_t)(k == 0 ? 99 : 80 + rnd_below(r, 18));
            op[FP_RS] = (uint8_t)(2 + rnd_below(r, 5));
            op[FP_DET] = (uint8_t)(4 + rnd_below(r, 7));
            k++;
        } else {
            fm_ratio(op, inharm_ratio(r));
            fm_env(op, 99, 40 + rnd_below(r, 30), 30 + rnd_below(r, 30), 40, 99, 60, 0, 0);
            op[FP_OL] = (uint8_t)(55 + rnd_below(r, 30));
        }
    }
    p[FP_LFS] = (uint8_t)rnd_in(r, 20, 45);
    p[FP_LAMD] = (uint8_t)rnd_below(r, 15);
    p[FP_LPMD] = (uint8_t)rnd_below(r, 6);
}

static void fm_gen_drone(uint8_t *p, rnd_t *r)
{
    static const int16_t NEAR[8] = {50, 100, 100, 200, 300, 100, 400, 150};
    int n;
    p[FP_ALG] = ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)(2 + rnd_below(r, 5));
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        int32_t ratio = NEAR[rnd_below(r, 8)] + rnd_below(r, 4);    /* near-harmonic, a little off */
        fm_ratio(op, ratio);
        op[FP_DET] = (uint8_t)rnd_below(r, 15);                    /* detuned: the pairs beat */
        if (fm_is_carrier(p, n)) {
            fm_env(op, 10 + rnd_below(r, 25), 20 + rnd_below(r, 20), 15, 12 + rnd_below(r, 20), 95, 99,
                   85 + rnd_below(r, 14), 0);
            op[FP_OL] = (uint8_t)(88 + rnd_below(r, 11));
            op[FP_AMS] = (uint8_t)rnd_below(r, 2);
        } else {
            /* the index wanders: up slowly, somewhere else even more slowly, held there */
            fm_env(op, 5 + rnd_below(r, 25), 3 + rnd_below(r, 15), 2 + rnd_below(r, 12), 20, 50 + rnd_below(r, 50),
                   rnd_below(r, 99), 40 + rnd_below(r, 59), 0);
            op[FP_OL] = (uint8_t)(60 + rnd_below(r, 32));
        }
    }
    p[FP_LFS] = (uint8_t)rnd_in(r, 2, 18);
    p[FP_LFW] = (uint8_t)(rnd_chance(r, 50) ? 0 : 4);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 2, 12);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 0, 25);
    p[FP_LPMS] = (uint8_t)rnd_in(r, 2, 4);
}

static void fm_gen_bruit(uint8_t *p, rnd_t *r)
{
    int n;
    static const uint8_t FBALG[6] = {31, 5, 3, 16, 0, 7};    /* algorithms whose feedback operator matters */
    p[FP_ALG] = FBALG[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)(6 + rnd_below(r, 2));
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (rnd_chance(r, 45))
            fm_fixed(op, 2 + rnd_below(r, 2), rnd_below(r, 100));
        else
            fm_ratio(op, rnd_in(r, 300, 2500));
        if (fm_is_carrier(p, n)) {
            int sust = rnd_chance(r, 60);
            fm_env(op, rnd_in(r, 40, 99), rnd_in(r, 20, 60), rnd_in(r, 15, 50), rnd_in(r, 25, 60), 99,
                   rnd_in(r, 60, 95), sust ? rnd_in(r, 60, 90) : 0, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 85, 99);
        } else {
            fm_env(op, rnd_in(r, 30, 99), rnd_in(r, 10, 50), rnd_in(r, 10, 50), 40, 99, rnd_in(r, 70, 99),
                   rnd_in(r, 70, 99), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 82, 99);
        }
    }
    p[FP_LFW] = 5;                                  /* sample and hold */
    p[FP_LFS] = (uint8_t)rnd_in(r, 45, 92);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 15, 70);
    p[FP_LPMS] = (uint8_t)rnd_in(r, 4, 7);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 0, 40);
}

static void fm_gen_impact(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = rnd_chance(r, 50) ? ALG_DEEP[rnd_below(r, 6)] : ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_below(r, 7);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            fm_ratio(op, rnd_chance(r, 70) ? 100 : inharm_ratio(r));
            fm_env(op, 99, rnd_in(r, 35, 70), rnd_in(r, 20, 50), rnd_in(r, 30, 60), 99, rnd_in(r, 30, 70), 0, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 90, 99);
        } else {
            if (rnd_chance(r, 40))
                fm_fixed(op, rnd_in(r, 1, 3), rnd_below(r, 100));   /* a thud, a knock, a ping */
            else
                fm_ratio(op, rnd_chance(r, 50) ? inharm_ratio(r) : rnd_in(r, 50, 900));
            fm_env(op, 99, rnd_in(r, 55, 90), rnd_in(r, 40, 70), 50, 99, rnd_in(r, 10, 50), 0, 0);   /* dies first */
            op[FP_OL] = (uint8_t)rnd_in(r, 70, 99);
        }
        op[FP_RS] = (uint8_t)rnd_below(r, 5);
    }
    /* the pitch falls from up there to the note */
    p[FP_PL1 + 3] = (uint8_t)rnd_in(r, 62, 99);
    p[FP_PL1] = p[FP_PL1 + 1] = p[FP_PL1 + 2] = 50;
    p[FP_PR1] = (uint8_t)rnd_in(r, 55, 92);
    p[FP_PR1 + 1] = p[FP_PR1 + 2] = 99;
    p[FP_LAMD] = 0;
    p[FP_LPMD] = 0;
}

static void fm_gen_iteratif(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = rnd_chance(r, 50) ? ALG_MANY[rnd_below(r, 6)] : ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_below(r, 6);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        fm_ratio(op, rnd_chance(r, 50) ? inharm_ratio(r) : 100 * (1 + rnd_below(r, 5)));
        if (fm_is_carrier(p, n)) {
            fm_env(op, rnd_in(r, 50, 99), rnd_in(r, 15, 40), rnd_in(r, 10, 30), rnd_in(r, 25, 50), 99, 90,
                   rnd_in(r, 60, 95), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 85, 99);
            op[FP_AMS] = 3;                              /* the LFO pulses it */
        } else {
            fm_env(op, rnd_in(r, 40, 99), rnd_in(r, 10, 40), rnd_in(r, 10, 40), 40, 99, rnd_in(r, 50, 99),
                   rnd_in(r, 40, 90), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 50, 88);
            op[FP_AMS] = (uint8_t)rnd_below(r, 4);
        }
    }
    {
        static const uint8_t W[4] = {3, 5, 1, 3};       /* square, sample and hold, saw down */
        p[FP_LFW] = W[rnd_below(r, 4)];
    }
    p[FP_LFS] = (uint8_t)rnd_in(r, 20, 72);              /* 1.5 .. 13 Hz */
    p[FP_LAMD] = (uint8_t)rnd_in(r, 55, 99);
    p[FP_LPMD] = (uint8_t)(p[FP_LFW] == 5 ? rnd_in(r, 0, 25) : 0);
    p[FP_LPMS] = (uint8_t)rnd_in(r, 2, 5);
    p[FP_LKS] = 1;
}

static void fm_gen_glisse(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_below(r, 6);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        fm_ratio(op, 100 * (1 + rnd_below(r, 4)) + rnd_below(r, 3));
        if (fm_is_carrier(p, n)) {
            fm_env(op, rnd_in(r, 20, 60), 30, 20, rnd_in(r, 15, 35), 99, 95, 90, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 88, 99);
        } else {
            fm_env(op, rnd_in(r, 15, 60), rnd_in(r, 10, 30), rnd_in(r, 10, 30), 30, 99, rnd_in(r, 50, 99),
                   rnd_in(r, 40, 85), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 55, 85);
        }
    }
    /* the pitch envelope travels: from far below or above, past the note, somewhere else, slowly */
    p[FP_PL1 + 3] = (uint8_t)(rnd_chance(r, 50) ? rnd_in(r, 0, 30) : rnd_in(r, 70, 99));
    p[FP_PL1] = (uint8_t)rnd_in(r, 20, 80);
    p[FP_PL1 + 1] = (uint8_t)rnd_in(r, 0, 99);
    p[FP_PL1 + 2] = (uint8_t)(rnd_chance(r, 50) ? 50 : rnd_in(r, 30, 70));
    p[FP_PR1] = (uint8_t)rnd_in(r, 15, 55);
    p[FP_PR1 + 1] = (uint8_t)rnd_in(r, 8, 40);
    p[FP_PR1 + 2] = (uint8_t)rnd_in(r, 8, 40);
    p[FP_PR1 + 3] = (uint8_t)rnd_in(r, 10, 50);
    /* and a siren of an LFO on top, now and then */
    if (rnd_chance(r, 50)) {
        p[FP_LFW] = (uint8_t)(rnd_chance(r, 50) ? 0 : 4);
        p[FP_LFS] = (uint8_t)rnd_in(r, 3, 25);
        p[FP_LPMD] = (uint8_t)rnd_in(r, 20, 70);
        p[FP_LPMS] = (uint8_t)rnd_in(r, 5, 7);
        p[FP_LFD] = (uint8_t)rnd_in(r, 30, 80);
    }
}

static void fm_gen_spectre(uint8_t *p, rnd_t *r)
{
    int n, k = 0;
    static const uint8_t DET[6] = {7, 0, 14, 3, 11, 7};
    p[FP_ALG] = rnd_chance(r, 70) ? 4 : 5;               /* three pairs (5), or two and a feedback trio (6) */
    p[FP_FB] = (uint8_t)rnd_in(r, 3, 7);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            fm_ratio(op, 100);
            op[FP_DET] = DET[k % 6];                     /* a few cents apart: the beating arpeggio */
            fm_env(op, rnd_in(r, 15, 50) - k * 4, 30, 20, rnd_in(r, 15, 30), 99, 99, 96, 0);
            op[FP_OL] = 95;
            k++;
        } else {
            fm_ratio(op, rnd_chance(r, 75) ? 100 : 200);
            op[FP_DET] = DET[(k + 2) % 6];
            fm_env(op, rnd_in(r, 10, 40), rnd_in(r, 5, 20), 10, 30, rnd_in(r, 80, 99), rnd_in(r, 70, 99),
                   rnd_in(r, 80, 99), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 72, 90);
        }
    }
    p[FP_LFS] = (uint8_t)rnd_in(r, 2, 10);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 0, 5);
    p[FP_LAMD] = 0;
}

static void fm_gen_abyss(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = rnd_chance(r, 50) ? 0 : ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_in(r, 5, 7);
    p[FP_TRNSP] = (uint8_t)(rnd_chance(r, 60) ? 12 : 0);    /* an octave or two down */
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        fm_ratio(op, rnd_chance(r, 50) ? 50 : 100 + rnd_below(r, 3));
        if (fm_is_carrier(p, n)) {
            fm_env(op, rnd_in(r, 15, 50), 25, 15, rnd_in(r, 10, 30), 99, 97, 92, 0);
            op[FP_OL] = 99;
            op[FP_AMS] = (uint8_t)rnd_in(r, 1, 3);
        } else {
            fm_env(op, rnd_in(r, 10, 40), rnd_in(r, 5, 20), 10, 30, rnd_in(r, 60, 99), rnd_in(r, 30, 90),
                   rnd_in(r, 50, 85), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 55, 80);
        }
        op[FP_DET] = (uint8_t)rnd_below(r, 15);
    }
    p[FP_LFW] = (uint8_t)(rnd_chance(r, 50) ? 0 : 4);
    p[FP_LFS] = (uint8_t)rnd_in(r, 2, 15);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 25, 60);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 0, 8);
}

static void fm_gen_friction(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = ALG_DEEP[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_in(r, 4, 7);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            fm_ratio(op, rnd_chance(r, 60) ? 100 : rnd_in(r, 100, 400));
            fm_env(op, rnd_in(r, 30, 70), rnd_in(r, 20, 50), rnd_in(r, 15, 40), rnd_in(r, 30, 60), 99,
                   rnd_in(r, 50, 95), rnd_in(r, 40, 85), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 88, 99);
        } else {
            if (rnd_chance(r, 60))
                fm_fixed(op, rnd_below(r, 2), rnd_in(r, 30, 99));   /* 2 .. 98 Hz: a creak, a rattle */
            else
                fm_ratio(op, rnd_in(r, 50, 700));
            /* irregular: up, down, up again */
            fm_env(op, rnd_in(r, 30, 90), rnd_in(r, 20, 70), rnd_in(r, 10, 50), 40, 99, rnd_in(r, 10, 60),
                   rnd_in(r, 50, 99), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 75, 99);
        }
    }
    p[FP_LFW] = 5;
    p[FP_LFS] = (uint8_t)rnd_in(r, 12, 48);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 25, 99);
    p[FP_LPMS] = (uint8_t)rnd_in(r, 5, 7);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 0, 30);
}

/* one more turn of chance, on any family */
static void fm_mutate(uint8_t *p, rnd_t *r)
{
    int n = 1 + rnd_below(r, 6);
    uint8_t *op = fm_op(p, n);
    switch (rnd_below(r, 6)) {
    case 0:                                             /* a partial nailed to a frequency */
        fm_fixed(op, rnd_in(r, 1, 3), rnd_below(r, 100));
        break;
    case 1: {                                           /* an envelope turned over: it grows as it holds */
        uint8_t t = op[FP_L1];
        op[FP_L1] = (uint8_t)rnd_below(r, 40);
        op[FP_L1 + 2] = t > 60 ? t : 80;
        op[FP_R1 + 2] = (uint8_t)rnd_in(r, 5, 30);
        break;
    }
    case 2:
        p[FP_FB] = 7;
        break;
    case 3:                                             /* the LFO taken too far */
        p[FP_LFS] = (uint8_t)rnd_in(r, 60, 99);
        p[FP_LPMD] = (uint8_t)rnd_in(r, 40, 99);
        p[FP_LPMS] = 7;
        break;
    case 4:
        p[FP_TRNSP] = (uint8_t)clampi(p[FP_TRNSP] + (rnd_chance(r, 50) ? 12 : -12), 0, 48);
        break;
    default:                                            /* an algorithm from another family */
        p[FP_ALG] = (uint8_t)rnd_below(r, 32);
        break;
    }
}


/* ------------------------------------------------- generation 1's families --- */
static void fm_gen_choir(uint8_t *p, rnd_t *r)
{
    static const uint8_t F1[4] = {3, 4, 5, 6}, F2[5] = {7, 8, 9, 11, 13};   /* harmonics near two formants */
    int n;
    p[FP_ALG] = 4;                                       /* three pairs: carriers 1, 3, 5 */
    p[FP_FB] = (uint8_t)rnd_below(r, 3);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            int32_t h = n == 1 ? 1 : n == 3 ? F1[rnd_below(r, 4)] : F2[rnd_below(r, 5)];
            fm_ratio(op, h * 100);
            fm_env(op, rnd_in(r, 18, 40), rnd_in(r, 20, 40), 20, rnd_in(r, 18, 34), 99, 96, 92, 0);
            op[FP_OL] = (uint8_t)(n == 1 ? 99 : n == 3 ? rnd_in(r, 84, 93) : rnd_in(r, 74, 86));
            op[FP_DET] = (uint8_t)rnd_in(r, 5, 9);
        } else {
            fm_ratio(op, 100);                           /* the fundamental: sidebands around each formant */
            fm_env(op, rnd_in(r, 15, 35), 25, 20, 30, rnd_in(r, 80, 99), rnd_in(r, 70, 95), rnd_in(r, 70, 92), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 50, 70);
        }
    }
    p[FP_LFW] = 4;                                       /* a sine vibrato, late */
    p[FP_LFS] = (uint8_t)rnd_in(r, 28, 40);
    p[FP_LFD] = (uint8_t)rnd_in(r, 40, 75);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 4, 10);
    p[FP_LPMS] = 3;
    p[FP_LAMD] = 0;
}
static void fm_gen_bowed(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = 0;                                       /* 2 > 1, and 6 > 5 > 4 > 3 with the feedback on 6 */
    p[FP_FB] = (uint8_t)rnd_in(r, 5, 7);                 /* a saw */
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            fm_ratio(op, n == 1 && rnd_chance(r, 40) ? 200 : 100);
            fm_env(op, rnd_in(r, 22, 45), 30, 25, rnd_in(r, 22, 38), 99, 97, 94, 0);
            op[FP_OL] = (uint8_t)(n == 3 ? 99 : rnd_in(r, 80, 92));
            op[FP_DET] = (uint8_t)(n == 1 ? rnd_in(r, 3, 6) : rnd_in(r, 8, 11));
            op[FP_RS] = 2;
        } else {                                         /* the bow: the index rising with the swell, a little slower */
            fm_ratio(op, n == 2 ? 100 : rnd_chance(r, 70) ? 100 : 200);
            fm_env(op, rnd_in(r, 14, 32), rnd_in(r, 15, 30), 15, 30, rnd_in(r, 85, 99), rnd_in(r, 75, 95), rnd_in(r, 70, 90), 0);
            op[FP_OL] = (uint8_t)(n == 6 ? rnd_in(r, 70, 85) : rnd_in(r, 55, 75));
        }
    }
    p[FP_LFW] = 4;
    p[FP_LFS] = (uint8_t)rnd_in(r, 30, 42);
    p[FP_LFD] = (uint8_t)rnd_in(r, 50, 80);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 4, 9);
    p[FP_LPMS] = 3;
}
static void fm_gen_hollow(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = 4;                                       /* three pairs */
    p[FP_FB] = 0;
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            int32_t c = n == 5 && rnd_chance(r, 50) ? 50 : 100;   /* (one an octave down, now and then) */
            fm_ratio(op, c);
            fm_env(op, rnd_in(r, 12, 30), 25, 20, rnd_in(r, 15, 30), 99, 98, 95, 0);
            op[FP_OL] = (uint8_t)(n == 1 ? 99 : rnd_in(r, 78, 92));
            op[FP_DET] = (uint8_t)(n == 1 ? 7 : rnd_chance(r, 50) ? rnd_in(r, 4, 6) : rnd_in(r, 8, 10));
        } else {
            uint8_t *c = fm_op(p, n - 1);
            fm_ratio(op, c[FP_FC] == 0 ? 100 : 200);      /* twice the carrier: odd harmonics only */
            fm_env(op, rnd_in(r, 10, 28), 20, 15, 30, rnd_in(r, 70, 95), rnd_in(r, 60, 90), rnd_in(r, 55, 85), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 45, 68);
        }
    }
    p[FP_LFW] = 4;
    p[FP_LFS] = (uint8_t)rnd_in(r, 4, 14);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 5, 20);
    p[FP_LPMD] = 0;
}
static void fm_gen_bloom(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = rnd_chance(r, 60) ? ALG_PAIRS[rnd_below(r, 6)] : ALG_DEEP[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_below(r, 5);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            fm_ratio(op, rnd_chance(r, 80) ? 100 * (1 + rnd_below(r, 2)) : inharm_ratio(r));
            fm_env(op, rnd_in(r, 25, 55), 30, 20, rnd_in(r, 18, 34), 99, 98, 95, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 88, 99);
        } else {                                         /* low, opening over seconds, half closing again */
            fm_ratio(op, rnd_chance(r, 70) ? 100 * (1 + rnd_below(r, 3)) : inharm_ratio(r));
            fm_env(op, rnd_in(r, 30, 60), rnd_in(r, 6, 16), rnd_in(r, 5, 14), 30, rnd_in(r, 15, 40), rnd_in(r, 88, 99),
                   rnd_in(r, 55, 80), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 62, 84);
        }
    }
    p[FP_LFS] = (uint8_t)rnd_in(r, 3, 12);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 0, 4);
}
static void fm_gen_tide(uint8_t *p, rnd_t *r)
{
    static const int16_t RAT[6] = {50, 100, 100, 200, 300, 150};
    int n, k = 0;
    p[FP_ALG] = 4;                                       /* three stacks */
    p[FP_FB] = (uint8_t)rnd_in(r, 2, 5);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {                       /* one fades as the next swells */
            fm_ratio(op, RAT[rnd_below(r, 6)]);
            if (k == 0)
                fm_env(op, rnd_in(r, 30, 50), rnd_in(r, 6, 14), 10, 25, 99, rnd_in(r, 40, 65), rnd_in(r, 30, 55), 0);
            else if (k == 1)
                fm_env(op, rnd_in(r, 6, 14), 20, 15, 25, 99, 97, 95, 0);
            else
                fm_env(op, rnd_in(r, 4, 10), 20, 15, 25, 99, 96, 92, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 86, 98);
            op[FP_AMS] = (uint8_t)rnd_in(r, 1, 2);
            op[FP_DET] = (uint8_t)rnd_below(r, 15);
            k++;
        } else {
            fm_ratio(op, RAT[rnd_below(r, 6)] + rnd_below(r, 3));
            fm_env(op, rnd_in(r, 8, 30), rnd_in(r, 5, 15), 10, 30, rnd_in(r, 60, 95), rnd_in(r, 40, 90), rnd_in(r, 50, 85), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 50, 76);
        }
    }
    p[FP_LFW] = 4;                                       /* the tide */
    p[FP_LFS] = (uint8_t)rnd_in(r, 2, 9);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 18, 40);
    p[FP_LPMD] = 0;
}
static void fm_gen_shards(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = rnd_chance(r, 50) ? ALG_MANY[rnd_below(r, 6)] : ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_below(r, 7);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        fm_ratio(op, rnd_chance(r, 60) ? inharm_ratio(r) : 100 * (1 + rnd_below(r, 4)));
        if (fm_is_carrier(p, n)) {
            fm_env(op, rnd_in(r, 70, 99), rnd_in(r, 30, 60), 25, rnd_in(r, 40, 70), 99, rnd_in(r, 75, 95), rnd_in(r, 65, 90), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 84, 99);
            op[FP_AMS] = 3;                              /* chopped */
        } else {
            fm_env(op, rnd_in(r, 60, 99), rnd_in(r, 20, 60), 25, 40, 99, rnd_in(r, 60, 99), rnd_in(r, 50, 90), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 60, 90);
        }
    }
    p[FP_LFW] = 5;                                       /* sample and hold: a new pitch, a new level, each step */
    p[FP_LFS] = (uint8_t)rnd_in(r, 62, 90);
    p[FP_LPMS] = (uint8_t)rnd_in(r, 4, 6);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 20, 60);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 60, 99);
}
static void fm_gen_static(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_in(r, 3, 7);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            fm_ratio(op, rnd_chance(r, 70) ? 100 : inharm_ratio(r));
            fm_env(op, rnd_in(r, 50, 90), 30, 25, rnd_in(r, 30, 60), 99, rnd_in(r, 85, 99), rnd_in(r, 75, 95), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 85, 99);
        } else {                                         /* nailed to audio frequencies: ring modulation */
            fm_fixed(op, rnd_in(r, 2, 3), rnd_below(r, 100));
            fm_env(op, rnd_in(r, 40, 90), rnd_in(r, 10, 40), 15, 40, 99, rnd_in(r, 70, 99), rnd_in(r, 60, 95), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 75, 97);
        }
    }
    if (rnd_chance(r, 50)) {                             /* a square tremolo: a stutter */
        p[FP_LFW] = 3;
        p[FP_LFS] = (uint8_t)rnd_in(r, 40, 80);
        p[FP_LAMD] = (uint8_t)rnd_in(r, 50, 99);
        for (n = 1; n <= 6; n++)
            if (fm_is_carrier(p, n))
                fm_op(p, n)[FP_AMS] = 3;
    }
}
static void fm_gen_wind(uint8_t *p, rnd_t *r)
{
    static const int16_t DRONE[5] = {50, 100, 100, 200, 150};
    int n;
    p[FP_ALG] = 31;                                      /* six carriers, the feedback on 6: the breath */
    p[FP_FB] = 7;
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (n == 6) {
            fm_ratio(op, rnd_in(r, 600, 1400));
            fm_env(op, rnd_in(r, 10, 30), 20, 15, rnd_in(r, 15, 30), 99, 97, 95, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 74, 86);
            op[FP_AMS] = 3;                              /* gusts */
        } else {
            fm_ratio(op, DRONE[rnd_below(r, 5)] + rnd_below(r, 2));
            fm_env(op, rnd_in(r, 10, 28), 20, 15, rnd_in(r, 15, 30), 99, 96, 92, 0);
            op[FP_OL] = (uint8_t)(n == 1 ? rnd_in(r, 78, 90) : rnd_in(r, 55, 78));
            op[FP_DET] = (uint8_t)rnd_below(r, 15);
        }
    }
    p[FP_LFW] = rnd_chance(r, 50) ? 4 : 5;
    p[FP_LFS] = (uint8_t)rnd_in(r, 6, 22);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 35, 75);
    p[FP_LPMD] = 0;
}

/* ------------------------------------------------- generation 3's families --- */
/* plucked strings, bright or dark: the modulators die quickly (the attack's brightness), the carriers die away */
static void fm_gen_pluck(uint8_t *p, rnd_t *r)
{
    static const uint8_t ALG[6] = {4, 5, 0, 1, 15, 21};
    int n, dark = rnd_chance(r, 45);
    p[FP_ALG] = ALG[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_in(r, 0, dark ? 3 : 6);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            fm_ratio(op, rnd_chance(r, 75) ? 100 : rnd_chance(r, 50) ? 200 : 50);
            fm_env(op, 99, rnd_in(r, 28, 52), rnd_in(r, 20, 40), rnd_in(r, 35, 60), 99, rnd_in(r, 40, 75), 0, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 92, 99);
            op[FP_DET] = (uint8_t)rnd_in(r, 5, 9);
        } else {
            static const int16_t BR[6] = {100, 200, 300, 100, 400, 500};
            fm_ratio(op, dark ? (rnd_chance(r, 70) ? 100 : 200) : BR[rnd_below(r, 6)]);
            fm_env(op, 99, rnd_in(r, dark ? 55 : 45, dark ? 85 : 75), rnd_in(r, 30, 60), rnd_in(r, 40, 70), 99,
                   rnd_in(r, 0, dark ? 25 : 45), 0, 0);
            op[FP_OL] = (uint8_t)(dark ? rnd_in(r, 55, 76) : rnd_in(r, 74, 92));
        }
        op[FP_RS] = (uint8_t)rnd_in(r, 3, 6);              /* (higher strings die quicker) */
        op[FP_KVS] = (uint8_t)rnd_in(r, 0, 3);
    }
    p[FP_LAMD] = 0;
    p[FP_LPMD] = (uint8_t)rnd_in(r, 0, 4);
    if (dark && p[FP_TRNSP] >= 12u && rnd_chance(r, 40))
        p[FP_TRNSP] -= 12;
}
/* struck bars: marimba, vibraphone, kalimba -- a high modulator's knock, gone at once; the bar rings on */
static void fm_gen_mallet(uint8_t *p, rnd_t *r)
{
    static const int16_t KNOCK[6] = {350, 400, 700, 1000, 540, 314};
    int n;
    p[FP_ALG] = ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_below(r, 3);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            fm_ratio(op, rnd_chance(r, 70) ? 100 : rnd_chance(r, 50) ? 400 : 1000);
            fm_env(op, 99, rnd_in(r, 32, 55), rnd_in(r, 25, 45), rnd_in(r, 35, 60), 99, rnd_in(r, 30, 60), 0, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 88, 99);
            if (rnd_chance(r, 30))
                op[FP_AMS] = 2;                            /* (a vibraphone's motor) */
        } else {
            fm_ratio(op, KNOCK[rnd_below(r, 6)] + rnd_in(r, -6, 6));
            fm_env(op, 99, rnd_in(r, 75, 95), rnd_in(r, 50, 80), 60, 99, rnd_in(r, 0, 20), 0, 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 68, 90);
        }
        op[FP_RS] = (uint8_t)rnd_in(r, 2, 5);
    }
    p[FP_LFW] = 4;
    p[FP_LFS] = (uint8_t)rnd_in(r, 30, 45);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 0, 30);
    p[FP_LPMD] = 0;
}
/* a warp: ratios anywhere, partials nailed to frequencies, the pitch thrown about by its envelope */
static void fm_gen_warp(uint8_t *p, rnd_t *r)
{
    int n, i;
    p[FP_ALG] = (uint8_t)rnd_below(r, 32);
    p[FP_FB] = (uint8_t)rnd_in(r, 4, 7);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {                         /* (the carriers heard: their levels up, their ratios low) */
            fm_ratio(op, rnd_in(r, 50, 400));
            fm_env(op, rnd_in(r, 20, 99), rnd_in(r, 5, 60), rnd_in(r, 5, 60), rnd_in(r, 20, 60), rnd_in(r, 80, 99),
                   rnd_in(r, 55, 99), rnd_in(r, 65, 99), 0);
        } else {
            if (rnd_chance(r, 35))
                fm_fixed(op, rnd_in(r, 0, 3), rnd_below(r, 100));
            else
                fm_ratio(op, rnd_in(r, 50, 1200));
            fm_env(op, rnd_in(r, 20, 99), rnd_in(r, 5, 60), rnd_in(r, 5, 60), rnd_in(r, 20, 60), rnd_in(r, 50, 99),
                   rnd_below(r, 99), rnd_in(r, 30, 99), 0);
        }
        op[FP_OL] = (uint8_t)(fm_is_carrier(p, n) ? rnd_in(r, 85, 99) : rnd_in(r, 55, 95));
        op[FP_DET] = (uint8_t)rnd_below(r, 15);
    }
    for (i = 0; i < 4; i++) {                              /* the pitch envelope: anywhere, at any pace */
        p[FP_PR1 + i] = (uint8_t)rnd_in(r, 20, 80);
        p[FP_PL1 + i] = (uint8_t)rnd_in(r, 33, 67);         /* (bent, never thrown out of hearing) */
    }
    p[FP_PL1 + 2] = (uint8_t)rnd_in(r, 40, 60);            /* (it settles near the note) */
    p[FP_LFW] = (uint8_t)rnd_below(r, 6);
    p[FP_LFS] = (uint8_t)rnd_in(r, 5, 60);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 0, 40);
    p[FP_LPMS] = (uint8_t)rnd_in(r, 2, 6);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 0, 40);
}
/* a glitch: a quick sample-and-hold stepping the pitch and the level, envelopes chopped short */
static void fm_gen_glitch(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = rnd_chance(r, 50) ? ALG_PAIRS[rnd_below(r, 6)] : ALG_MANY[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_in(r, 2, 7);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        fm_ratio(op, 100 * rnd_in(r, 1, 8) / (rnd_chance(r, 25) ? 2 : 1));
        if (fm_is_carrier(p, n)) {
            fm_env(op, 99, rnd_in(r, 40, 80), rnd_in(r, 30, 70), rnd_in(r, 50, 80), 99, rnd_in(r, 60, 99), rnd_in(r, 40, 90), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 88, 99);
            op[FP_AMS] = (uint8_t)rnd_in(r, 2, 3);
        } else {
            fm_env(op, 99, rnd_in(r, 50, 99), rnd_in(r, 40, 80), 60, 99, rnd_below(r, 99), rnd_below(r, 99), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 60, 92);
        }
    }
    p[FP_LFW] = 5;
    p[FP_LFS] = (uint8_t)rnd_in(r, 70, 99);
    p[FP_LPMS] = (uint8_t)rnd_in(r, 5, 7);
    p[FP_LPMD] = (uint8_t)rnd_in(r, 25, 99);
    p[FP_LAMD] = (uint8_t)rnd_in(r, 30, 90);
}
/* rubber: low ratios, the pitch bouncing in at every note (up, under, back) */
static void fm_gen_rubber(uint8_t *p, rnd_t *r)
{
    int n;
    p[FP_ALG] = rnd_chance(r, 50) ? ALG_DEEP[rnd_below(r, 6)] : ALG_PAIRS[rnd_below(r, 6)];
    p[FP_FB] = (uint8_t)rnd_in(r, 2, 6);
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        fm_ratio(op, rnd_chance(r, 50) ? 100 : rnd_chance(r, 50) ? 50 : 200);
        if (fm_is_carrier(p, n)) {
            fm_env(op, 99, rnd_in(r, 25, 50), rnd_in(r, 20, 40), rnd_in(r, 35, 60), 99, rnd_in(r, 50, 85), rnd_in(r, 0, 60), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 90, 99);
        } else {
            fm_env(op, 99, rnd_in(r, 30, 60), rnd_in(r, 20, 50), 50, 99, rnd_in(r, 40, 80), rnd_in(r, 20, 60), 0);
            op[FP_OL] = (uint8_t)rnd_in(r, 65, 88);
        }
    }
    p[FP_PL1 + 3] = (uint8_t)rnd_in(r, 15, 35);            /* from under the note, */
    p[FP_PL1] = (uint8_t)rnd_in(r, 62, 80);                /* up past it, */
    p[FP_PL1 + 1] = (uint8_t)rnd_in(r, 42, 48);            /* back under, */
    p[FP_PL1 + 2] = 50;                                    /* to it */
    p[FP_PR1] = (uint8_t)rnd_in(r, 70, 90);
    p[FP_PR1 + 1] = (uint8_t)rnd_in(r, 60, 85);
    p[FP_PR1 + 2] = (uint8_t)rnd_in(r, 55, 80);
    p[FP_PR1 + 3] = 99;
    p[FP_LAMD] = 0;
    p[FP_LPMD] = 0;
}
/* generation 3's breadth, on every family (after all the earlier draws): now and then plucked, a modulator moved
 * to another ratio, an operator detuned */
static void fm_care3(uint8_t *p, int32_t f, uint32_t path, rnd_t *r)
{
    static const int16_t R3[8] = {50, 150, 250, 300, 350, 450, 600, 700};
    int n;
    if (f < FF_N1 && rnd_chance(r, path == PATH_AMBIENT ? 6 : 14))   /* plucked: struck, dying away */
        for (n = 1; n <= 6; n++) {
            uint8_t *op = fm_op(p, n);
            op[FP_R1] = 99;
            op[FP_R1 + 1] = (uint8_t)rnd_in(r, fm_is_carrier(p, n) ? 30 : 55, fm_is_carrier(p, n) ? 55 : 85);
            op[FP_L1] = 99;
            op[FP_L1 + 2] = 0;
            op[FP_R1 + 3] = (uint8_t)clampi(op[FP_R1 + 3], 30, 70);
        }
    if (rnd_chance(r, 35)) {
        uint8_t *op = fm_op(p, 1 + rnd_below(r, 6));
        if (!op[FP_MODE] && !fm_is_carrier(p, (int)(6 - (op - p) / FP_OP)))
            fm_ratio(op, R3[rnd_below(r, 8)]);
    }
    if (rnd_chance(r, 30))
        fm_op(p, 1 + rnd_below(r, 6))[FP_DET] = (uint8_t)(rnd_chance(r, 50) ? rnd_in(r, 0, 3) : rnd_in(r, 11, 14));
}

/* generation 1's care, on every patch: no screaming sidebands, the brightness scaled down the keyboard */
static void fm_shape1(uint8_t *p, uint32_t path, rnd_t *r)
{
    int n;
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (!fm_is_carrier(p, n)) {
            int32_t ratio = op[FP_MODE] ? 0 : (op[FP_FC] ? op[FP_FC] : 1) * (100 + op[FP_FF]) / 100;
            if (ratio > 4 && path != PATH_EXP)          /* sidebands far up alias and scream */
                op[FP_OL] = (uint8_t)clampi(op[FP_OL] - clampi((ratio - 4) * 2, 0, 12), 0, 99);
            op[FP_BP] = (uint8_t)rnd_in(r, 36, 46);    /* about C3 .. A3 */
            op[FP_RD] = (uint8_t)rnd_in(r, 12, path == PATH_EXP ? 25 : 40);   /* up the keyboard: less bright */
            op[FP_RC] = 1;                              /* (-EXP) */
            op[FP_LD] = (uint8_t)rnd_in(r, 0, 18);      /* down it: less mud */
            op[FP_LC] = 0;                              /* (-LIN) */
        } else if (op[FP_RS] == 0)
            op[FP_RS] = (uint8_t)rnd_in(r, 1, 3);       /* the higher, the quicker its envelope, a little */
    }
}
/* generation 1's mutations: a patch bent, not broken */
static void fm_mutate1(uint8_t *p, uint32_t path, rnd_t *r)
{
    int n = 1 + rnd_below(r, 6);
    uint8_t *op = fm_op(p, n);
    switch (rnd_below(r, 7)) {
    case 0:                                             /* one operator detuned against the rest: a slow beat */
        op[FP_DET] = (uint8_t)(rnd_chance(r, 50) ? rnd_in(r, 0, 3) : rnd_in(r, 11, 14));
        break;
    case 1: {                                           /* an envelope turned over: it grows as it holds */
        uint8_t t = op[FP_L1];
        op[FP_L1] = (uint8_t)rnd_below(r, 40);
        op[FP_L1 + 2] = t > 60 ? t : 80;
        op[FP_R1 + 2] = (uint8_t)rnd_in(r, 5, 30);
        break;
    }
    case 2:                                             /* the LFO waits, then moves the timbre */
        p[FP_LFD] = (uint8_t)rnd_in(r, 40, 90);
        if (!fm_is_carrier(p, n))
            op[FP_AMS] = (uint8_t)rnd_in(r, 1, 3);
        if (!p[FP_LAMD])
            p[FP_LAMD] = (uint8_t)rnd_in(r, 15, 45);
        break;
    case 3:                                             /* an octave down */
        if (p[FP_TRNSP] >= 12)
            p[FP_TRNSP] -= 12;
        break;
    case 4:                                             /* a partial nailed to a frequency (the edgier paths only) */
        if (path != PATH_AMBIENT)
            fm_fixed(op, rnd_in(r, 1, 3), rnd_below(r, 100));
        break;
    case 5:                                             /* more feedback */
        p[FP_FB] = (uint8_t)clampi(p[FP_FB] + rnd_in(r, 1, 3), 0, path == PATH_AMBIENT ? 5 : 7);
        break;
    default:                                            /* the modulator's ratio moved to a neighbour */
        if (!fm_is_carrier(p, n) && !op[FP_MODE])
            fm_ratio(op, clampi(((op[FP_FC] ? op[FP_FC] : 1) * (100 + op[FP_FF])) + (rnd_chance(r, 50) ? 100 : -50), 50, 3100));
        break;
    }
}

/* AMBIENT: slower, darker, longer, stiller */
static void fm_ambient(uint8_t *p, int32_t f, rnd_t *r)
{
    int n, i;
    for (n = 1; n <= 6; n++) {
        uint8_t *op = fm_op(p, n);
        if (fm_is_carrier(p, n)) {
            if ((f != FF_METAL && f != FF_PLUCK && f != FF_MALLET) || rnd_chance(r, 50))
                                                              /* swells (a bell, a pluck may keep its strike) */
                op[FP_R1] = (uint8_t)clampi(op[FP_R1], 0, rnd_in(r, 18, 55));
            op[FP_R1 + 3] = (uint8_t)clampi(op[FP_R1 + 3], 0, rnd_in(r, 8, 28));   /* long tails */
        } else
            op[FP_OL] = (uint8_t)clampi(op[FP_OL] - rnd_in(r, 4, 14), 0, 99);      /* darker */
    }
    p[FP_FB] = (uint8_t)clampi(p[FP_FB], 0, 5);
    p[FP_LFS] = (uint8_t)clampi(p[FP_LFS], 0, 28);
    if (p[FP_LPMS] >= 5u) {                                   /* no sirens */
        p[FP_LPMS] = 3;
        p[FP_LPMD] = (uint8_t)clampi(p[FP_LPMD], 0, 10);
    }
    for (i = 0; i < 4; i++)                                   /* the glissandi narrowed to a third */
        p[FP_PL1 + i] = (uint8_t)(50 + (p[FP_PL1 + i] - 50) / 3);
    if (p[FP_TRNSP] >= 24u && rnd_chance(r, 35))
        p[FP_TRNSP] -= 12;                                    /* an octave down */
}
/* EXPERIMENTAL: brighter, harder */
static void fm_experimental(uint8_t *p, rnd_t *r)
{
    int n;
    for (n = 1; n <= 6; n++)
        if (!fm_is_carrier(p, n))
            fm_op(p, n)[FP_OL] = (uint8_t)clampi(fm_op(p, n)[FP_OL] + rnd_in(r, 0, 10), 0, 99);
}

/* a patch of family f (FF_N: any), from seed, on a path. Rhythm: no fm_measure (see header) */
static uint32_t fm_generate(uint8_t *p, int32_t f, uint32_t seed, uint32_t path, uint32_t gen)
{
    rnd_t rr, *r = &rr;
    path = path < PATH_N ? path : PATH_OPEN;
    fm_gen_cur = gen;
    rnd_seed(r, seed ^ 0xF00Du);
    if (gen == 0u && (f < 0 || f >= FF_N0))
        f = path == PATH_OPEN ? rnd_below(r, FF_N0) : (int32_t)rnd_weighted(r, FF_W[path], FF_N0);
    else if (gen <= 2u && (f < 0 || f >= FF_N1))
        f = (int32_t)rnd_weighted(r, FF_W1[path], FF_N1);
    else if (f < 0 || f >= FF_N)
        f = (int32_t)rnd_weighted(r, FF_W3[path], FF_N);
    fm_voice_defaults(p);
    switch (f) {
    case FF_METAL: fm_gen_metal(p, r); break;
    case FF_DRONE: fm_gen_drone(p, r); break;
    case FF_BRUIT: fm_gen_bruit(p, r); break;
    case FF_IMPACT: fm_gen_impact(p, r); break;
    case FF_ITERATIF: fm_gen_iteratif(p, r); break;
    case FF_GLISSE: fm_gen_glisse(p, r); break;
    case FF_SPECTRE: fm_gen_spectre(p, r); break;
    case FF_ABYSS: fm_gen_abyss(p, r); break;
    case FF_FRICTION: fm_gen_friction(p, r); break;
    case FF_CHOIR: fm_gen_choir(p, r); break;
    case FF_BOWED: fm_gen_bowed(p, r); break;
    case FF_HOLLOW: fm_gen_hollow(p, r); break;
    case FF_BLOOM: fm_gen_bloom(p, r); break;
    case FF_TIDE: fm_gen_tide(p, r); break;
    case FF_SHARDS: fm_gen_shards(p, r); break;
    case FF_STATIC: fm_gen_static(p, r); break;
    case FF_PLUCK: fm_gen_pluck(p, r); break;
    case FF_MALLET: fm_gen_mallet(p, r); break;
    case FF_WARP: fm_gen_warp(p, r); break;
    case FF_GLITCH: fm_gen_glitch(p, r); break;
    case FF_RUBBER: fm_gen_rubber(p, r); break;
    default: fm_gen_wind(p, r); break;
    }
    if (gen >= 1u) {                                     /* generation 1: shaped, bent with care, then leaned */
        fm_shape1(p, path, r);
        if (rnd_chance(r, path == PATH_EXP ? 70 : path == PATH_AMBIENT ? 30 : 45))
            fm_mutate1(p, path, r);
        if (path == PATH_EXP && rnd_chance(r, 35))
            fm_mutate1(p, path, r);
        if (gen >= 3u)
            fm_care3(p, f, path, r);
        if (path == PATH_AMBIENT)
            fm_ambient(p, f, r);
        else if (path == PATH_EXP)
            fm_experimental(p, r);
    } else if (path == PATH_AMBIENT) {
        if (rnd_chance(r, 15)) {                              /* gently: an envelope turned over */
            uint8_t *op = fm_op(p, 1 + rnd_below(r, 6)), t = op[FP_L1];
            op[FP_L1] = (uint8_t)rnd_below(r, 40);
            op[FP_L1 + 2] = t > 60 ? t : 80;
            op[FP_R1 + 2] = (uint8_t)rnd_in(r, 5, 30);
        }
        fm_ambient(p, f, r);
    } else if (path == PATH_EXP) {
        fm_experimental(p, r);
        if (rnd_chance(r, 60))
            fm_mutate(p, r);
        if (rnd_chance(r, 25))
            fm_mutate(p, r);
    } else if (rnd_chance(r, 33))
        fm_mutate(p, r);
    {   /* every carrier sounding, so a patch is never silent: the loudest at full level */
        int n, any = 0;
        for (n = 1; n <= 6; n++)
            if (fm_is_carrier(p, n) && fm_op(p, n)[FP_OL] >= 80)
                any = 1;
        if (!any)
            for (n = 1; n <= 6; n++)
                if (fm_is_carrier(p, n)) {
                    fm_op(p, n)[FP_OL] = 99;
                    if (fm_op(p, n)[FP_L1] < 90)
                        fm_op(p, n)[FP_L1] = 99;
                    break;
                }
    }
    fm_name(p, FF_NAME[f]);
    fm6_sanitize(p);
    return (uint32_t)f;
}

/* ===================== Rhythm-specific selection and shaping =====================
 * Everything below is new for Circuit Rhythm (not from Tonitor).
 * Two lanes, drawn 50/50 from the seed:
 *   DARK AMBIENT    Tonitor's AMBIENT path (slower, darker, longer) over drones,
 *                   depths, spectra, tides, blooms, hollow pipes, wind, voices, bells
 *   RHYTHMIC NOISE  Tonitor's EXPERIMENTAL path (brighter, harder) over noise,
 *                   impacts, friction, glitches, ring-mod static, metal, shards,
 *                   mallets, then made percussive (instant attack, short decays)
 * GLISSE, WARP and RUBBER are never drawn, and every patch is finished with no
 * pitch envelope at all and at most a slight vibrato: no glides, sirens or
 * laser sweeps. */
#define LANE_AMBIENT 0u
#define LANE_NOISE 1u
static const uint8_t RHYTHM_W[2][FF_N] = {
  /* MET DRO BRU IMP ITE GLI SPE ABY FRI CHO BOW HOL BLO TID SHA STA WIN PLU MAL WAR GLT RUB */
    {2,  4,  0,  0,  0,  0,  3,  4,  1,  2,  2,  3,  3,  4,  0,  1,  3,  0,  0,  0,  0,  0},
    {3,  0,  5,  5,  2,  0,  0,  1,  3,  0,  0,  0,  0,  0,  2,  3,  1,  0,  2,  0,  3,  0},
};
static void rhythm_shape(uint8_t *p, uint32_t lane, rnd_t *r)
{
    int n, i;
    for (i = 0; i < 4; i++) {                          /* no pitch envelope: no glides */
        p[FP_PR1 + i] = 99;
        p[FP_PL1 + i] = 50;
    }
    if (p[FP_LPMS] > 3u)                               /* no sirens: a slight vibrato at most */
        p[FP_LPMS] = 3;
    if (p[FP_LPMD] > 6u)
        p[FP_LPMD] = (uint8_t)rnd_in(r, 0, 6);
    if (lane == LANE_NOISE) {
        int hit = rnd_chance(r, 65);                   /* a one-shot hit, or a short gated burst */
        for (n = 1; n <= 6; n++) {
            uint8_t *op = fm_op(p, n);
            if (fm_is_carrier(p, n)) {
                op[FP_R1] = 99;
                op[FP_L1] = 99;
                /* DX7 rates: ~55 decays in about a second, ~85 in tens of ms */
                op[FP_R1 + 1] = (uint8_t)(hit ? rnd_in(r, 60, 92) : rnd_in(r, 40, 85));
                op[FP_L1 + 1] = (uint8_t)rnd_in(r, 25, hit ? 65 : 80);
                op[FP_R1 + 2] = (uint8_t)(hit ? rnd_in(r, 55, 85) : rnd_in(r, 30, 75));
                op[FP_L1 + 2] = (uint8_t)(hit ? 0 : clampi(op[FP_L1 + 2], 0, 60));
                op[FP_R1 + 3] = (uint8_t)rnd_in(r, 45, 85);
                op[FP_L1 + 3] = 0;
            } else if (op[FP_R1] < 80u)
                op[FP_R1] = (uint8_t)rnd_in(r, 80, 99);    /* the modulators bite at once */
        }
        if (rnd_chance(r, 50))
            p[FP_FB] = (uint8_t)clampi(p[FP_FB] + rnd_in(r, 1, 3), 0, 7);   /* rougher */
        if (p[FP_TRNSP] >= 12u && rnd_chance(r, 30))
            p[FP_TRNSP] -= 12;                         /* heavier */
    } else {
        for (n = 1; n <= 6; n++) {
            uint8_t *op = fm_op(p, n);
            if (fm_is_carrier(p, n))
                op[FP_R1 + 3] = (uint8_t)clampi(op[FP_R1 + 3], 0, 35);   /* long tails */
            else
                op[FP_OL] = (uint8_t)clampi(op[FP_OL] - rnd_in(r, 0, 8), 0, 99);   /* darker still */
        }
    }
    {   /* keep one carrier clearly audible after the reshaping */
        int any = 0;
        for (n = 1; n <= 6; n++)
            if (fm_is_carrier(p, n) && fm_op(p, n)[FP_OL] >= 80)
                any = 1;
        if (!any)
            for (n = 1; n <= 6; n++)
                if (fm_is_carrier(p, n)) {
                    fm_op(p, n)[FP_OL] = 99;
                    break;
                }
    }
}
/* A Rhythm patch from a seed: lane, family, Tonitor's generator (generation 3),
   then the shaping above. Returns the family, with bit 7 set for the noise lane. */
static uint32_t rhythm_generate(uint8_t *p, uint32_t seed)
{
    rnd_t rr, *r = &rr;
    uint32_t lane, f;
    rnd_seed(r, seed ^ 0x52484d31u);
    lane = rnd_chance(r, 50) ? LANE_NOISE : LANE_AMBIENT;
    f = rnd_weighted(r, RHYTHM_W[lane], FF_N);
    fm_generate(p, (int32_t)f, seed, lane == LANE_NOISE ? PATH_EXP : PATH_AMBIENT, 3u);
    rhythm_shape(p, lane, r);
    fm6_sanitize(p);
    return f | lane << 7;
}
#pragma clang attribute pop
