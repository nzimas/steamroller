/* Standalone adapter for the unmodified local Tonitor FM6 core.
 * The included source retains its Apache-2.0 notices. This adapter does not
 * include Tonitor's GPL wrapper, patch generator, UI, HAL or installer.
 * TONITOR_CORE_PATH is supplied by investigate_tonitor.py at build time.
 */
#include <stdint.h>
#include "fm6_reference_tables.h"
#include TONITOR_CORE_PATH

typedef struct {
    fm6_note_t note;
    fm6_lfo_t lfo;
    uint8_t patch[155];
    int32_t pitch;
} reference_voice_t;

static void reference_patch(uint8_t *patch, unsigned algorithm)
{
    for (unsigned i = 0; i < 155; ++i) patch[i] = 0;
    for (unsigned k = 0; k < 6; ++k) {
        uint8_t *op = patch + k * FP_OP;
        op[FP_R1] = 99; op[FP_R1 + 1] = 70;
        op[FP_R1 + 2] = 60; op[FP_R1 + 3] = 65;
        op[FP_L1] = 99; op[FP_L1 + 1] = 85; op[FP_L1 + 2] = 80;
        op[FP_OL] = 85; op[FP_FC] = (uint8_t)(1 + k % 3);
        op[FP_DET] = 7;
    }
    for (unsigned k = 0; k < 4; ++k) {
        patch[FP_PR1 + k] = 99;
        patch[FP_PL1 + k] = 50;
    }
    patch[FP_ALG] = (uint8_t)algorithm;
    patch[FP_FB] = 3;
    patch[FP_LFS] = 35;
    patch[FP_LFW] = 4;
    patch[FP_TRNSP] = 24;
    for (unsigned k = FP_NAME; k < FP_SIZE; ++k) patch[k] = ' ';
}

void reference_init(reference_voice_t *voice, unsigned algorithm, unsigned note)
{
    uint8_t *raw = (uint8_t *)voice;
    for (unsigned i = 0; i < sizeof(*voice); ++i) raw[i] = 0;
    reference_patch(voice->patch, algorithm & 31);
    voice->pitch = (int32_t)note * 16;
    fm6_note_init(&voice->note, voice->patch, (int32_t)note, 100, 1);
    fm6_lfo_reset(&voice->lfo, voice->patch);
    fm6_lfo_key(&voice->lfo);
}

void reference_release(reference_voice_t *voice)
{
    fm6_note_key(&voice->note, voice->patch, 0);
}

void reference_render(reference_voice_t *voice, int32_t *output)
{
    const int32_t zero[6] = {0, 0, 0, 0, 0, 0};
    for (unsigned i = 0; i < CTL; ++i) output[i] = 0;
    fm6_note_compute(&voice->note, voice->patch, output,
                    fm6_lfo_sample(&voice->lfo), fm6_lfo_delay(&voice->lfo),
                    fm6_note_logfreq(voice->pitch, 0), voice->patch[FP_ALG],
                    voice->patch[FP_FB], zero, zero, 256);
}

#ifndef REFERENCE_FREESTANDING
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "wb");
    if (!file) return 2;
    printf("{\"sample_rate\":%u,\"block_samples\":%u,\"note_state_bytes\":%zu,"
           "\"lfo_state_bytes\":%zu,\"reference_voice_bytes\":%zu,\"algorithms\":[",
           FS, CTL, sizeof(fm6_note_t), sizeof(fm6_lfo_t), sizeof(reference_voice_t));
    for (unsigned algorithm = 0; algorithm < 32; ++algorithm) {
        reference_voice_t voice;
        reference_init(&voice, algorithm, 69);
        int32_t block[CTL];
        int64_t peak = 0;
        unsigned nonzero = 0;
        uint64_t hash = UINT64_C(14695981039346656037);
        for (unsigned b = 0; b < 512; ++b) {
            if (b == 256) reference_release(&voice);
            reference_render(&voice, block);
            for (unsigned i = 0; i < CTL; ++i) {
                int64_t x = block[i];
                int64_t absolute = x < 0 ? -x : x;
                if (absolute > peak) peak = absolute;
                nonzero += x != 0;
                uint32_t bits = (uint32_t)block[i];
                for (unsigned j = 0; j < 4; ++j) {
                    hash ^= (bits >> (8 * j)) & 255;
                    hash *= UINT64_C(1099511628211);
                    if (fputc((int)((bits >> (8 * j)) & 255), file) == EOF) return 3;
                }
            }
        }
        printf("%s{\"algorithm\":%u,\"samples\":16384,\"nonzero\":%u,"
               "\"peak_q24\":%" PRId64 ",\"fnv1a64_q24_le\":\"%016" PRIx64 "\"}",
               algorithm ? "," : "", algorithm + 1, nonzero, peak, hash);
        if (!nonzero) return 4;
    }
    printf("]}\n");
    return fclose(file) ? 3 : 0;
}
#endif
