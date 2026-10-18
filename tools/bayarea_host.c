/* Host render harness for BAYAREA (firmware/rhythm_fm/bayarea.c), at 48 kHz.
 *   bayarea_host <dir> [seeds]
 * renders seeds sounds of each SteamRoller family (clunk .. gnarly, and the
 * kits' blip) as <dir>/<family>_<n>.wav, as the generator strikes them
 * (ba_strike: pitch, hold and release included): 16-bit mono 48 kHz, ended
 * as the generator ends it (32 blocks below -60 dB of its peak), at most
 * 2 s (the generator's cap, without its fade).
 * Each sound is brought to -1 dBFS, as SteamRoller stores one. One line per
 * sound: family, kind, seed, length, the design's peak, decay, attack. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../firmware/rhythm_fm/generated/fm6_reference_tables.h"
#include "../firmware/rhythm_fm/bayarea.c"

#define MAXN (48000 * 2)
static int32_t pcm[MAXN];
static const char *const KIND[BK_N] = {"bongo", "pluck", "chime", "folded", "swell", "drone"};
static const char *const FAMILY[BAF_N] = {"clunk", "scrape", "hiss", "rumble", "whir", "clang", "chop", "gnarly", "blip"};

static void put32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void wav(const char *fn, const int32_t *x, int n)
{
    int32_t pk = 1;
    FILE *f = fopen(fn, "wb");
    if (!f) { perror(fn); exit(1); }
    for (int i = 0; i < n; i++) pk = abs(x[i]) > pk ? abs(x[i]) : pk;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + 2 * n); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put32(f, 1 | 1u << 16); put32(f, 48000); put32(f, 96000); put32(f, 2 | 16u << 16);
    fwrite("data", 1, 4, f); put32(f, 2 * n);
    for (int i = 0; i < n; i++) {
        int32_t s = (int32_t)((int64_t)x[i] * 29204 / pk);   /* -1 dBFS */
        fputc(s, f); fputc(s >> 8, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static BaSlot slot;
    int seeds = argc > 2 ? atoi(argv[2]) : 4;
    if (argc < 2) { fprintf(stderr, "usage: %s <dir> [seeds]\n", argv[0]); return 2; }
    for (int f = 0; f < BAF_N; f++)
        for (int s = 0; s < seeds; s++) {
            uint32_t seed = 0x5EA0u + 977u * (uint32_t)s + 131u * (uint32_t)f;
            int n = 0;
            char fn[512];
            ba_strike(&slot, (unsigned)f, seed);
            int32_t peak = 0, quiet = 0;
            /* the generator's end: 32 blocks below -60 dB of the peak so far */
            while (n + BA_CTL <= MAXN && quiet < 32 && ba_block(&slot, pcm + n)) {
                int32_t bp = 0;
                for (int i = 0; i < BA_CTL; i++) bp = abs(pcm[n + i]) > bp ? abs(pcm[n + i]) : bp;
                peak = bp > peak ? bp : peak;
                quiet = bp * 1000 < peak ? quiet + 1 : 0;
                n += BA_CTL;
            }
            snprintf(fn, sizeof fn, "%s/%s_%d.wav", argv[1], FAMILY[f], s + 1);
            wav(fn, pcm, n);
            printf("%-6s %-6s seed %05x  %4d ms  note %3.0f Hz  peak %5d Hz  decay %4d ms  attack %4d ms\n", FAMILY[f], KIND[slot.d.kind],
                   seed, n / 48, slot.v.f / 256.0, slot.d.peak, slot.d.decay, slot.d.attack);
        }
    return 0;
}
