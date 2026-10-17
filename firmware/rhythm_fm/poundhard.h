/* SPDX-License-Identifier: GPL-3.0-only
 * Circuit Rhythm port of Vascular's poundhard FM drum voice
 * (vascular-ios Sources/CVascularOpcodes/src/PoundhardCore.h, commit
 * b79f71ede290e1a28dc88d5b9df17a598ee31fc5), in C with single-precision floats.
 *
 * Kept: three operators in seven algorithms with feedback, the pitch sweep,
 * body envelope, wavefolder and level, the second carrier, noise with grain
 * and ring, the measured transient, the noise band and envelope, all six
 * filters (multimode, four-pole, legacy, comb -/+, peaking EQ) with their
 * envelope, the base-width band, the amplitude envelope, overdrive, sample-
 * rate and bit reduction, the soft ceiling, and `design` for all eleven kinds
 * with the clap's own burst LFO.
 * Changed for the Rhythm: the waveforms are 33 of the 128 frames as 128-point
 * cycles (generated/poundhard_tables.h), and each strike fixes one carrier and
 * one modulator cycle, interpolated between frames, in the voice (no LFO moves
 * them); control values are computed every 16 samples as in the original, with
 * per-sample exponentials replaced by per-block coefficients; the comb's
 * delay is 256 int16 samples (lowest comb pitch ~188 Hz); no delay/reverb/chorus
 * sends (the Rhythm's own FX sends apply), no tempo-synced random LFOs (a
 * patch's LFO seed is 0), sliders at their middles.
 */
#pragma once
#include <stdint.h>

enum { PH_KICK, PH_SNARE, PH_HIHAT, PH_CLAP, PH_TOM, PH_CLANG, PH_METAL, PH_ANVIL, PH_RUST, PH_ZAP, PH_GLITCH, PH_KINDS };
/* The Rhythm's eight pads, by Vascular's drum roles (OrganismPoundhard.swift
   poundhardRole): kick, snare, hat, clap, tom, metal, noise, click. */
enum { PH_ROLE_KICK, PH_ROLE_SNARE, PH_ROLE_HAT, PH_ROLE_CLAP, PH_ROLE_TOM, PH_ROLE_METAL, PH_ROLE_NOISE, PH_ROLE_CLICK, PH_ROLES };
/* Pad roles 0-7 with character; 8-15 the same roles gnarly (ph_character). */
#define PH_GNARLY 8u
#define PH_PADS 16u
/* The generator's eight families (track buttons 1-8). */
enum { PH_FAM_KICK, PH_FAM_SNARE, PH_FAM_HAT, PH_FAM_METAL, PH_FAM_NOISE, PH_FAM_GLITCH, PH_FAM_IMPACT, PH_FAM_GNARLY, PH_FAMILIES };

#define PH_COMB 256u

typedef struct {
    float tune, stim, sdep, waveC, waveAB, feedback, foldAmt;
    float ratioA, decA, endA, modA, ratioB, decB, endB, modB;
    float hold, decay, level;
    float noiseHold, noiseDecay, transient, transientLevel, noiseBase, noiseWidth, grain, noiseLevel;
    float cutoff, fG, fH, aHold, aDec, volume, bits, overdrive, srr, reg;
    float lfoSpeed, lfoMult, lfoDepth;          /* the clap's burst: saw, one-shot, on noise level */
    uint8_t kind, algo, filter, noiseRing, srrPost, odPost, lfoOn, rsv;
} PhPatch;

typedef struct { float level, t; uint8_t stage, rsv[3]; } PhEnv;
typedef struct { float ic1, ic2; } PhSvf;
typedef struct { float a1, a2, a3, k; } PhSvfC;   /* coefficients, fixed per strike */

typedef struct {
    PhPatch p;
    int16_t carrier[128], modulator[128];      /* this strike's cycles */
    int16_t comb[PH_COMB];
    int16_t fold[66];                           /* this strike's folder curve, Q14 */
    uint32_t phC, phA, phB, trPhase, noise;     /* phases: 2^32 per cycle */
    uint32_t incC, incA, incB, incTr;
    float fbA, fbB, fbA1, fbB1, t, lfoPhase, baseHz, velocity, declick, lastOut;
    PhEnv body, modA, modB, noiseEnv, amp;
    PhSvf trSvf, noiseHp, noiseLp, svfA;
    PhSvfC cTr, cNoiseHp, cNoiseLp, cFilt;
    float ladder[4], combLpZ, srrHold, srrCount, srrHoldPost, srrCountPost;
    /* fixed per strike */
    float holdBody, holdNoise, holdAmp, mulBody, mulNoise, mulAmp, mulBodyN, mulNoiseN, mulAmpN, mulModA, mulModB, susA, susB;
    float sweepDepth, sweepTau, level, grainP, trLevel, trTau, trTonal, trEnv, trMul, trEnd;
    float mixLp, mixBp, mixHp, gLp4, filtFb, combPeriod, combLpA, eqGain, odDrive, odNorm, bitSteps, srrStep, volume, fbAmt, modA2, modB2;
    /* control rate (every 16 samples) */
    float idxA, idxB, outB, noiseLevel;
    uint16_t combAt;
    uint8_t ctl, active, noiseHpOn, noiseLpOn, rsv[2];
} PhVoice;

/* Draw a patch of a kind (PH_KICK..) from a seed. */
void ph_design(PhPatch *p, unsigned kind, uint32_t seed);
/* The kind a pad role plays for a seed (metal and click choose among theirs; role & 7). */
unsigned ph_role_kind(unsigned role, uint32_t seed);
/* After ph_design: one of the role's archetypes and, on the gnarly row (pad >=
   PH_GNARLY) always, extra destructive treatments. pad = role | gnarly. */
void ph_character(PhPatch *p, unsigned pad, uint32_t seed);
/* A whole generated sound: the patch of family f (PH_FAM_*) for a seed, and
   the pad role it was drawn as (role | gnarly). */
unsigned ph_family(PhPatch *p, unsigned family, uint32_t seed);
/* Kits are planned and built in extension.c (kit_*), from ph_design,
   ph_character_a and ph_treat. */
/* Strike voice v with patch p at hz (before the patch's register), velocity 0..1. */
void ph_strike(PhVoice *v, const PhPatch *p, float hz, float velocity);
/* Add n samples (<= 32, at PH_SR) of v to out (float, full scale ~1). Inactive voices add nothing. */
void ph_render(PhVoice *restrict v, float *restrict out, unsigned n);
