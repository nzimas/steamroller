/* SPDX-License-Identifier: GPL-3.0-only
 * Step mixer and Grid FX locks: with Record engaged, holding a step in a track's view
 * turns the two bottom pad rows (normally the sample pads) into sliders for
 * that step's level, pan, delay send and reverb send. Two rows show at a
 * time; the sample-page buttons scroll through them (0x13, page+1: the next
 * parameter; 0x14, page-1: back): level/pan, pan/delay, delay/reverb.
 *   - unipolar rows (level, delay, reverb): pad 1..8 = 0, 18, 36 ... 127;
 *   - pan (bipolar): pads 1-4 left, 5-8 right (0, 16, 32, 48 | 80 ... 127),
 *     the middle two together = centre;
 *   - pressing the pad that shows the step's current lock removes the lock.
 * A locked step shows a bright bar; an unlocked one shows the knob's value dim.
 * The value is written the way a held step + knob turn writes it: the stock
 * sequencer message 0x08047038(knob router [+0x3C], track, lane, value), so
 * the editor stores it on its held step. Lanes: 8 reverb send (0x80+t),
 * 9 delay send (0x88+t), 10 level (0x91+t), 11 pan (0x99+t); played by plock.c.
 *
 * Stock state read here (docs/RHYTHM-AUTOMATION-PLOCKS.md §6):
 *   - Record: the transport object's store [+0x28] index 1 (set by its Record
 *     message, 0x08033CA8; the live-record gate 0x08036B06 reads the same);
 *     the object is captured as its listener runs (fm_transport_message).
 *   - track view: router view 0, object router[+0x3C]; its sub-mode byte
 *     +0x264 = 0 (the sample pads), current track = store [+0x25C] index 0.
 *   - held step: the knob router's per-track object X = *([R+0x34] + t*[R+0x2C]):
 *     store X[+0x24] index 0 = a step is held, store X[+0xC] index 1 = the step,
 *     *(X[0]+0x18) = the current pattern's lanes (as the knob display 0x08038EF2).
 */
#define SL_MIX 4u                     /* level, pan, delay, reverb; then the Grid FX rows (gridfx.c) */
#define SL_ROWS (SL_MIX + GFX_TYPES)
static const uint8_t sl_lane[SL_MIX] = {10, 11, 9, 8};
static const uint8_t sl_id[SL_MIX] = {0x91, 0x99, 0x88, 0x80};       /* + track */
/* the FX rows, top to bottom: bit crusher, stutter (gater), looper, filter, vinyl, reverser, phaser */
static const uint8_t sl_fx[GFX_TYPES] = {4, 2, 0, 5, 6, 1, 3};
/* mixer rows: green, cyan, yellow, violet. FX rows: the stock Grid FX view's colours
   (0x0805AB02: palette 0x08062AB0 [0x39 + type] latched, [0x40 + type] idle):
   crusher light blue, stutter pale yellow, looper red, filter indigo, vinyl magenta,
   reverser orange, phaser green */
static const uint8_t sl_on[SL_ROWS] = {0x22, 0x18, 0x6d, 0x47, 0x10, 0x2d, 0x53, 0x62, 0x5e, 0x41, 0x1c};
static const uint8_t sl_dim[SL_ROWS] = {0x24, 0x19, 0x73, 0x48, 0x11, 0x2e, 0x54, 0x63, 0x5f, 0x42, 0x1d};
static const uint8_t sl_pan[8] = {0, 16, 32, 48, 80, 96, 112, 127};

typedef struct { unsigned track, step; uint8_t *lanes; } SlStep;

static void *sl_ptr(void *p, unsigned o) { return *(void **)((uint8_t *)p + o); }
#ifndef FM_HOST
static unsigned sl_store(void *store, unsigned index) { return *plock_store(store, index); }
#else
extern unsigned sl_store(void *store, unsigned index);
#endif

/* Record is engaged (the transport store's index 1). */
static unsigned sl_record(void) {
    return S->transport && sl_ptr(S->transport, 0x28) && sl_store(sl_ptr(S->transport, 0x28), 1);
}
/* The track view (object `view`, i.e. router[+0x3C]) on its sample pads, with
   Record on and a step held: fills the track, step and lanes. */
static unsigned sl_active(void *view, SlStep *h) {
    void *R = S->plock_router;
    if (S->magic != MAGIC || !view || !R || !sl_record()) return 0;
    if (*((uint8_t *)view + 0x264) != 0) return 0;
    const unsigned t = sl_store(sl_ptr(view, 0x25c), 0) & 7u;
    void *X = *(void **)((uint8_t *)sl_ptr(R, 0x34) + t * *(uint32_t *)((uint8_t *)R + 0x2c));
    if (!X || !sl_store(sl_ptr(X, 0x24), 0)) return 0;
    h->track = t;
    h->step = sl_store(sl_ptr(X, 0x0c), 1) & 31u;
    h->lanes = (uint8_t *)sl_ptr(*(void **)X, 0x18);
    return h->lanes != 0;
}
/* The step's lock in a mixer row (0xFF: none): micro-step 0 (1-5 hold FX locks). */
static unsigned sl_value(const SlStep *h, unsigned row) {
    return h->lanes[192u * sl_lane[row] + 6u * h->step];
}
static unsigned sl_pad_value(unsigned row, unsigned i) { return row == 1u ? sl_pan[i] : (i * 127u + 3u) / 7u; }

/* The pad a value lights up to (unipolar: the bar's end; pan: the side's end). */
static unsigned sl_reach(unsigned row, unsigned v) {
    if (row != 1u) return (v * 7u + 63u) / 127u;
    if (v == 64u) return 8u;                                           /* centre: pads 4 and 5 */
    unsigned best = 0;
    for (unsigned i = 1; i < 8u; i++) {
        const int a = (int)sl_pan[i] - (int)v, b = (int)sl_pan[best] - (int)v;
        if ((a < 0 ? -a : a) < (b < 0 ? -b : b)) best = i;
    }
    return best;
}

/* Pads 16-31 while active: set (or remove) the step's lock. Returns 1 when consumed. */
static unsigned sl_pad(void *router, unsigned pad) {
    SlStep h;
    if (pad < 16u || pad > 31u || router_view(router) != 0 || !sl_active(sl_ptr(router, 0x3c), &h)) return 0;
    const unsigned slot = (pad - 16u) >> 3, i = pad & 7u, row = S->sl_row + slot;
    S->sl_held |= 1u << (pad - 16u);
    if (row >= SL_MIX) {
        /* an FX row: pad i = preset i; its own pad again removes it */
        const unsigned k = sl_fx[row - SL_MIX];
        uint8_t *b = gfx_byte(h.lanes, h.step, k);
        if (gfx_get(h.lanes, h.step, k) == i) { *b = 0xffu; S->sl_clears++; }
        else { *b = (uint8_t)(0x80u | i); S->sl_writes++; }
        S->sl_last = i | (0x20u + k) << 8 | h.step << 12 | h.track << 20;
        return 1;
    }
    unsigned v = sl_pad_value(row, i);
    if (row == 1u && (i == 3u || i == 4u) && (S->sl_held >> (8u * slot + 7u - i) & 1u)) v = 64;   /* the middle pair: centre */
    const unsigned now = sl_value(&h, row);
    uint8_t *b = h.lanes + 192u * sl_lane[row] + 6u * h.step;
    if (now != 0xffu && (now == v || sl_reach(row, now) == i)) {
        b[0] = 0xffu;                                                  /* the lock's own pad again: unlock */
        S->sl_clears++;
    } else {
#ifndef FM_HOST
        ((void (*)(void *, unsigned, unsigned, unsigned))0x08047039u)(sl_ptr(S->plock_router, 0x3c), h.track, sl_lane[row], v);
#else
        b[0] = (uint8_t)v;
#endif
        S->sl_writes++;
    }
    S->sl_last = v | sl_lane[row] << 8 | h.step << 12 | h.track << 20;
    return 1;
}
static void sl_release(unsigned pad) {
    if (pad >= 16u && pad <= 31u) S->sl_held &= ~(1u << (pad - 16u));
}

/* Router button press (vtable word 0x080672EC, stock 0x0803D8B8): while
   active, the sample-page buttons scroll the rows instead of the sample pages. */
unsigned fm_button(void *router, unsigned id, unsigned a, unsigned b) {
    SlStep h;
    if ((id == 0x13u || id == 0x14u) && router_view(router) == 0 && sl_active(sl_ptr(router, 0x3c), &h)) {
        if (id == 0x13u && S->sl_row < SL_ROWS - 2u) S->sl_row++;
        if (id == 0x14u && S->sl_row) S->sl_row--;
        return 1;
    }
#ifndef FM_HOST
    return ((unsigned (*)(void *, unsigned, unsigned, unsigned))0x0803d8b9u)(router, id, a, b);
#else
    (void)a; (void)b; return 0;
#endif
}

/* The track view's frame (vt+0x1C, vtable word 0x080678F8, stock 0x0804DD1C):
   stock composes it, then the bottom rows show the sliders while active. */
uint16_t *fm_track_frame(void *view) {
#ifndef FM_HOST
    uint16_t *f = ((uint16_t *(*)(void *))0x0804dd1du)(view);
#else
    extern uint16_t *host_frame(void *); uint16_t *f = host_frame(view);
#endif
    SlStep h;
    if (!f || !sl_active(view, &h)) { if (S->magic == MAGIC) { S->sl_row = 0; S->sl_held = 0; } return f; }
    for (unsigned slot = 0; slot < 2u; slot++) {
        const unsigned row = S->sl_row + slot;
        if (row >= SL_MIX) {
            const unsigned cur = gfx_get(h.lanes, h.step, sl_fx[row - SL_MIX]);
            for (unsigned i = 0; i < 8u; i++) f[16u + 8u * slot + i] = cur == i ? sl_on[row] : sl_dim[row];
            continue;
        }
        const unsigned now = sl_value(&h, row);
        unsigned v = now;
        if (v == 0xffu) { v = plock_live(sl_id[row] + h.track); if (v > 127u) v = row == 1u ? 64u : 0; }
        const uint8_t lit = now == 0xffu ? sl_dim[row] : sl_on[row];
        const unsigned reach = sl_reach(row, v);
        for (unsigned i = 0; i < 8u; i++) {
            unsigned on;
            if (row != 1u) on = i <= reach;
            else if (reach == 8u) on = i == 3u || i == 4u;
            else on = reach < 4u ? i >= reach && i <= 3u : i >= 4u && i <= reach;
            f[16u + 8u * slot + i] = on ? lit : (now == 0xffu ? 0 : sl_dim[row]);
        }
    }
    return f;
}

/* The transport object's message listener (vtable words 0x08066B40 and the
   thunk 0x08066B6C, stock 0x08033CA8 / 0x08033CA6): remember the object (its
   store [+0x28] holds Record), then stock. */
unsigned fm_transport_message(void *object, unsigned id, unsigned value, unsigned extra) {
    if (S->magic == MAGIC) S->transport = object;
#ifndef FM_HOST
    return ((unsigned (*)(void *, unsigned, unsigned, unsigned))0x08033ca9u)(object, id, value, extra);
#else
    (void)id; (void)value; (void)extra; return 0;
#endif
}
unsigned fm_transport_message_thunk(void *iface, unsigned id, unsigned value, unsigned extra) {
    return fm_transport_message((uint8_t *)iface - 8, id, value, extra);
}
