/* SPDX-License-Identifier: GPL-3.0-only
 * SteamRoller (formerly Rhythm FM), custom Circuit Rhythm firmware: the stock Rhythm
 * sampler and sequencer, plus Elektron-style parameter locks on the stock
 * automation lanes (plock.c), and in the Sample Rec view a sound generator
 * built on Vascular's poundhard drum voice (poundhard.c):
 *   - pad 22 (third row, second from right) toggles generate mode (green):
 *     track button 1-8 draws a new sound of that family and auditions it on
 *     that track's output (1 kick, 2 snare, 3 hi-hat/cymbal, 4 metal,
 *     5 noise, 6 glitch/click, 7 impact, 8 gnarly); Shift + track plays it again;
 *   - pad 21 (third from right) makes a kit page: a tap fills the current
 *     Sample Rec page with 16 sounds planned to complement every generated
 *     sound already in the pack (each stored WAV carries a tag describing
 *     it), so pages expand one kit up to 128 (purple, blinking while it
 *     works); holding it 1.5 s clears the page;
 *   - pad 23 (rightmost) toggles commit mode (red): a track button plays its
 *     sound again; holding it and pressing a slot pad stores the sound: it is
 *     rendered again (bit-exact), written as a 48 kHz mono WAV into that slot
 *     of the current pack through the stock storage task, and the stock
 *     "file changed" event reloads the slot (docs/RHYTHM-SAMPLE-WRITE-MAP.md).
 *     All storage work runs in the main task, never in fm_service (it runs
 *     inside the DSP-SPI mutex).
 * The audition is rendered on the MCU and streamed to DSP core 1, whose
 * consumer adds it to the track's output span after the native sampler pass.
 */
#include <stdint.h>
#include <stddef.h>
/* The sine table poundhard shares with the former FM core runs from ITCM
   (linked at 0x100, stored in flash, copied at boot). */
#ifdef FM_HOST
#define FM6_HOT_DATA
#else
#define FM6_HOT_DATA __attribute__((section(".itcm_rodata")))
extern uint8_t __itcm_load[], __itcm_start[], __itcm_end[];
#endif
#include "generated/fm6_reference_tables.h"
#include "poundhard.h"

#ifdef FM_HOST
extern uint8_t fm_host_ram[0x50000];
extern uint8_t fm_host_itcm[0x4000];
#define RAM_POINTER(address) ((void *)((address)<0x4000u ? fm_host_itcm+(address) : fm_host_ram + ((address)-0x20000000u)))
#else
#define RAM_POINTER(address) ((void *)(address))
#endif
#define MAGIC 0x464d31u
#define ITCM_CODE 0x00000100u            /* sine table; 0x000..0x0ff stays unused (stray NULL reads) */
#ifndef DRUM_POOL
#define DRUM_POOL 0x00001000u            /* poundhard voices: audition, and a second for rendering to a slot */
#endif
#define ARM_STATE 0x00002000u
#define NDRUMV 2u
#define HEAP_LIMIT ((volatile uint32_t *)RAM_POINTER(0x20002b54u))
#define HEAP_CURRENT ((volatile uint32_t *)RAM_POINTER(0x20002b58u))
#define TRACKS 8u
#define FRAMES 32u
/* DSP shared memory: control words, then the audition FIFO. */
#define CTRL 0x37700u
#define CTRL_NONZERO (CTRL+8u)           /* +1 sequence, +2 underruns, +3 drops, +4 status, +5 frames, +6 heartbeats, +7 copies */
#define CTRL_READ_SLOT (CTRL+9u)         /* the DSP's read slot address, advanced with the sequence */
#define CTRL_WORDS 10u
#define BLOCK_WORDS (2u+FRAMES)          /* sequence, FRAMES | track << 6, samples */
#define TRACK_SHIFT 6u
#define SLOT0 0x37720u
#define SLOT_WORDS (1u+BLOCK_WORDS)      /* READY first */
#define SLOT_COUNT 12u
#define SEQ_WRAP (SLOT_COUNT<<19)        /* < 2^23: a positive DSP immediate */
#define SLOT(sequence) (SLOT0+((sequence)%SLOT_COUNT)*SLOT_WORDS)
#define MAX_AHEAD 10u                    /* queued blocks kept ahead: 6.7 ms */
#define TX_WORDS 170u                    /* 510 bytes per DMA: the native TX buffer is 512 */
#define BLOCKS_PER_CALL 4u
#define CALL_BUDGET 48000u               /* ~0.5 ms: past it, stop unless the FIFO is nearly empty */
#define CALL_MIN_AHEAD 4u
#define REPORT_VERSION 23u
#define REPORT_FIELDS 53u
#define REPORT_BYTES (6u+5u*REPORT_FIELDS+1u)
#define MASK24 0xffffffu
#define DRUM_HZ 55.0f                    /* note 60 = A1; the kinds' registers lift it */
#define GEN_SCALE 4194304.0f             /* generator full scale ~1 -> 2^22 (-6 dBFS) */
#define GEN_MAX_BLOCKS 3000u             /* a generated sound lasts at most 2 s... */
#define GEN_FADE_BLOCKS 75u              /* ...fading out over its last 50 ms when cut */
#define GEN_QUIET_BLOCKS 32u             /* ~21 ms below -60 dB of its peak: it has ended */

enum { COMMIT_IDLE, COMMIT_DELETE, COMMIT_WRITE, COMMIT_CLOSE };   /* store-to-slot steps */
/* A generated sound's length rule state (shared by audition, measuring and storing). */
typedef struct { uint32_t block, quiet; float peak; } Gen;
/* A sound waiting on a track to be stored: its seed (re-rendered bit-exactly),
   its length in samples and peak once measured (0: not yet). */
/* kit: 1 = planned kit sound (pad/archetype/flavour/kind below), 0xFF = a family sound.
   pad/archetype/flavour/kind are also the tag written into its WAV (archetype 0xFF: drawn).
   engine: ENGINE_POUNDHARD or ENGINE_BAYAREA (a BAYAREA sound's pad is its kit
   category, its flavour KF_BAYAREA, its kind the BAYAREA family BAF_*). */
typedef struct { uint32_t seed, samples; float peak; uint8_t family, valid, kit, pad, archetype, flavour, kind, engine; } Pending;
typedef struct { uint8_t pad, archetype, flavour, kind; } KitSound;
typedef struct { uint32_t type, index; uint8_t pack, rsv[3]; } StoragePath;   /* stock path {4, slot, pack} */
typedef struct {
    uint32_t magic, next_sequence, primed, blocks, busy, resyncs;
    uint32_t services, ticks, nonzero_blocks;
    uint32_t block_cycles, max_block_cycles;
    uint32_t min_queued, max_service_gap, max_call_cycles, last_service;   /* since the previous report */
    uint32_t render_sum, render_n, transfer_sum, transfer_n, max_transfer, timer_backlog;
    uint32_t dsp_magic, dsp_sequence, dsp_status, dsp_frames, dsp_heartbeats;
    uint32_t dsp_underruns, dsp_drops, dsp_copies, dsp_nonzero_copies, bulk_errors;
    /* generator */
    volatile uint32_t requested[TRACKS];        /* Shift + track button presses (UI task) */
    uint32_t served[TRACKS];                    /* requests turned into sounds (audio service) */
    uint32_t seed[TRACKS], generated, peak;
    Gen audition;
    Pending pending[TRACKS+1u];                 /* [8]: the kit sound being stored */
    /* store-to-slot (commit) */
    StoragePath path;
    uint32_t commit_state, commit_track, commit_slot, commit_total, commit_offset, commit_bytes, commit_sample;
    uint32_t commits, commit_errors, last_commit;
    uint32_t commit_progress;                   /* services count at the last storage step (stall guard) */
    volatile uint32_t replay[TRACKS];           /* play track t's sound again (UI task) */
    uint32_t replayed[TRACKS];
    uint8_t gen_mode, commit_mode, engine, rsv_mode;   /* engine: the one pads 16/17 picked */
    uint32_t flash_until, refusals;             /* pad 23 flashes orange until this service count */
    /* kit (pad 21) */
    uint32_t kit_state, kit_start, kit_next, kit_seed, kits, clears, kit_press_at;
    KitSound plan[16];                          /* the page being made */
    uint32_t kit_known;                         /* tagged sounds found in the pack when it was planned */
    uint32_t kit_focus;                         /* the page's focus categories (bytes; 0xFF none), all 0xFF for a starter page */
    uint8_t kit_held, kit_triggered, kit_engine, rsv_kit;   /* kit_engine: the page's, fixed when it starts */
    uint32_t mix_pos;
    Gen gen_store;
    uint8_t voice_engine[NDRUMV], rsv_voice[2];   /* the engine each generator voice was struck with */
    float gain;
    uint8_t audition_track, last_family, last_pad, last_kind;
    uint32_t pad_events, last_pad_press;
    int32_t render[BLOCK_WORDS];
    float mix[FRAMES];                          /* the audition's block */
    float store_mix[FRAMES];                    /* the store's block: its own, read across storage callbacks while an audition may run */
    uint8_t report[REPORT_BYTES];
    /* parameter locks (plock.c) */
    uint32_t plock_locks, plock_reverts, plock_last, plock_mixer_edits;
    uint32_t plock_mixer_lanes;
    uint32_t plock_early, plock_sweep_at, plock_swept, locks_direct;
    uint16_t plock_preview_base[12];
    /* spice (spice.c) */
    uint32_t spices, spice_last, spice_refusals, spice_sum;
    /* pattern generator (pattern.c) */
    uint32_t patterns, pg_last, pg_refusals;
    uint8_t pg_mode, rsv_pg[3];                 /* 0: the track's own sound; 1: samples from the pack by role */
    uint8_t spice_snap[8][32];                  /* lanes 4-11 at micro-step 0 before the spice (re-roll) */
    /* step mixer locks (steplock.c) */
    uint32_t sl_row, sl_held, sl_writes, sl_clears, sl_last;
    /* Grid FX locks (gridfx.c): owner = track + 1 (0 none), active types, their presets (4 bits each) */
    uint32_t gfx_owner, gfx_active, gfx_preset, gfx_routes, gfx_hits, gfx_claims, gfx_releases;
    /* pointers last: the host harness (64-bit) keeps every offset above */
    void *plock_router;
    void *plock_preview;
    uint8_t *buffer;                            /* the stock 4 KB scratch while storing */
    void *players[TRACKS];                      /* track players, captured as they play (plock_seen) */
    void *transport;                            /* the stock transport object (Record: store [+0x28] index 1) */
    uint8_t *spice_lanes;                       /* the spiced pattern's lanes (re-roll key) */
} State;
#define S ((State *)RAM_POINTER(ARM_STATE))
#define D ((PhVoice *)RAM_POINTER(DRUM_POOL))
_Static_assert(DRUM_POOL + NDRUMV*sizeof(PhVoice) <= ARM_STATE, "poundhard voices overlap the state");
_Static_assert(ARM_STATE + sizeof(State) <= 0x4000, "state exceeds ITCM RAM");
_Static_assert(SLOT0 + SLOT_COUNT*SLOT_WORDS <= 0x38000u, "FIFO exceeds the shared region");
_Static_assert(CTRL + CTRL_WORDS <= SLOT0, "control words overlap the FIFO");

extern void rhythm_boot_original(void);
extern void rhythm_pad_original(void *controller, unsigned pad, unsigned velocity);

#ifndef FM_HOST
static uint32_t irq_save(void) {
    uint32_t p; __asm__ volatile("mrs %0, primask\ncpsid i" : "=r"(p) :: "memory"); return p;
}
static void irq_restore(uint32_t p) { __asm__ volatile("msr primask, %0" :: "r"(p) : "memory"); }
static uint32_t dsp_read(uint32_t address) {
    return ((uint32_t (*)(unsigned,unsigned))0x080314d7u)(0,address) >> 8;
}
static void dsp_write(uint32_t address, uint32_t word) {
    ((void (*)(unsigned,unsigned,unsigned))0x08031229u)(0,word << 8,address);
}
#else
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t p) { (void)p; }
extern uint32_t dsp_read(uint32_t);
extern void dsp_write(uint32_t,uint32_t);
#endif
static unsigned fm_write_chunk(uint32_t address, const int32_t *samples, unsigned count);
/* A block is sent as DMA chunks of at most 170 words through the 512-byte
   native TX buffer. READY is committed by the caller only after every chunk
   completed. */
__attribute__((noinline)) unsigned fm_write_block(uint32_t address, const int32_t *samples, unsigned count) {
    if(!count || count>BLOCK_WORDS) return 0;
    for(unsigned done=0;done<count;) {
        unsigned n=count-done>TX_WORDS ? TX_WORDS : count-done;
        if(!fm_write_chunk(address+done,samples+done,n)) return 0;
        done+=n;
    }
    return 1;
}
static __attribute__((noinline)) unsigned fm_write_chunk(uint32_t address, const int32_t *samples, unsigned count) {
#ifndef FM_HOST
    void *lock=(void *)0x20003688u;
    ((void (*)(void *))0x08029c8du)(lock);
    ((void (*)(void))0x08046415u)();
    ((void (*)(void))0x08046535u)();
    ((void (*)(unsigned,unsigned))0x080313efu)(0,address);
    uint8_t *tx=((uint8_t *(*)(void *))0x0802b685u)((void *)0x20000318u);
    if(tx!=(uint8_t *)0x200058b8u) {
        ((void (*)(void **))0x08030eb9u)(&lock);
        return 0;
    }
    for(unsigned i=0;i<count;i++) {
        uint32_t word=(uint32_t)samples[i];
        tx[3*i]=(uint8_t)(word>>16); tx[3*i+1]=(uint8_t)(word>>8); tx[3*i+2]=(uint8_t)word;
    }
    ((void (*)(unsigned,const void *,unsigned))0x080315e7u)(0,tx,count*3);
    /* Raw transfer does not maintain the single-word writer's address cache. */
    *(volatile uint32_t *)0x20003684u=0xffffu;
    ((void (*)(void **))0x08030eb9u)(&lock);
#else
    for(unsigned i=0;i<count;i++) dsp_write(address+i,(uint32_t)samples[i]&MASK24);
#endif
    return 1;
}

void *memset(void *p, int value, size_t n) {
    unsigned char *q=p; while(n--) *q++=(unsigned char)value; return p;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x=a, *y=b; for(;n--;x++,y++) if(*x!=*y) return *x<*y ? -1 : 1; return 0;
}
void *memcpy(void *p, const void *s, size_t n) {
    unsigned char *q=p; const unsigned char *r=s; while(n--) *q++=*r++; return p;
}
/* The compiler lowers struct copies to these AEABI helpers; no runtime library is linked. */
void __aeabi_memcpy(void *p, const void *s, size_t n) { memcpy(p,s,n); }
void __aeabi_memcpy4(void *p, const void *s, size_t n) { memcpy(p,s,n); }
void __aeabi_memcpy8(void *p, const void *s, size_t n) { memcpy(p,s,n); }
void __aeabi_memclr(void *p, size_t n) { memset(p,0,n); }
void __aeabi_memclr4(void *p, size_t n) { memset(p,0,n); }
void __aeabi_memclr8(void *p, size_t n) { memset(p,0,n); }
#define OUT_TRACKS 0u                    /* plock.c: no track has its macros 1-4 remapped */
#include "poundhard.c"
#include "bayarea.c"
enum { ENGINE_POUNDHARD, ENGINE_BAYAREA };
/* A generator voice holds a poundhard voice or, in the same memory, a BAYAREA one. */
#define BA(i) ((BaSlot *)(void *)&D[i])
_Static_assert(sizeof(BaSlot) <= sizeof(PhVoice), "a BAYAREA voice exceeds a generator voice");
#include "plock.c"
/* Non-audio code (spice, Grid FX locks, UI, report, storing, kits) is built for
   size; the audio paths (generator render, fm_tick, poundhard, plock) stay -O2. */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)
#include "spice.c"
#include "gridfx.c"
#pragma clang attribute pop
static unsigned sl_record(void);           /* steplock.c */
static uint32_t cycles(void) {
#ifdef FM_HOST
    return 0;
#else
    return *(volatile uint32_t *)0xe0001004u;   /* DWT CYCCNT */
#endif
}
static void cycle_counter_enable(void) {
#ifndef FM_HOST
    /* Diagnostic: DEMCR.TRCENA, unlock the Cortex-M7 DWT (LAR), DWT_CTRL.CYCCNTENA. */
    *(volatile uint32_t *)0xe000edfcu |= 1u<<24;
    *(volatile uint32_t *)0xe0001fb0u = 0xc5acce55u;
    *(volatile uint32_t *)0xe0001004u = 0;
    *(volatile uint32_t *)0xe0001000u |= 1u;
#endif
}
/* The M7 instruction cache is off in stock and stays off for stock code: it
   is enabled only while fm_tick renders (the stock firmware has a busy-wait
   DSP handshake whose timing must not change). Invalidated once at boot. */
static void icache_prepare(void) {
#ifndef FM_HOST
    __asm__ volatile("dsb sy\nisb sy" ::: "memory");
    *(volatile uint32_t *)0xe000ef50u=0;          /* ICIALLU */
    __asm__ volatile("dsb sy\nisb sy" ::: "memory");
#endif
}
static void icache_on(void) {
#ifndef FM_HOST
    __asm__ volatile("dsb sy\nisb sy" ::: "memory");
    *(volatile uint32_t *)0xe000ed14u |= 1u<<17;   /* CCR.IC */
    __asm__ volatile("dsb sy\nisb sy" ::: "memory");
#endif
}
static void icache_off(void) {
#ifndef FM_HOST
    __asm__ volatile("dsb sy\nisb sy" ::: "memory");
    *(volatile uint32_t *)0xe000ed14u &= ~(1u<<17);
    __asm__ volatile("dsb sy\nisb sy" ::: "memory");
#endif
}
static int itcm_probe(uint32_t address) {
    volatile uint32_t *probe=(volatile uint32_t *)RAM_POINTER(address);
    uint32_t previous=*probe;
    *probe=0x51a7c39eu;
#ifndef FM_HOST
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    uint32_t readback=*probe;
    *probe=previous;
    return readback==0x51a7c39eu;
}
void fm_boot(void) {
    /* STM32F745/746 ITCM RAM: 16K at 0..0x3fff, unused by stock. The native
       SRAM heap and backup SRAM are untouched. */
#ifndef FM_HOST
    volatile uint32_t *control=(volatile uint32_t *)0xe000ef90u;
    if(((*control>>3)&15u)>=5u) {
        /* Once ITCM is enabled, address zero is RAM rather than the boot
           alias. Keep exception vector reads on the application flash table. */
        *(volatile uint32_t *)0xe000ed08u=0x08010000u;
        *control |= 1u;
        __asm__ volatile("dsb sy\nisb sy" ::: "memory");
#else
    {
#endif
        if(!itcm_probe(ARM_STATE) || !itcm_probe(ITCM_CODE) || !itcm_probe(DRUM_POOL)) { rhythm_boot_original(); return; }
#ifndef FM_HOST
        memcpy(__itcm_start,__itcm_load,(size_t)(__itcm_end-__itcm_start));
        __asm__ volatile("dsb sy\nisb sy" ::: "memory");
#endif
        memset(S,0,sizeof(*S));
        memset(D,0,NDRUMV*sizeof(PhVoice));
        for(unsigned t=0;t<TRACKS;t++) S->seed[t]=0x6000u+t;
        S->last_family=0xffu;
        icache_prepare();
        S->min_queued=0xffffffffu;
        S->magic=MAGIC;
        cycle_counter_enable();
    }
    rhythm_boot_original();
}

#ifndef FM_IDENTITY_BUILD
#define FM_IDENTITY_BUILD 15753
#endif
#define FM_MEMORY_FAILURE_BUILD 15713
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)
void fm_identity_send(void *context, unsigned mask, const void *original) {
    (void)original;
    /* Static payloads survive the native asynchronous transport. Only the
       descriptor is temporary, just as in the original identity handler. */
    static const uint8_t ready[17]={0xf0,0x7e,0,6,2,0,0x20,0x29,1,0x63,0,0,1,1,
        FM_IDENTITY_BUILD>>7,FM_IDENTITY_BUILD&127,0xf7};
    static const uint8_t unavailable[17]={0xf0,0x7e,0,6,2,0,0x20,0x29,1,0x63,0,0,1,1,
        FM_MEMORY_FAILURE_BUILD>>7,FM_MEMORY_FAILURE_BUILD&127,0xf7};
    const uint8_t *payload=unavailable;
#ifdef FM_HOST
    if(S->magic==MAGIC) payload=ready;
#else
    if((*(volatile uint32_t *)0xe000ef90u & 1u) && S->magic==MAGIC) payload=ready;
#endif
    if(payload==ready) {
        uint32_t saved=irq_save();
        /* Version 23 (0.3.0): v22 + fxHits (hits with FX locks | bus claims << 16), fxState (owner track + 1 | active types << 8 | releases << 16). Version 22 (0.2.11): v21 + locksDirect (lock values written straight to the DSP). Version 21 (0.2.10): v20 + locksEarly (next hit's locks sent ahead), locksSwept (locks erased from removed steps). Version 20 (0.2.8): v19 + stepLocks (writes | clears << 16), stepLast (value | lane << 8 | step << 12 | track << 20), stepState (transport found | Record << 1 | row << 4). Version 19 (0.2.7): v18 + spices, spiceLast (track | pattern << 3 | re-roll << 6 | palette << 8 | locks << 16 | period << 24), spiceRefusals | players << 8. Version 18 (0.2.6): v17 + kitKnown (tagged sounds found for the last page), kitFocus. Version 17 (0.2.3): v16 + kits, clears, kitProgress (state | next << 8 | first slot << 16). Version 16: v15 + refusals, modes (generate | commit << 1). v15 = v14 + commits, commitErrors, lastCommit (slot | track << 8 | state << 16 | error << 24). */
        const uint32_t fields[REPORT_FIELDS]={S->services,S->ticks,S->blocks,S->nonzero_blocks,
            S->dsp_magic,S->dsp_sequence,S->dsp_status,S->dsp_frames,S->dsp_heartbeats,
            S->dsp_underruns,S->dsp_drops,S->resyncs,S->next_sequence,S->dsp_copies,S->dsp_nonzero_copies,S->bulk_errors,
            S->generated,(uint32_t)S->last_family|(uint32_t)S->last_pad<<8|(uint32_t)S->last_kind<<16,
            S->last_family<TRACKS ? S->seed[S->last_family] : 0,S->peak,
            S->render_n ? S->render_sum/S->render_n : 0,S->max_block_cycles,S->max_call_cycles,S->max_service_gap,
            S->min_queued,S->timer_backlog,S->transfer_n ? S->transfer_sum/S->transfer_n : 0,
            S->plock_locks,S->plock_reverts,S->plock_last|(S->plock_router ? 1u<<24 : 0),
            S->pad_events,S->last_pad_press,S->commits,S->commit_errors,S->last_commit|S->commit_state<<16,
            S->refusals,(uint32_t)S->gen_mode|(uint32_t)S->commit_mode<<1,
            S->kits,S->clears,S->kit_state|S->kit_next<<8|S->kit_start<<16,
            S->kit_known,S->kit_focus,
            S->spices,S->spice_last,S->spice_refusals|spice_players()<<8,
            S->sl_writes|S->sl_clears<<16,S->sl_last,(S->transport ? 1u:0)|sl_record()<<1|S->sl_row<<4,
            S->plock_early,S->plock_swept,S->locks_direct,
            S->gfx_hits|S->gfx_claims<<16,S->gfx_owner|S->gfx_active<<8|S->gfx_releases<<16};
        /* Stall/queue figures cover the interval since the previous report. */
        S->min_queued=0xffffffffu; S->max_service_gap=0; S->max_call_cycles=0; S->max_block_cycles=0;
        S->render_sum=S->render_n=S->transfer_sum=S->transfer_n=S->max_transfer=S->timer_backlog=0;
        irq_restore(saved);
        const uint8_t header[6]={0xf0,0x7d,0x52,0x46,0x4d,REPORT_VERSION};
        memcpy(S->report,header,sizeof(header));
        for(unsigned i=0;i<REPORT_FIELDS;i++) for(unsigned k=0;k<5;k++)
            S->report[6+5*i+k]=(fields[i]>>(7*k))&127;
        S->report[REPORT_BYTES-1u]=0xf7;
        const void *diagnostic[2]={S->report,(void *)(uintptr_t)sizeof(S->report)};
        /* Report first: the browser closes its identity listener/ports after
           receiving the subsequent standard identity reply. */
        ((void (*)(void *,unsigned,const void *))0x080298b9u)(context,mask,diagnostic);
    }
    const void *descriptor[2]={payload,(void *)(uintptr_t)17};
    ((void (*)(void *,unsigned,const void *))0x080298b9u)(context,mask,descriptor);
}
/* ------------------------------------------------------------ the UI --- */
/* The top-level UI router (stock class vt 0x080672DC): view id = byte 0 of
   the property at +0x14; 4 = the Sample Rec view (object at +0x58, its page
   at +0xC); held buttons (ids 0..27) as bits at +0xA0. */
#ifndef FM_HOST
static unsigned router_view(void *router) {
    return *((uint8_t *(*)(void *,unsigned))0x0803fc59u)(*(void **)((uint8_t *)router+0x14u),0);
}
#else
extern unsigned host_view;
static unsigned router_view(void *router) { (void)router; return host_view; }
#endif
#define VIEW_SAMPLE_REC 4u
static unsigned router_held(void *router) { return *(uint32_t *)((uint8_t *)router+0xa0u); }
/* The track pages object is on screen (the secondary Sample Rec view keeps
   the Sample Rec view id, but shows the track pages, router[+0x3C], in their
   mode 0xA): the current page view router[+0x7C] (or [+0x78]). */
static unsigned track_pages_shown(void *router) {
    void *tp=*(void **)((uint8_t *)router+0x3cu), *cur=*(void **)((uint8_t *)router+0x7cu);
    if(!cur) cur=*(void **)((uint8_t *)router+0x78u);
    return tp && cur==tp;
}
static unsigned in_sample_rec(void *router) { return router_view(router)==VIEW_SAMPLE_REC && !track_pages_shown(router); }
static unsigned sample_rec_page(void *router) { return *(*(uint8_t **)((uint8_t *)router+0x58u)+0xcu); }
/* stock Sample Rec state (view+0x48, byte 0): 2/3 = recording; the recorder
   then holds the scratch and writes its own file */
static unsigned sample_rec_recording(void *router) { unsigned st=*(*(uint8_t **)((uint8_t *)router+0x58u)+0x48u); return st==2u || st==3u; }
#define PAD_POUNDHARD 16u                 /* third row, leftmost: the poundhard engine */
#define PAD_BAYAREA 17u                   /* third row, second from left: the BAYAREA engine */
#define PAD_KIT 21u                       /* third row, third from right */
#define PAD_GEN 22u                       /* third row, second from right */
#define KIT_TRACK TRACKS                  /* pending[] entry of the kit sound being stored */
#define KIT_HOLD 144000000u               /* 1.5 s of 96 MHz cycles: hold the kit pad to clear */
enum { KIT_IDLE, KIT_MAKING, KIT_CLEARING };
#define PAD_COMMIT 23u                    /* third row, rightmost */
#define FLASH_SERVICES 300u               /* ~0.3 s of main-loop services */
static void refuse(void) { S->refusals++; S->flash_until=S->services+FLASH_SERVICES; }
/* Shift + track button (stock discards Shift + tracks 1..8). In the Sample
   Rec view in generate mode: play track t's sound again. Anywhere else, with
   Record on: a whole new pattern for track t (pattern.c); with Record off:
   spice track t's current pattern with parameter locks (spice.c). router 0
   (host and probes): generate. */
unsigned pattern_generate(unsigned t, void *router);
/* The secondary Sample Rec view (Sample Rec pressed again): the track pages
   (router[+0x3C]) on screen in their mode 0xA (byte +0x264, set by
   0x0804D6B6(pages, 0xA) from the router's button 0x12, 0x0803DAF2), whatever
   the view id says. Its bottom row's first two pads choose the pattern
   generator's path. */
#define PAD_PG_ONE 24u
#define PAD_PG_POOL 25u
static unsigned pg_view(void *router) {
    const uint8_t *tv=*(uint8_t **)((uint8_t *)router+0x3cu);
    return track_pages_shown(router) && tv[0x264]==0xau;
}
void fm_randomize_request(unsigned track, void *router) {
    if(track>=TRACKS || S->magic!=MAGIC) return;
    if(!router) { S->requested[track]++; return; }
    if(in_sample_rec(router) && S->gen_mode) { if(S->pending[track].valid) S->replay[track]++; return; }
    if(sl_record()) { pattern_generate(track,router); return; }
    spice_track(track);
}
/* Track button without Shift (stock 0x0803D9A4: select the track and peek at
   its view). In the Sample Rec view with a mode on, it stays here: generate
   mode draws a new sound for the track, commit mode plays its sound again
   (and a pad pressed while it is held stores it). Returns 1 to swallow. */
unsigned fm_track_press(void *router, unsigned id) {
    if(S->magic!=MAGIC || id>=TRACKS || !in_sample_rec(router)) return 0;
    if(S->gen_mode) { S->requested[id]++; return 1; }
    if(S->commit_mode) { if(S->pending[id].valid) S->replay[id]++; return 1; }
    return 0;
}
static unsigned commit_start(unsigned track, unsigned slot);
static unsigned sl_pad(void *router, unsigned pad);
static void sl_release(unsigned pad);
static void commit_guard(void);
static void kit_make(void);
static void kit_clear(void);
/* Controller pad press (stock 0x0803D7FE: router, pad, velocity); runs before
   every stock pad check. In the Sample Rec view: pads 22/23 toggle the modes;
   in commit mode a slot pad pressed while a track with a sound is held
   stores it (consumed only if the store starts). Everything else is stock. */
void fm_pad(void *controller, unsigned pad, unsigned velocity) {
    if(S->magic==MAGIC) {
#ifndef FM_HOST
        void *store=*(void **)((uint8_t *)controller+0x0cu);
        unsigned shift=*((uint8_t *(*)(void *,unsigned))0x0803fc59u)(store,0);
#else
        extern unsigned host_shift; unsigned shift=host_shift;
#endif
        S->pad_events++;
        S->last_pad_press=(pad&0xffu)|(shift ? 0x100u:0)|(S->services<<12);
        if(sl_pad(controller,pad)) return;       /* step mixer locks (steplock.c) */
        if((pad==PAD_PG_ONE || pad==PAD_PG_POOL) && pg_view(controller)) { S->pg_mode=(uint8_t)(pad==PAD_PG_POOL); return; }
        if(in_sample_rec(controller)) {
            commit_guard();
            if(pad==PAD_KIT) {
                /* momentary: a tap makes a kit, a 1.5 s hold clears it (see
                   fm_pad_release and fm_samplerec_frame) */
                if(S->kit_state==KIT_IDLE && S->commit_state==COMMIT_IDLE && !sample_rec_recording(controller)) {
                    unsigned page=sample_rec_page(controller);
                    S->kit_start=16u*(page&7u);
                    S->kit_held=1; S->kit_triggered=0; S->kit_press_at=cycles();
                } else refuse();
                return;
            }
            /* the engine pads, while a mode is on (otherwise pads 16/17 are stock's threshold recording) */
            if((pad==PAD_POUNDHARD || pad==PAD_BAYAREA) && (S->gen_mode || S->commit_mode)) {
                S->engine=(uint8_t)(pad==PAD_BAYAREA ? ENGINE_BAYAREA : ENGINE_POUNDHARD);
                return;
            }
            if(pad==PAD_GEN) { S->gen_mode=(uint8_t)!S->gen_mode; S->commit_mode=0; return; }
            if(pad==PAD_COMMIT) { S->commit_mode=(uint8_t)!S->commit_mode; S->gen_mode=0; return; }
            if(pad<16u && S->commit_mode) {
                unsigned held=router_held(controller);
                for(unsigned t=0;t<TRACKS;t++) if(held>>t&1u) {
                    if(S->pending[t].valid && !sample_rec_recording(controller) && commit_start(t,pad+16u*sample_rec_page(controller))) return;
                    refuse();
                    break;
                }
            }
        }
    }
    rhythm_pad_original(controller,pad,velocity);
}
/* Router pad release (vtable word 0x080672E8, stock 0x0803D896). A short
   press of the kit pad makes a kit. Pads we consumed never set the stock
   held-pad bit, so the stock release does nothing for them. */
void fm_pad_release(void *router, unsigned pad, unsigned velocity) {
    if(S->magic==MAGIC) sl_release(pad);
    if(S->magic==MAGIC && pad==PAD_KIT && S->kit_held) {
        S->kit_held=0;
        if(!S->kit_triggered) kit_make();
    }
#ifndef FM_HOST
    ((void (*)(void *,unsigned,unsigned))0x0803d897u)(router,pad,velocity);
#else
    (void)router; (void)velocity;
#endif
}
/* The Sample Rec view's frame (its vt+0x1C, stock 0x0804FE9A, vtable word
   0x08067A24): stock composes pads 16-31 every LED refresh; pads 22/23 then
   show the modes (green generate, red commit; yellow while storing, orange
   briefly when a store was refused or failed), and while a mode is on pads
   16/17 the engine (bright: the one chosen). */
#define LED_GREEN 0x22u
#define LED_RED 0x55u
#define LED_YELLOW 0x6du
#define LED_ORANGE 0x2bu
#define LED_DIM_GREEN 0x24u                 /* 0, 41, 0 */
#define LED_DIM_RED 0x54u                   /* 40, 0, 0 */
#define LED_PURPLE 0x37u                    /* 198, 58, 198 */
#define LED_DIM_PURPLE 0x38u                /* 31, 9, 31 */
#define LED_DIM_YELLOW 0x73u                /* stock "toggle off" */
#define LED_CYAN 0x18u
#define LED_DIM_CYAN 0x19u
uint16_t *fm_samplerec_frame(void *view) {
#ifndef FM_HOST
    uint16_t *f=((uint16_t *(*)(void *))0x0804fe9bu)(view);
#else
    extern uint16_t *host_frame(void *); uint16_t *f=host_frame(view);
#endif
    if(S->magic!=MAGIC || !f) return f;
    /* the frame is pulled on every LED refresh, in the main task outside the
       DSP mutex: also where a held kit pad turns into a clear */
    commit_guard();
    if(S->kit_held && !S->kit_triggered && cycles()-S->kit_press_at>KIT_HOLD) { S->kit_triggered=1; kit_clear(); }
    unsigned blink=cycles()>>25&1u;             /* ~0.35 s */
    f[PAD_KIT]=S->kit_state ? (blink ? LED_PURPLE : LED_DIM_PURPLE) : S->kit_held ? LED_PURPLE : LED_DIM_PURPLE;
    f[PAD_GEN]=S->gen_mode ? LED_GREEN : LED_DIM_GREEN;
    if(S->gen_mode || S->commit_mode) {           /* the engines: yellow poundhard, cyan BAYAREA */
        f[PAD_POUNDHARD]=S->engine==ENGINE_POUNDHARD ? LED_YELLOW : LED_DIM_YELLOW;
        f[PAD_BAYAREA]=S->engine==ENGINE_BAYAREA ? LED_CYAN : LED_DIM_CYAN;
    }
    if((int32_t)(S->flash_until-S->services)>0) f[PAD_COMMIT]=LED_ORANGE;
    else if(S->commit_state && S->commit_track!=KIT_TRACK) f[PAD_COMMIT]=LED_YELLOW;
    else f[PAD_COMMIT]=S->commit_mode ? LED_RED : LED_DIM_RED;
    return f;
}
#include "steplock.c"
/* A lock's value straight into the DSP's track parameter block. The stock
   setters (knob router [+8], 0x0803898C -> per-track sound params) only
   cache a value and mark it dirty; the main loop's drain (0x0803081C) writes
   it to Y:$307C8 + 32 t + param later, while a note trigger is written to
   the DSP at once from the sequencer. A lock sent with its note therefore
   landed after the note's transient (hardware 0.2.10: a hard-right kick
   still clicked on the left; a Mixer pan, set long before, did not). Here
   the value is written at once with the drain's own writer 0x0804070E
   (track, param, value) -> locked DSP write 0x08031228, the same write the
   note trigger uses from the sequencer. Param = setter table 0x08063708
   [id] (u8 0x10 + track, u8, u16 param); sound params 6..31 only. */
void lock_dsp_now(unsigned id, unsigned value) {
    if(S->magic!=MAGIC || id>0xa0u) return;
    const uint8_t *e=(const uint8_t *)0x08063708u+4u*id;
#ifdef FM_HOST
    (void)e; (void)value;
#else
    const unsigned param=e[2]|(unsigned)e[3]<<8;
    if(param<6u || param>=0x20u || (e[0]&0xf8u)!=0x10u) return;
    ((void (*)(unsigned,unsigned,unsigned))0x0804070fu)(e[0]&7u,param,value);
    S->locks_direct++;
#endif
}
static uint32_t entropy(void) {
#ifdef FM_HOST
    return 0;
#else
    return *(volatile uint32_t *)0xe000e018u;   /* SysTick current value */
#endif
}
#pragma clang attribute pop
/* ------------------------------------------------------ the generator --- */
/* A planned kit sound's patch: the planned kind, character archetype and
   flavour, the rest drawn from its seed. */
enum { KF_NONE, KF_IMPACT, KF_CYMBAL, KF_DESTROY, KF_BAYAREA };
/* Kit categories, shared by both engines (kits below). poundhard plays the
   first 13; BAYAREA plays impact (clunk), metal (clang), gnarly metal (chop),
   click (blip) and destroyed (gnarly) in its own way, never kicks, and the
   last four, its textures, only it plays. */
enum { C_KICK, C_KICK_G, C_SNARE, C_SNARE_G, C_CLAP, C_IMPACT, C_HAT, C_CYMBAL, C_METAL, C_METAL_G, C_NOISE, C_CLICK, C_DESTROY,
       C_SCRAPE, C_RUMBLE, C_WHIR, C_HISS, C_COUNT };
#define C_POUNDHARD 13u
/* each BAYAREA family's kit category, and back */
static const uint8_t baf_category[BAF_N]={ C_IMPACT, C_SCRAPE, C_HISS, C_RUMBLE, C_WHIR, C_METAL, C_METAL_G, C_DESTROY, C_CLICK };
static __attribute__((noinline)) unsigned kit_patch(PhPatch *p, const Pending *q) {
    PhRng rr, *r=&rr;
    ph_seed(r,(uint64_t)q->seed ^ 0x4B17ull);
    ph_design(p,q->kind,q->seed);
    ph_character_a(p,q->pad,q->seed,q->archetype==0xffu ? -1 : (int)q->archetype);
    if(q->flavour==KF_IMPACT) {
        p->overdrive=ph_range(r,70,127); p->odPost=(uint8_t)ph_chance(r,0.5f);
        if(p->foldAmt<40) p->foldAmt=ph_range(r,40,110);
        if(p->decay<40) p->decay=ph_range(r,40,80);
        p->volume=100;
    } else if(q->flavour==KF_CYMBAL) {
        p->decay=ph_range(r,70,100); p->noiseDecay=ph_range(r,65,90);
        p->aHold=ph_range(r,70,100); p->aDec=ph_range(r,95,115);
    } else if(q->flavour==KF_DESTROY) {
        static const uint32_t extra[6]={ T_CRUSH, T_DECIM, T_CHAOS, T_FOLD|T_DRIVE, T_COMB, T_RING };
        ph_treat(p,r,extra[ph_pick(r,6)]|extra[ph_pick(r,6)],q->pad&7u);
    }
    return q->pad;
}
/* Strike generator voice vi with track t's pending sound (its seed decides everything). */
static void gen_strike(unsigned vi, Gen *g, unsigned track) {
    Pending *q=&S->pending[track];
    g->block=0; g->quiet=0; g->peak=0;
    if(q->engine==ENGINE_BAYAREA) {
        unsigned baf=q->kit!=0xffu ? q->kind : q->family;
        baf=baf<BAF_N ? baf : BAF_CLUNK;
        ba_strike(BA(vi),baf,q->seed);
        q->pad=baf_category[baf]; q->kind=(uint8_t)baf;
        S->last_family=q->family; S->last_pad=q->pad; S->last_kind=q->kind;
        return;
    }
    PhPatch patch;
    PhVoice *v=&D[vi];
    unsigned pad=q->kit!=0xffu ? kit_patch(&patch,q) : ph_family(&patch,q->family,q->seed);
    q->pad=(uint8_t)pad; q->kind=patch.kind;
    S->last_family=q->family; S->last_pad=(uint8_t)pad; S->last_kind=patch.kind;
    v->active=0;        /* starts from silence */
    /* poundhard keeps its noise generator across strikes: seed it from the
       sound, so every rendering (audition, replay, the stored copy) is the same */
    v->noise=q->seed*2654435761u|1u;
    ph_strike(v,&patch,DRUM_HZ,1.0f);
}
/* Whether generator voice vi still sounds (the engine of the sound it was struck with). */
static unsigned gen_active(unsigned vi) {
    return S->voice_engine[vi]==ENGINE_BAYAREA ? BA(vi)->v.live!=0 : D[vi].active!=0;
}
static void gen_stop(unsigned vi) {
    if(S->voice_engine[vi]==ENGINE_BAYAREA) BA(vi)->v.live=0; else D[vi].active=0;
}
/* One 32-sample block of a generated sound into out: until it falls 60 dB
   below its peak, at most 2 s with a 50 ms fade. The voice ends with it. */
static __attribute__((noinline)) void gen_render(unsigned vi, Gen *g, float *out) {
    if(S->voice_engine[vi]==ENGINE_BAYAREA) {
        int32_t x[FRAMES];
        if(!ba_block(BA(vi),x)) for(unsigned i=0;i<FRAMES;i++) x[i]=0;
        for(unsigned i=0;i<FRAMES;i++) out[i]=(float)x[i]*(1.0f/32768.0f);
    } else {
        for(unsigned i=0;i<FRAMES;i++) out[i]=0;
        ph_render(&D[vi],out,FRAMES);
    }
    float block_peak=0;
    for(unsigned i=0;i<FRAMES;i++) { float a=out[i]<0 ? -out[i] : out[i]; if(a>block_peak) block_peak=a; }
    if(block_peak>g->peak) g->peak=block_peak;
    g->quiet=block_peak<g->peak*0.001f ? g->quiet+1u : 0;
    if(g->block+GEN_FADE_BLOCKS>=GEN_MAX_BLOCKS) {
        float gain=(float)(GEN_MAX_BLOCKS-g->block)/(float)GEN_FADE_BLOCKS, step=-1.0f/(float)(GEN_FADE_BLOCKS*FRAMES);
        for(unsigned i=0;i<FRAMES;i++) { out[i]*=gain; gain+=step; }
    }
    if(++g->block>=GEN_MAX_BLOCKS || g->quiet>=GEN_QUIET_BLOCKS) gen_stop(vi);
}
/* Strike, noting the voice's engine. */
static void gen_start(unsigned vi, Gen *g, unsigned track) {
    S->voice_engine[vi]=S->pending[track].engine;
    gen_strike(vi,g,track);
}
/* A new sound of family f, waiting on track f, auditioned there at once. */
static __attribute__((noinline)) void generate(unsigned family) {
    uint32_t seed=(S->seed[family]*2654435761u)^entropy()^(S->ticks<<8)^(family<<28)^S->generated;
    S->seed[family]=seed;
    Pending *p=&S->pending[family];
    p->seed=seed; p->family=(uint8_t)family; p->samples=0; p->peak=0; p->valid=1; p->kit=0xffu;
    p->archetype=0xffu; p->flavour=family==PH_FAM_IMPACT ? KF_IMPACT : family==PH_FAM_GNARLY ? KF_DESTROY : KF_NONE;
    p->engine=S->engine;
    if(p->engine==ENGINE_BAYAREA) p->flavour=KF_BAYAREA;
    S->audition_track=(uint8_t)family;
    S->generated++;
    gen_start(0,&S->audition,family);
}
/* One 32-sample audition block into S->render[2..]; 0 once the voice ended.
   An audition that runs to its end measures the sound for storing. */
static __attribute__((noinline)) unsigned render_block(void) {
    for(unsigned track=0;track<TRACKS;track++) {
        if(S->served[track]!=S->requested[track]) { S->served[track]=S->requested[track]; S->replayed[track]=S->replay[track]; generate(track); }
        else if(S->replayed[track]!=S->replay[track]) {
            /* the same sound again (same seed: bit-exact) */
            S->replayed[track]=S->replay[track];
            S->audition_track=(uint8_t)track;
            gen_start(0,&S->audition,track);
        }
    }
    if(!gen_active(0)) return 0;
    gen_render(0,&S->audition,S->mix);
    if(!gen_active(0)) {
        Pending *p=&S->pending[S->audition_track];
        if(p->valid && !p->samples) { p->samples=S->audition.block*FRAMES; p->peak=S->audition.peak; }
    }
    uint32_t peak=S->peak;
    for(unsigned i=0;i<FRAMES;i++) {
        float y=S->mix[i]*GEN_SCALE;
        int32_t x=y>8388607.0f ? 0x7fffff : y< -8388608.0f ? -0x800000 : (int32_t)y;
        uint32_t m=(uint32_t)(x<0 ? -x : x); if(m>peak) peak=m;
        S->render[2+i]=x;
    }
    S->peak=peak;
    return 1;
}
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)
/* ------------------------------------------------- storing to a slot --- */
/* Stock storage task (asynchronous with a callback; callbacks run in the
   main task outside the DSP mutex) and helpers, docs/RHYTHM-SAMPLE-WRITE-MAP.md. */
#define WAV_HEADER 64u                   /* fmt chunk of 36 bytes: 16 PCM + cbSize 18 + our 18-byte tag */
#define CHUNK 4096u
#define TAIL 256u                         /* the loader never plays a slot's last 256 samples */
typedef void (*storage_cb)(const StoragePath *path, unsigned kind, unsigned result, uint32_t a, uint32_t b, void *ctx);
#ifndef FM_HOST
#define STORAGE_DELETE(path,cb) ((unsigned (*)(const StoragePath *,storage_cb,void *))0x08046099u)(path,cb,0)
#define STORAGE_WRITE(path,off,buf,len,cb) ((unsigned (*)(const StoragePath *,uint32_t,const void *,uint32_t,storage_cb,void *))0x0804607bu)(path,off,buf,len,cb,0)
#define STORAGE_CLOSE(path,cb) ((unsigned (*)(const StoragePath *,storage_cb,void *))0x080460c9u)(path,cb,0)
#define SCRATCH_ACQUIRE() ((uint8_t *(*)(unsigned))0x0802ef6bu)(0x47454e)
#define SCRATCH_RELEASE() ((void (*)(void))0x0802ef99u)()
#define CURRENT_PACK() (**(volatile uint8_t **)0x20006e24u)
#define FILE_CHANGED(path) ((void (*)(void *,const StoragePath *))0x08035d63u)(*(void **)0x20006eacu,path)
#define AUDITION_SLOT(slot) ((void (*)(unsigned,unsigned,unsigned))0x080407f1u)(slot,0,0)
#else
extern unsigned host_storage_delete(const StoragePath *,storage_cb);
extern unsigned host_storage_write(const StoragePath *,uint32_t,const void *,uint32_t,storage_cb);
extern unsigned host_storage_close(const StoragePath *,storage_cb);
extern uint8_t *host_scratch_acquire(void);
extern void host_scratch_release(void);
extern void host_file_changed(const StoragePath *);
extern void host_audition_slot(unsigned);
#define STORAGE_DELETE(path,cb) host_storage_delete(path,cb)
#define STORAGE_WRITE(path,off,buf,len,cb) host_storage_write(path,off,buf,len,cb)
#define STORAGE_CLOSE(path,cb) host_storage_close(path,cb)
#define SCRATCH_ACQUIRE() host_scratch_acquire()
#define SCRATCH_RELEASE() host_scratch_release()
#define CURRENT_PACK() 0u
#define FILE_CHANGED(path) host_file_changed(path)
#define AUDITION_SLOT(slot) host_audition_slot(slot)
#endif
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put16(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
/* The next stored sample, as int16: the sound re-rendered on voice 1,
   normalised to -1 dBFS, then the zero tail. */
static int16_t commit_sample(void) {
    unsigned track=S->commit_track;
    int32_t value=0;
    if(S->commit_sample<S->pending[track].samples) {
        if(S->mix_pos>=FRAMES) { gen_render(1,&S->gen_store,S->store_mix); S->mix_pos=0; }
        float y=S->store_mix[S->mix_pos++]*S->gain*32767.0f;
        value=y>32767.0f ? 32767 : y< -32768.0f ? -32768 : (int32_t)(y<0 ? y-0.5f : y+0.5f);
    }
    S->commit_sample++;
    return (int16_t)value;
}
static void commit_fail(unsigned code) {
    if(S->buffer) { SCRATCH_RELEASE(); S->buffer=0; }
    S->commit_state=COMMIT_IDLE; S->commit_errors++;
    S->kit_state=KIT_IDLE;                      /* a kit or a clear stops at the first error */
    S->last_commit=code<<24 | S->commit_slot;
    refuse();
}
/* A store whose storage callback never came (~30 s of main-loop services)
   is abandoned, so it can never block later stores. */
#define COMMIT_STALL 30000u
static void commit_guard(void) {
    if((S->commit_state!=COMMIT_IDLE || S->kit_state==KIT_CLEARING) && S->services-S->commit_progress>COMMIT_STALL) commit_fail(4);
}
static void commit_written(const StoragePath *path, unsigned kind, unsigned result, uint32_t a, uint32_t b, void *ctx);
static void commit_closed(const StoragePath *path, unsigned kind, unsigned result, uint32_t a, uint32_t b, void *ctx);
/* The next 4 KB (the first one starts with the WAV header). */
static __attribute__((noinline)) void commit_write_next(void) {
    uint8_t *buf=S->buffer;
    uint32_t off=S->commit_offset, len=S->commit_total-off;
    if(len>CHUNK) len=CHUNK;
    unsigned at=0;
    if(off==0) {
        /* The loader needs "data" right after "fmt " within its 64-byte read
           (docs/RHYTHM-SAMPLE-WRITE-MAP.md 1.5): a 36-byte fmt chunk carries
           the tag kit expansion reads back (kit_tags). */
        const Pending *q=&S->pending[S->commit_track];
        uint32_t data=S->commit_total-WAV_HEADER;
        for(unsigned i=0;i<WAV_HEADER;i++) buf[i]=0;
        memcpy(buf,"RIFF",4); put32(buf+4,WAV_HEADER-8u+data); memcpy(buf+8,"WAVEfmt ",8);
        put32(buf+16,36); put16(buf+20,1); put16(buf+22,1); put32(buf+24,48000); put32(buf+28,96000);
        put16(buf+32,2); put16(buf+34,16); put16(buf+36,18);
        memcpy(buf+38,"PHK1",4); buf[42]=q->pad; buf[43]=q->archetype; buf[44]=q->flavour; buf[45]=q->kind;
        buf[46]=q->family; buf[47]=(uint8_t)(1u+q->engine); put32(buf+48,q->seed);   /* 47: 1 poundhard, 2 BAYAREA */
        memcpy(buf+56,"data",4); put32(buf+60,data);
        at=WAV_HEADER;
    }
    icache_on();
    for(;at<len;at+=2) put16(buf+at,(uint16_t)commit_sample());
    icache_off();
    S->commit_bytes=len;
    S->commit_progress=S->services;
    STORAGE_WRITE(&S->path,off,buf,len,commit_written);
}
static void commit_deleted(const StoragePath *path, unsigned kind, unsigned result, uint32_t a, uint32_t b, void *ctx) {
    (void)path; (void)kind; (void)a; (void)b; (void)ctx;
    if(S->commit_state!=COMMIT_DELETE) return;
    if(result!=0 && result!=5) { commit_fail(1); return; }     /* 5: there was no file */
    S->commit_state=COMMIT_WRITE;
    commit_write_next();
}
static void commit_written(const StoragePath *path, unsigned kind, unsigned result, uint32_t a, uint32_t b, void *ctx) {
    (void)path; (void)kind; (void)a; (void)ctx;
    if(S->commit_state!=COMMIT_WRITE) return;
    if(result!=0 || b!=S->commit_bytes) { commit_fail(2); return; }
    S->commit_offset+=S->commit_bytes;
    if(S->commit_offset<S->commit_total) { commit_write_next(); return; }
    SCRATCH_RELEASE(); S->buffer=0;
    S->commit_state=COMMIT_CLOSE; S->commit_progress=S->services;
    STORAGE_CLOSE(&S->path,commit_closed);
}
static void commit_closed(const StoragePath *path, unsigned kind, unsigned result, uint32_t a, uint32_t b, void *ctx) {
    (void)path; (void)kind; (void)a; (void)b; (void)ctx;
    if(S->commit_state!=COMMIT_CLOSE) return;
    if(result!=0) { commit_fail(3); return; }
    /* The stock "file changed" event: rescans the slot, frees its DSP pages
       and reloads it (as after a Components upload); then play it once. */
    StoragePath changed=S->path;
    changed.pack=CURRENT_PACK();
    FILE_CHANGED(&changed);
    S->commits++; S->last_commit=S->commit_slot|(S->commit_track<<8);
    S->commit_state=COMMIT_IDLE;
    if(S->commit_track==KIT_TRACK) { if(S->kit_state==KIT_MAKING) { S->kit_next++; kit_make(); } }
    else AUDITION_SLOT(S->commit_slot);
}
/* ---------------------------------------------------------- kits --- */
/* A kit grows page by page (16 sounds each, up to 128). Every page is
   planned against all generated sounds already in the pack, read back from
   the tags in their WAV headers: categories keep a target balance for the
   whole kit, and within a category the least used archetypes and kinds come
   first, so each page extends the ones before it.
   A page is made by one engine (the one chosen when it starts) from the
   categories that engine plays, against the census of every sound in the
   pack, whichever engine made it: a BAYAREA page after a poundhard one goes
   where the kit is still thin, and the next poundhard page counts both. */
#define KIT_PAGE 16u
/* share of each category in a whole kit (rows group by this order); poundhard's
   13 sum to 64, as before BAYAREA */
static const uint8_t kit_weight[C_COUNT]={ 8, 8, 6, 4, 3, 5, 6, 2, 5, 3, 5, 5, 4, 5, 4, 4, 4 };
/* the categories each engine plays (bits) */
#define KIT_CATS_POUNDHARD ((1u<<C_POUNDHARD)-1u)
#define KIT_CATS_BAYAREA (1u<<C_IMPACT|1u<<C_METAL|1u<<C_METAL_G|1u<<C_CLICK|1u<<C_DESTROY|1u<<C_SCRAPE|1u<<C_RUMBLE|1u<<C_WHIR|1u<<C_HISS)
/* each BAYAREA category's family (the inverse of baf_category; b unused) */
static unsigned category_baf(unsigned c, unsigned b) {
    (void)b;
    for(unsigned f=0;f<BAF_N;f++) if(baf_category[f]==c) return f;
    return BAF_GNARLY;
}
typedef struct { uint8_t cat[C_COUNT], arche[C_COUNT][8], kind[PH_KINDS], role_g[8]; } KitCensus;
static unsigned kit_category(unsigned pad, unsigned flavour) {
    unsigned role=pad&7u, g=pad>=PH_GNARLY;
    if(flavour==KF_BAYAREA) return pad<C_COUNT ? pad : C_DESTROY;
    if(flavour==KF_DESTROY) return C_DESTROY;
    if(flavour==KF_IMPACT || role==PH_ROLE_TOM) return C_IMPACT;
    if(flavour==KF_CYMBAL) return C_CYMBAL;
    switch(role) {
    case PH_ROLE_KICK: return g ? C_KICK_G : C_KICK;
    case PH_ROLE_SNARE: return g ? C_SNARE_G : C_SNARE;
    case PH_ROLE_CLAP: return C_CLAP;
    case PH_ROLE_HAT: return C_HAT;
    case PH_ROLE_METAL: return g ? C_METAL_G : C_METAL;
    case PH_ROLE_NOISE: return C_NOISE;
    default: return C_CLICK;
    }
}
static void kit_count(KitCensus *c, const KitSound *k) {
    unsigned cat=kit_category(k->pad,k->flavour);
    if(c->cat[cat]<255u) c->cat[cat]++;
    if(k->flavour==KF_BAYAREA) return;          /* archetypes, kinds and gnarly roles are poundhard's */
    if(k->archetype<8u && c->arche[cat][k->archetype]<255u) c->arche[cat][k->archetype]++;
    if(k->kind<PH_KINDS && c->kind[k->kind]<255u) c->kind[k->kind]++;
    if(k->pad>=PH_GNARLY && c->role_g[k->pad&7u]<255u) c->role_g[k->pad&7u]++;
}
#ifndef FM_HOST
#define STORAGE_READ_SYNC(path,buf,len) ((unsigned (*)(const StoragePath *,uint32_t,void *,uint32_t,storage_cb,void *))0x0804603bu)(path,0,buf,len,0,0)
#else
extern unsigned host_storage_read(const StoragePath *,void *,uint32_t);
#define STORAGE_READ_SYNC(path,buf,len) host_storage_read(path,buf,len)
#endif
/* Read the tags of every slot outside the page being made (synchronous
   reads of 64 bytes, in the main task, through the stock scratch). */
/* (cat, if given: each slot's category; 0xFE a sample without a tag, 0xFF
   none. skip: the first slot of a page left out, 128 for none.) */
static __attribute__((noinline)) unsigned kit_tags(KitCensus *c, unsigned skip, uint8_t *cat) {
    uint8_t *buf=SCRATCH_ACQUIRE();
    S->kit_known=0;
    if(cat) memset(cat,0xff,128);
    if(!buf) return 0;
    StoragePath path={4,0,0xfe,{0,0,0}};
    for(unsigned slot=0;slot<128u;slot++) {
        if(slot>=skip && slot<skip+KIT_PAGE) continue;
        path.index=slot;
        for(unsigned i=0;i<WAV_HEADER;i++) buf[i]=0;
        if(STORAGE_READ_SYNC(&path,buf,WAV_HEADER)!=0) continue;
        if(cat) cat[slot]=0xfe;
        if(memcmp(buf,"RIFF",4) || memcmp(buf+8,"WAVEfmt ",8) || buf[16]!=36u || memcmp(buf+38,"PHK1",4)) continue;
        KitSound k={buf[42],buf[43],buf[44],buf[45]};
        kit_count(c,&k);
        if(cat) cat[slot]=(uint8_t)kit_category(k.pad,k.flavour);
        S->kit_known++;
    }
    SCRATCH_RELEASE();
    return 1;
}
/* The least used option; ties go to the option the kit's random walk
   reaches first (so equal counts do not always pick the same one). */
static unsigned kit_least(const uint8_t *count, const uint8_t *options, unsigned n, uint32_t *rnd) {
    unsigned start=(*rnd>>16)%n, best=options[start];
    for(unsigned i=1;i<n;i++) {
        unsigned o=options[(start+i)%n];
        if(count[o]<count[best]) best=o;
    }
    *rnd=*rnd*1103515245u+12345u;
    return best;
}
/* Plan the 16 sounds of the page from the census of the pack.
   - The first page (no tagged sound yet) is a balanced starter kit.
   - An expansion has a focus: three or four categories drawn among those
     furthest below their share of the grown kit (the further behind, the
     likelier), which get 11-12 slots; the rest are a few "spice" sounds
     from other categories still at or below their share. The kit converges
     to the weights over pages, but no two pages are laid out alike, and
     the page starts with its lead category. */
static __attribute__((noinline)) void kit_plan(KitCensus *c) {
    uint32_t rnd=S->kit_seed|1u;
    unsigned total=KIT_PAGE, picked[KIT_PAGE], n=0, order[C_COUNT];
    const unsigned ba=S->kit_engine==ENGINE_BAYAREA, plays=ba ? KIT_CATS_BAYAREA : KIT_CATS_POUNDHARD;
    /* shares within the engine's categories: weight / scale of the sounds in
       them (this page's included), whichever engine made those sounds; a
       poundhard-only kit balances exactly as before BAYAREA */
    int32_t scale=0;
    uint8_t cat[C_COUNT];
    int32_t want[C_COUNT];
    for(unsigned i=0;i<C_COUNT;i++) { cat[i]=c->cat[i]; order[i]=i; if(plays>>i&1u) { scale+=kit_weight[i]; total+=c->cat[i]; } }
    #define PLAYS(i) (plays>>(i)&1u)
    #define DEFICIT(i) ((int32_t)(kit_weight[i]*total)-scale*(int32_t)cat[i])
    if(!S->kit_known) {
        for(;n<KIT_PAGE;n++) {
            int best=-1; int32_t most=-0x7fffffff;
            for(unsigned i=0;i<C_COUNT;i++) {
                if(!PLAYS(i)) continue;
                int32_t w=DEFICIT(i)*16+(int32_t)((rnd>>(i&15))&15u);
                if(w>most) { most=w; best=(int)i; }
            }
            picked[n]=(unsigned)best; cat[best]++;
            rnd=rnd*1103515245u+12345u;
        }
        S->kit_focus=0xffffffffu;
    } else {
        /* the focus: draw 3 or 4 categories, weighted by how far behind they are */
        unsigned focus[4], nf=3u+(rnd>>30&1u);
        for(unsigned f=0;f<nf;f++) {
            int32_t sum=0;
            for(unsigned i=0;i<C_COUNT;i++) {
                unsigned used=0; for(unsigned g=0;g<f;g++) used|=focus[g]==i;
                want[i]=used || !PLAYS(i) ? 0 : DEFICIT(i)>0 ? DEFICIT(i)+64 : 16;   /* even caught-up categories keep a small chance */
                sum+=want[i];
            }
            rnd=rnd*1103515245u+12345u;
            int32_t at=(int32_t)((rnd>>8)%(uint32_t)sum);
            unsigned i=0;
            while(i<C_COUNT-1u && at>=want[i]) { at-=want[i]; i++; }
            focus[f]=i;
        }
        /* 11 or 12 slots over the focus, each at least 2, the rest by deficit */
        unsigned core=11u+(rnd>>29&1u);
        for(unsigned f=0;f<nf;f++) { picked[n++]=focus[f]; picked[n++]=focus[f]; cat[focus[f]]+=2; }
        while(n<core) {
            unsigned best=focus[0]; int32_t most=-0x7fffffff;
            for(unsigned f=0;f<nf;f++) { int32_t w=DEFICIT(focus[f])*16+(int32_t)((rnd>>(4*f))&15u); if(w>most) { most=w; best=focus[f]; } }
            picked[n++]=best; cat[best]++;
            rnd=rnd*1103515245u+12345u;
        }
        /* spice: the remaining slots from categories not in the focus, still at or below their share */
        while(n<KIT_PAGE) {
            unsigned tries=0, i;
            do {
                rnd=rnd*1103515245u+12345u; i=(rnd>>12)%C_COUNT;
                unsigned in=0; for(unsigned f=0;f<nf;f++) in|=focus[f]==i;
                if(!in && PLAYS(i) && (DEFICIT(i)>=0 || tries>24u)) break;
            } while(++tries<64u);
            if(!PLAYS(i)) i=focus[0];                /* (64 misses: never another engine's category) */
            picked[n++]=i; cat[i]++;
        }
        /* the page's row order: its focus first (lead category first), then the spice */
        unsigned k=0;
        for(unsigned f=0;f<nf;f++) order[focus[f]]=k++;
        for(unsigned i=0;i<C_COUNT;i++) { unsigned in=0; for(unsigned f=0;f<nf;f++) in|=focus[f]==i; if(!in) order[i]=k++; }
        S->kit_focus=focus[0]|focus[1]<<8|focus[2]<<16|(nf>3u ? focus[3] : 0xffu)<<24;
    }
    #undef DEFICIT
    #undef PLAYS
    /* rows group by category, in the page's order */
    for(unsigned i=1;i<KIT_PAGE;i++) for(unsigned j=i;j>0 && order[picked[j-1]]>order[picked[j]];j--) { unsigned t=picked[j]; picked[j]=picked[j-1]; picked[j-1]=t; }
    static const uint8_t metals[3]={PH_CLANG,PH_METAL,PH_ANVIL}, clicks[2]={PH_ZAP,PH_GLITCH};
    static const uint8_t bodies[3]={PH_ROLE_TOM,PH_ROLE_KICK,PH_ROLE_METAL}, destroyable[8]={0,1,2,3,4,5,6,7};
    static const uint8_t arche8[8]={0,1,2,3,4,5,6,7};
    for(unsigned i=0;i<KIT_PAGE;i++) {
        unsigned k=picked[i];
        KitSound s={0,0,KF_NONE,0};
        if(ba) {                                 /* BAYAREA: the category's family; its seed draws the rest */
            s.pad=(uint8_t)k; s.archetype=0xffu; s.flavour=KF_BAYAREA; s.kind=(uint8_t)category_baf(k,c->cat[k]&1u);
            S->plan[i]=s;
            kit_count(c,&s);
            continue;
        }
        switch(k) {
        case C_KICK: s.pad=PH_ROLE_KICK; s.kind=PH_KICK; break;
        case C_KICK_G: s.pad=PH_ROLE_KICK|PH_GNARLY; s.kind=PH_KICK; break;
        case C_SNARE: s.pad=PH_ROLE_SNARE; s.kind=PH_SNARE; break;
        case C_SNARE_G: s.pad=PH_ROLE_SNARE|PH_GNARLY; s.kind=PH_SNARE; break;
        case C_CLAP: s.pad=PH_ROLE_CLAP; s.kind=PH_CLAP; break;
        case C_HAT: s.pad=PH_ROLE_HAT|(c->cat[C_HAT]%3u==2u ? PH_GNARLY : 0); s.kind=PH_HIHAT; break;
        case C_CYMBAL: s.pad=PH_ROLE_HAT; s.kind=PH_HIHAT; s.flavour=KF_CYMBAL; break;
        case C_METAL: s.pad=PH_ROLE_METAL; s.kind=(uint8_t)kit_least(c->kind,metals,3,&rnd); break;
        case C_METAL_G: s.pad=PH_ROLE_METAL|PH_GNARLY; s.kind=(uint8_t)kit_least(c->kind,metals,3,&rnd); break;
        case C_NOISE: s.pad=PH_ROLE_NOISE|(c->cat[C_NOISE]&1u ? PH_GNARLY : 0); s.kind=PH_RUST; break;
        case C_CLICK: s.pad=PH_ROLE_CLICK|(c->cat[C_CLICK]%3u==2u ? PH_GNARLY : 0); s.kind=(uint8_t)kit_least(c->kind,clicks,2,&rnd); break;
        case C_IMPACT: {
            uint8_t uses[8]={0}; for(unsigned b=0;b<3;b++) uses[bodies[b]]=c->role_g[bodies[b]];
            unsigned body=kit_least(uses,bodies,3,&rnd);
            s.pad=(uint8_t)(body|PH_GNARLY); s.flavour=KF_IMPACT;
            s.kind=(uint8_t)(body==PH_ROLE_TOM ? PH_TOM : body==PH_ROLE_KICK ? PH_KICK : PH_ANVIL);
            break;
        }
        default: {   /* destroyed: the role least destroyed so far */
            unsigned role=kit_least(c->role_g,destroyable,8,&rnd);
            s.pad=(uint8_t)(role|PH_GNARLY); s.flavour=KF_DESTROY;
            s.kind=(uint8_t)ph_role_kind(role,S->kit_seed+i);
            break;
        }
        }
        /* the least used character archetype of this category (gnarly: 0..6) */
        s.archetype=(uint8_t)kit_least(c->arche[k],arche8,s.pad>=PH_GNARLY && (s.pad&7u)!=PH_ROLE_KICK ? 7u : 8u,&rnd);
        S->plan[i]=s;
        kit_count(c,&s);
    }
}
/* Kit page: the next of 16 planned sounds into kit_start + kit_next (each
   stored like a single sound; the store's close callback calls this again). */
static __attribute__((noinline)) void kit_make(void) {
    if(S->kit_state==KIT_IDLE) {
        S->kit_seed=(S->kit_seed*2654435761u)^entropy()^(S->ticks<<4)^S->kits^0x4b17u;
        S->kit_engine=S->engine;
        KitCensus census;
        memset(&census,0,sizeof census);
        kit_tags(&census,S->kit_start,0);
        kit_plan(&census);
        S->kit_next=0; S->kit_state=KIT_MAKING;
    }
    if(S->kit_next>=KIT_PAGE) { S->kit_state=KIT_IDLE; S->kits++; return; }
    Pending *p=&S->pending[KIT_TRACK];
    const KitSound *k=&S->plan[S->kit_next];
    unsigned slot=S->kit_start+S->kit_next;
    p->seed=S->kit_seed^(slot*0x9E3779B9u); p->kit=1; p->family=0xffu; p->samples=0; p->peak=0; p->valid=1; p->engine=S->kit_engine;
    p->pad=k->pad; p->archetype=k->archetype; p->flavour=k->flavour; p->kind=k->kind;
    if(!commit_start(KIT_TRACK,slot)) { S->kit_state=KIT_IDLE; refuse(); }
}
/* Clear: delete the page's 16 slots one by one; the file-changed event after
   each frees its DSP pages and clears its "has sample" bit. */
static void clear_deleted(const StoragePath *path, unsigned kind, unsigned result, uint32_t a, uint32_t b, void *ctx);
static __attribute__((noinline)) void clear_next(void) {
    if(S->kit_next>=KIT_PAGE) { S->kit_state=KIT_IDLE; S->clears++; return; }
    S->path.type=4; S->path.index=S->kit_start+S->kit_next; S->path.pack=0xfe; S->path.rsv[0]=S->path.rsv[1]=S->path.rsv[2]=0;
    S->commit_progress=S->services;
    STORAGE_DELETE(&S->path,clear_deleted);
}
static void clear_deleted(const StoragePath *path, unsigned kind, unsigned result, uint32_t a, uint32_t b, void *ctx) {
    (void)path; (void)kind; (void)a; (void)b; (void)ctx;
    if(S->kit_state!=KIT_CLEARING) return;
    if(result!=0 && result!=5) { S->commit_slot=S->kit_start+S->kit_next; commit_fail(6); return; }
    StoragePath changed=S->path;
    changed.pack=CURRENT_PACK();
    FILE_CHANGED(&changed);
    S->kit_next++;
    clear_next();
}
static __attribute__((noinline)) void kit_clear(void) {
    if(S->kit_state!=KIT_IDLE || S->commit_state!=COMMIT_IDLE) { refuse(); return; }
    S->kit_state=KIT_CLEARING; S->kit_next=0;
    clear_next();
}
/* Store track t's waiting sound into slot (0..127) of the current pack. */
static __attribute__((noinline)) unsigned commit_start(unsigned track, unsigned slot) {
    if(S->commit_state!=COMMIT_IDLE || slot>=128u) return 0;
    Pending *p=&S->pending[track];
    if(!p->samples) {
        /* not measured yet (its audition was cut short): render it once dry */
        Gen g; icache_on();
        gen_start(1,&g,track);
        while(gen_active(1)) gen_render(1,&g,S->store_mix);
        icache_off();
        p->samples=g.block*FRAMES; p->peak=g.peak;
    }
    S->commit_track=track; S->commit_slot=slot;
    S->gain=p->peak>0.0001f ? 0.891f/p->peak : 1.0f;          /* -1 dBFS */
    if(S->gain>8.0f) S->gain=8.0f;
    S->commit_sample=0; S->mix_pos=FRAMES; S->commit_offset=0;
    S->commit_total=WAV_HEADER+2u*(p->samples+TAIL);
    gen_start(1,&S->gen_store,track);
    S->path.type=4; S->path.index=slot; S->path.pack=0xfe; S->path.rsv[0]=S->path.rsv[1]=S->path.rsv[2]=0;
    S->buffer=SCRATCH_ACQUIRE();
    if(!S->buffer) { commit_fail(5); return 0; }
    S->commit_state=COMMIT_DELETE; S->commit_progress=S->services;
    STORAGE_DELETE(&S->path,commit_deleted);
    return 1;
}
#pragma clang attribute pop
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)
#include "pattern.c"
#pragma clang attribute pop
/* Rare work, kept in flash: FIFO (re)initialization and the telemetry
   refresh every 128 service calls. */
static __attribute__((noinline)) void tick_housekeeping(void) {
    if(!S->primed || (S->ticks&127u)==1u) S->dsp_magic=dsp_read(CTRL);
    if(S->dsp_magic!=MAGIC) {
        for(unsigned i=0;i<SLOT_COUNT;i++) dsp_write(SLOT(i),0);
        for(unsigned i=1;i<CTRL_WORDS;i++) dsp_write(CTRL+i,0);
        for(unsigned t=0;t<TRACKS;t++) dsp_write(GFX_ROUTE+t,0);   /* every track through the Grid FX bus (stock) */
        S->gfx_routes=0;
        dsp_write(CTRL_READ_SLOT,SLOT0);
        dsp_write(CTRL,MAGIC);
        S->dsp_magic=MAGIC; S->primed=0;
    }
    if((S->ticks&127u)==1u) {
        S->dsp_status=dsp_read(CTRL+4); S->dsp_frames=dsp_read(CTRL+5);
        S->dsp_heartbeats=dsp_read(CTRL+6);
        S->dsp_underruns=dsp_read(CTRL+2); S->dsp_drops=dsp_read(CTRL+3);
        S->dsp_copies=dsp_read(CTRL+7); S->dsp_nonzero_copies=dsp_read(CTRL_NONZERO);
    }
}
static uint32_t seq_diff(uint32_t a, uint32_t b) { return a>=b ? a-b : a+SEQ_WRAP-b; }
static int pending_request(void) {
    for(unsigned t=0;t<TRACKS;t++) if(S->served[t]!=S->requested[t] || S->replayed[t]!=S->replay[t]) return 1;
    return 0;
}
__attribute__((noinline)) void fm_tick(void) {
    if(S->magic!=MAGIC || S->busy) return;
    S->busy=1;
    uint32_t call_start=cycles();
    if(S->ticks) { uint32_t gap=call_start-S->last_service; if(gap>S->max_service_gap) S->max_service_gap=gap; }
    S->last_service=call_start;
    S->ticks++;
#ifndef FM_HOST
    {   /* stock timer tick counters (TIM5/TIM4 output-compare objects, +0xa count, limit 254) */
        uint32_t a=*(volatile uint8_t *)0x200001dau, b=*(volatile uint8_t *)0x200001e6u;
        if(a>(S->timer_backlog&255u)) S->timer_backlog=(S->timer_backlog&~255u)|a;
        if(b>(S->timer_backlog>>8&255u)) S->timer_backlog=(S->timer_backlog&~0xff00u)|b<<8;
    }
#endif
    tick_housekeeping();
    /* Idle (no audition, no request): nothing else to do, no DSP traffic. */
    if(!gen_active(0) && !pending_request()) { S->busy=0; return; }
    icache_on();
    /* The DSP consumer advances only after copying a block; while no audition
       sounds it counts misses and keeps its read sequence, so the producer
       resumes exactly where it stopped. */
    uint32_t expected=dsp_read(CTRL+1)&MASK24;
    if(expected>=SEQ_WRAP) expected=0;
    S->dsp_sequence=expected;
    if(!S->primed) { S->next_sequence=expected; S->primed=1; }
    {
        uint32_t queued=seq_diff(S->next_sequence,expected);
        if(queued<SEQ_WRAP/2u && queued<S->min_queued) S->min_queued=queued;
    }
    for(unsigned budget=0;budget<BLOCKS_PER_CALL;budget++) {
        uint32_t ahead=seq_diff(S->next_sequence,expected);
        if(ahead>=SEQ_WRAP/2u) { S->next_sequence=expected; S->resyncs++; ahead=0; }
        if(ahead>=MAX_AHEAD) break;
        uint32_t slot=SLOT(S->next_sequence);
        if(dsp_read(slot)) break;
        uint32_t start=cycles();
        unsigned sounding=render_block();
        uint32_t rendered=cycles();
        if(!sounding) break;
        S->block_cycles=rendered-start;
        if(S->block_cycles>S->max_block_cycles) S->max_block_cycles=S->block_cycles;
        S->render_sum+=S->block_cycles; S->render_n++;
        S->nonzero_blocks++;
        S->render[0]=(int32_t)S->next_sequence;
        S->render[1]=(int32_t)(FRAMES|(uint32_t)S->audition_track<<TRACK_SHIFT);
        if(!fm_write_block(slot+1,S->render,BLOCK_WORDS)) { S->bulk_errors++; break; }
        dsp_write(slot,1); /* Commit only after the buffered DMA completes. */
        {
            uint32_t sent=cycles()-rendered;
            S->transfer_sum+=sent; S->transfer_n++;
            if(sent>S->max_transfer) S->max_transfer=sent;
        }
        S->next_sequence=S->next_sequence+1u>=SEQ_WRAP ? 0 : S->next_sequence+1u; S->blocks++;
        if(cycles()-call_start>CALL_BUDGET && ahead+1u>=CALL_MIN_AHEAD) break;
    }
    { uint32_t spent=cycles()-call_start; if(spent>S->max_call_cycles) S->max_call_cycles=spent; }
    icache_off();
    S->busy=0;
}
void lock_sweep(void);
void fm_service(void) {
    if(S->magic==MAGIC) { S->services++; lock_sweep(); }
    fm_tick();
    ((void (*)(void))0x080305cbu)();
}
