/* SPDX-License-Identifier: GPL-3.0-only */
/* ENDLESS MEMORY: a firmware for the M-VAVE FM-1, built on Dinghy (zvenson), Felucca (Leo Kuroshita, Hügelton
 * Instruments) and SLOOP (isod89).
 *
 * The sound: ENDLESS. Every voice is a Shepard tone: eight partials spaced by an interval (ALGORITHM: octaves,
 * fifths, ...) under a bell of brightness. CLIMB moves all partials up or down for ever: the pitch seems to rise or
 * fall without end, because partials fade in at the bottom and out at the top.
 *
 * The sequencer: STAIRCASE. Play a short phrase (2 to 8 notes, REC on). It repeats, and every repeat moves it by
 * MOVE steps of the scale, up or down, so the melody climbs (or falls) for ever, as the sound does.
 *
 * SCALE and the transpose (ROOT) act last, on what you hear: what is recorded stays as it was played.
 *
 * The panel (every button does what its label says; only SEL and HOME have new jobs):
 *   EDIT   the engine: CLIMB, BRIGHT, WIDTH, BLUR        ALGORITHM  the spacing of the partials
 *   ENV    the volume envelope; again: the brightness envelope
 *   LFO    RATE, DEPTH, SHAPE, TO (what it moves)        FX     ECHO, TIME, SPACE, TONE
 *   ARP    arpeggiator on / off and its page             SEQ    the staircase: MOVE, REPEATS, RATE, GATE
 *   GLO    TEMPO, SCALE, ROOT, MIDI channel; here the keys set the root (and transpose everything)
 *   SELECT tempo, everywhere
 *   PLAY   start / stop          REC  record a new phrase on / off (hold 1 s: erase it)
 *   SEL    FREEZE: like a sustain pedal that stays down  HOME   the home page: the filters (HP and LP, freq and res)
 *   SAVE   save the sound and memory (hold SAVE and turn PRESETS: save to another place)
 *   PRESETS  eight sounds, loaded at once                OCT- OCT+  octave
 * Hold a page button and turn a knob: you edit that page and come back when you let go. */
#define APP_NAME "ENDLESS MEMORY"
#define STORE_MAGIC 0x4D454D45u                 /* "EMEM": our own data */
#define CV_MAX (240u * 144u)
static void app_idle(void);
#define LCD_IDLE app_idle
#include "platform.c"
#include "em_tables.h"

#ifndef EM_THEME
#define EM_THEME 0                              /* 0 MONO, 1 PAPER, 2 GRID */
#endif
#ifndef EM_LOGO
#define EM_LOGO 8                               /* 0 RISE 1 CLOCK 2 FADE 3 FREEZE 4 SPIRAL 5 GRID 6 ECHO 7 LOOP 8 WORD 9 MONOGRAM */
#endif

#ifdef DINGHY_HOST
static uint32_t em_ticks(void) { return fm1_ms * 24000u; }
#else
static uint32_t em_ticks(void) { return fm1_ticks(); }
#endif

/* ================================================================ the data */
#define NV 6u                                   /* voices */
#define NP 8u                                   /* partials a voice */
#define NPRESET 8u

typedef struct {
    int16_t alg, climb, bright, width, blur;    /* EDIT */
    int16_t a, d, s, r;                         /* ENV (volume) */
    int16_t ta, td, ts, tamt;                   /* ENV (brightness) */
    int16_t lrate, ldepth, lshape, ldest;       /* LFO */
    int16_t echo, etime, space, tone;           /* FX */
    int16_t arp, amode, arate, aoct, agate;     /* ARP */
    int16_t smove, sreps, srate, sgate;         /* SEQ: the staircase */
    int16_t level;
    char name[10];
    int16_t hpf, hpq, lpf, lpq;                 /* the 2-pole filters before echo and space (STAGE page) */
} patch_t;
#define MAXPH 8u                                /* notes in a phrase */
typedef struct { uint32_t ver; patch_t p; uint8_t n, pad[3]; uint8_t note[MAXPH]; } preset_t;
#define PRESET_VER 0x454D0004u

static patch_t P;
static uint8_t ph_note[MAXPH], ph_n;            /* the phrase, as played */

typedef struct { uint32_t ver; int16_t tempo, scale, root, ch, oct, preset; } glob_t;   /* root: transpose -12..12 */
static glob_t G = {2, 96, 0, 0, 0, 0, 0};

/* ================================================================ the sound (audio interrupt) */
static const int16_t SPACING[6] = {768, 448, 320, 384, 256, 800};   /* 1/64 semitone: 12, 7, 5, 6, 4, 12.5 */
static const int8_t DETUNE[NP] = {-29, 17, -8, 31, -21, 5, 24, -13}; /* BLUR's spread of each partial */

static volatile struct {                        /* written by the main loop, read by the interrupt */
    int32_t climb_inc, S, bright, width, blur;
    uint32_t att, dec, sus, rel, tatt, tdec, tsus;
    int32_t tamt, lfo_depth, lfo_shape, lfo_dest, level, bend;
    uint32_t lfo_inc;
    int32_t echo_mix, echo_fb, echo_len, space, rev_fb, lp_k;
    int32_t hp_a1, hp_a2, hp_a3, hp_k, lf_a1, lf_a2, lf_a3;   /* Q24 */
} E;
static int32_t lim_g = 32768;
static int32_t hpL1, hpL2, hpR1, hpR2, lfL1, lfL2, lfR1, lfR2;   /* the filters' two states, each side */

/* a zero-delay state-variable filter step (2 poles); returns the low-pass, *bp the band-pass.
 * The damping never goes below 0.25, so it rings but never sings by itself. */
static inline int32_t svf(int32_t x, int32_t *s1, int32_t *s2, int32_t a1, int32_t a2, int32_t a3, int32_t *bp)
{
    int32_t v3 = x - *s2;
    int32_t v1 = (int32_t)(((int64_t)a1 * *s1 + (int64_t)a2 * v3) >> 24);
    int32_t v2 = *s2 + (int32_t)(((int64_t)a2 * *s1 + (int64_t)a3 * v3) >> 24);
    *s1 = 2 * v1 - *s1;
    *s2 = 2 * v2 - *s2;
    *bp = v1;
    return v2;
}

typedef struct {
    volatile uint8_t note, vel, gate, trig;
    uint8_t raw;                                /* (main loop) the note as played, before scale and transpose */
    volatile uint8_t stage, tstage;
    volatile int32_t env, tenv;                 /* Q24 */
    uint32_t ph[NP], inc[NP];
    int32_t aL[NP], aR[NP];                     /* Q23: the levels now */
    int32_t tL[NP], tR[NP];                     /* Q15: where they go in this tick */
    uint32_t off_at, age;                       /* (main loop) */
    uint8_t timed;                              /* (main loop) started by the memory or the arp: off_at stops it */
} voice_t;
static voice_t V[NV];

static volatile int32_t climb;                  /* where the partials are: 1/64 semitone << 16, 0 .. S << 16 */
static volatile uint32_t lfo_ph;
static volatile int32_t lfo_now;                /* -32767 .. 32767 */
static uint32_t lfo_rnd = 0x1234567u, lfo_prev, lfo_target;
static volatile uint32_t isr_us;                /* how long the last render took */

static int16_t echo_buf[44100] __attribute__((section(".pool")));
static int16_t cb0[1557] __attribute__((section(".pool")));
static int16_t cb1[1617] __attribute__((section(".pool")));
static int16_t cb2[1491] __attribute__((section(".pool")));
static int16_t cb3[1422] __attribute__((section(".pool")));
static int16_t ap0[225] __attribute__((section(".pool")));
static int16_t ap1[556] __attribute__((section(".pool")));
static int16_t ap2[341] __attribute__((section(".pool")));
static int16_t ap3[441] __attribute__((section(".pool")));
static uint32_t echo_pos, ci0, ci1, ci2, ci3, ai0, ai1, ai2, ai3;
static int32_t cf0, cf1, cf2, cf3, lpL, lpR;

static uint32_t rnd32(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static int32_t clip16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }
static int32_t iabs(int32_t x) { return x < 0 ? -x : x; }

static int32_t lfo_shape_at(uint32_t ph, int32_t shape)
{
    int32_t t = (int32_t)(ph >> 16);            /* 0 .. 65535 */
    switch (shape) {
    case 0: return SINE[ph >> 22];
    case 1: return t < 32768 ? t * 2 - 32767 : 32767 - (t - 32768) * 2;
    case 2: return t < 32768 ? 32767 : -32767;
    case 3: return 32767 - t;
    default: return 0;
    }
}

static void lfo_tick(void)
{
    uint32_t old = lfo_ph, ph = old + E.lfo_inc;
    lfo_ph = ph;
    if (E.lfo_shape >= 4) {                     /* random: a new value each cycle; drift: glides to it */
        if (ph < old) { lfo_prev = lfo_target; lfo_target = rnd32(&lfo_rnd) & 0xFFFFu; }
        if (E.lfo_shape == 4) lfo_now = (int32_t)lfo_target - 32768;
        else {
            int32_t a = (int32_t)lfo_prev - 32768, b = (int32_t)lfo_target - 32768, f = (int32_t)(ph >> 17);
            lfo_now = a + (((b - a) * f) >> 15);
        }
    } else
        lfo_now = lfo_shape_at(ph, E.lfo_shape);
}

static void env_tick(voice_t *v)
{
    int32_t e = v->env, t = v->tenv;
    uint8_t st = v->stage, ts = v->tstage;
    if (v->trig) { v->trig = 0; st = 1; ts = 1; }
    if (!v->gate && st && st < 3u) { st = 3; ts = 3; }
    switch (st) {
    case 1:
        e += (int32_t)E.att;
        if (e >= (1 << 24)) { e = 1 << 24; st = 2; }
        break;
    case 2:
        e -= ((e - (int32_t)E.sus) >> 8) * (int32_t)E.dec >> 8;
        break;
    case 3:
        e -= (e >> 8) * (int32_t)E.rel >> 8;
        if (e < 2048) { e = 0; st = 0; }
        break;
    }
    switch (ts) {
    case 1:
        t += (int32_t)E.tatt;
        if (t >= (1 << 24)) { t = 1 << 24; ts = 2; }
        break;
    case 2:
        t -= ((t - (int32_t)E.tsus) >> 8) * (int32_t)E.tdec >> 8;
        break;
    case 3:
        t -= (t >> 8) * (int32_t)E.rel >> 8;
        break;
    }
    v->env = e; v->tenv = t; v->stage = st; v->tstage = ts;
}

/* the partials' pitches and target levels for one control tick (32 frames) */
static void voice_tick(voice_t *v, int32_t c, int32_t S, int32_t R, uint32_t inv_half, int32_t m_pitch,
                       int32_t m_bright, int32_t m_width, int32_t trem)
{
    uint32_t k, invw;
    int32_t a[NP], sum = 0, f, gain, w, br, keep = 32767 - E.blur * 70;
    br = E.bright + m_bright + (((v->tenv >> 9) * E.tamt) >> 15);
    w = E.width + m_width;
    if (w < 96) w = 96;
    invw = (256u << 16) / (uint32_t)w;
    for (k = 0; k < NP; k++) {
        int32_t x = (int32_t)k * S + c, d, e, b, p;
        uint32_t ie, ib;
        if (x >= R) x -= R;
        d = x - (R >> 1);
        ie = ((uint32_t)iabs(d) * inv_half) >> 16;
        e = ie >= 256u ? 0 : BELL[ie];
        ib = ((uint32_t)iabs(d - br) * invw) >> 16;
        b = ib >= 256u ? 0 : BELL[ib];
        p = (int32_t)v->note * 64 + d + E.bend + m_pitch + ((DETUNE[k] * E.blur) >> 5);
        a[k] = (e * b) >> 15;
        if (p < 30 * 64)                        /* the deepest partials fade out from 46 Hz to 23 Hz: no rumble for small speakers */
            a[k] = p <= 18 * 64 ? 0 : a[k] * (p - 18 * 64) / (12 * 64);
        if (p < 16 * 64 || p >= 131 * 64) { a[k] = 0; p = 60 * 64; }
        v->inc[k] = INC_TOP[(uint32_t)p % 768u] >> (10u - (uint32_t)p / 768u);
        sum += a[k];
    }
    gain = (((v->env >> 9) * (int32_t)v->vel) >> 7) * E.level * 2 >> 9;   /* Q15 (some room before the limiter) */
    gain = (gain * trem) >> 15;
    f = sum < 32768 ? gain : (int32_t)(((uint32_t)gain << 15) / (uint32_t)sum);
    for (k = 0; k < NP; k++) {
        int32_t t = (a[k] * f) >> 15, ts = (t * keep) >> 15;
        v->tL[k] = (k & 1u) ? ts : t;
        v->tR[k] = (k & 1u) ? t : ts;
    }
}

static void render_voice(voice_t *v, int32_t *bl, int32_t *br)
{
    uint32_t k, i;
    for (k = 0; k < NP; k++) {
        int32_t aL = v->aL[k], aR = v->aR[k], dL = ((v->tL[k] << 8) - aL) >> 5, dR = ((v->tR[k] << 8) - aR) >> 5;
        uint32_t ph = v->ph[k], inc = v->inc[k];
        if (!aL && !aR && !dL && !dR) { v->ph[k] = ph + inc * 32u; continue; }
        for (i = 0; i < 32u; i++) {
            int32_t s, s0;
            ph += inc;
            s0 = SINE[ph >> 22];                /* read between the table's points: clean low notes */
            s = s0 + (((SINE[((ph >> 22) + 1u) & 1023u] - s0) * (int32_t)(ph >> 6 & 0xFFFFu)) >> 16);
            aL += dL; aR += dR;
            bl[i] += (s * (aL >> 8)) >> 15;
            br[i] += (s * (aR >> 8)) >> 15;
        }
        v->ph[k] = ph;
        v->aL[k] = v->tL[k] << 8;
        v->aR[k] = v->tR[k] << 8;
    }
}

static int32_t comb(int16_t *buf, uint32_t n, uint32_t *i, int32_t *filt, int32_t in, int32_t fb)
{
    int32_t y = buf[*i];
    *filt = y + (((*filt - y) * 13107) >> 15);  /* damping 0.4 */
    buf[*i] = (int16_t)clip16(in + ((*filt * fb) >> 15));
    if (++*i >= n) *i = 0;
    return y;
}
static int32_t allpass(int16_t *buf, uint32_t n, uint32_t *i, int32_t in)
{
    int32_t b = buf[*i];
    buf[*i] = (int16_t)clip16(in + (b >> 1));
    if (++*i >= n) *i = 0;
    return b - in;
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t t0 = em_ticks(), j, i, vi;
    int32_t S = E.S, R = S * (int32_t)NP, bl[32], br[32];
    uint32_t inv_half;
    if (S < 64) return;                         /* (not set up yet) */
    inv_half = (256u << 16) / (uint32_t)(R >> 1);
    for (j = 0; j + 32u <= n; j += 32u) {
        int32_t c, m_pitch = 0, m_bright = 0, m_width = 0, m_climb = 0, trem = 32767, ld = E.lfo_depth;
        lfo_tick();
        switch (E.lfo_dest) {                   /* what the LFO moves */
        case 0: m_pitch = (lfo_now * ld * 128 / 100) >> 15; break;
        case 1: m_climb = (lfo_now * ld) >> 4; break;
        case 2: m_bright = (lfo_now * ld * 15) >> 15; break;
        case 3: m_width = (lfo_now * ld * 12) >> 15; break;
        case 4: trem = 32767 - ((ld * 327 / 2) * ((lfo_now + 32767) >> 1) >> 14); break;
        }
        c = climb + E.climb_inc + m_climb;
        if (c >= (S << 16) || c < 0) {
            c %= S << 16;
            if (c < 0) c += S << 16;
        }
        climb = c;
        for (i = 0; i < 32u; i++) bl[i] = br[i] = 0;
        for (vi = 0; vi < NV; vi++) {
            voice_t *v = &V[vi];
            env_tick(v);
            if (!v->stage && !v->env) {
                uint32_t k, any = 0;
                for (k = 0; k < NP; k++) { v->tL[k] = v->tR[k] = 0; any |= (uint32_t)(v->aL[k] | v->aR[k]); }
                if (!any) continue;
            } else
                voice_tick(v, c >> 16, S, R, inv_half, m_pitch, m_bright, m_width, trem);
            render_voice(v, bl, br);
        }
        for (i = 0; i < 32u; i++) {             /* the effects: echo and space side by side, both fed by the dry sound */
            int32_t L = bl[i], Rr = br[i], m, oL, oR;
            if (E.lp_k < 65000) {
                lpL += (int32_t)(((int64_t)(L - lpL) * E.lp_k) >> 16);
                lpR += (int32_t)(((int64_t)(Rr - lpR) * E.lp_k) >> 16);
                L = lpL; Rr = lpR;
            }
            {                                   /* high-pass, then low-pass: what echo and space (and the dry sound) hear */
                int32_t b, lo, k = E.hp_k;
                lo = svf(L, &hpL1, &hpL2, E.hp_a1, E.hp_a2, E.hp_a3, &b);
                L = L - (int32_t)(((int64_t)k * b) >> 24) - lo;
                lo = svf(Rr, &hpR1, &hpR2, E.hp_a1, E.hp_a2, E.hp_a3, &b);
                Rr = Rr - (int32_t)(((int64_t)k * b) >> 24) - lo;
                L = svf(L, &lfL1, &lfL2, E.lf_a1, E.lf_a2, E.lf_a3, &b);
                Rr = svf(Rr, &lfR1, &lfR2, E.lf_a1, E.lf_a2, E.lf_a3, &b);
            }
            m = (L + Rr) >> 1;
            oL = L; oR = Rr;
            if (E.echo_mix) {
                uint32_t rp = echo_pos + 44100u - (uint32_t)E.echo_len;
                int32_t e;
                if (rp >= 44100u) rp -= 44100u;
                e = echo_buf[rp];
                echo_buf[echo_pos] = (int16_t)clip16((m >> 1) + ((e * E.echo_fb) >> 15));
                if (++echo_pos >= 44100u) echo_pos = 0;
                oL += (e * E.echo_mix) >> 15;
                oR += (e * E.echo_mix) >> 15;
            }
            if (E.space) {
                int32_t in = m >> 5, fb = E.rev_fb, wl, wr;
                wl = comb(cb0, 1557, &ci0, &cf0, in, fb) + comb(cb2, 1491, &ci2, &cf2, in, fb);
                wr = comb(cb1, 1617, &ci1, &cf1, in, fb) + comb(cb3, 1422, &ci3, &cf3, in, fb);
                wl = allpass(ap1, 556, &ai1, allpass(ap0, 225, &ai0, wl));
                wr = allpass(ap3, 441, &ai3, allpass(ap2, 341, &ai2, wr));
                oL += (wl * E.space) >> 14;
                oR += (wr * E.space) >> 14;
            }
            bl[i] = oL; br[i] = oR;
        }
        {                                       /* a limiter: never clips, never crunches. It drops at once and comes back
                                                 * over most of a second, so it never follows a low note's waveform */
            int32_t pk = 0, g0 = lim_g, g1, dg;
            for (i = 0; i < 32u; i++) { int32_t a = iabs(bl[i]), b = iabs(br[i]); if (a > pk) pk = a; if (b > pk) pk = b; }
            g1 = pk > 26000 ? (int32_t)((26000u << 15) / (uint32_t)pk) : 32768;
            if (g1 > g0) g1 = g0 + ((g1 - g0) >> 10);  /* (lets go slowly: ~0.7 s) */
            else g0 = g1;                       /* (and takes hold at once, from the block's first sample) */
            lim_g = g1;
            dg = (g1 - g0) / 32;
            for (i = 0; i < 32u; i++) {
                int32_t g = g0 + dg * (int32_t)i;
                out[2u * (j + i)] += clip16((int32_t)(((int64_t)bl[i] * g) >> 15));
                out[2u * (j + i) + 1u] += clip16((int32_t)(((int64_t)br[i] * g) >> 15));
            }
        }
    }
    isr_us = (em_ticks() - t0) / 24u;
}

/* ================================================================ scales and the transpose (they act last) */
typedef struct { const char *name, *full; uint16_t mask; } scale_t;
#define SC(a, b, c, d, e, f, g, h, i, j, k, l) ((a) | (b) << 1 | (c) << 2 | (d) << 3 | (e) << 4 | (f) << 5 | (g) << 6 | (h) << 7 | (i) << 8 | (j) << 9 | (k) << 10 | (l) << 11)
static const scale_t SCALES[] = {           /* bit n: n semitones above the root */
    {"CHROM", "CHROMATIC", 0xFFF},
    {"MAJOR", "MAJOR", SC(1,0,1,0,1,1,0,1,0,1,0,1)},
    {"MINOR", "MINOR", SC(1,0,1,1,0,1,0,1,1,0,1,0)},
    {"DORIAN", "DORIAN", SC(1,0,1,1,0,1,0,1,0,1,1,0)},
    {"PHRYG", "PHRYGIAN", SC(1,1,0,1,0,1,0,1,1,0,1,0)},
    {"LYDIAN", "LYDIAN", SC(1,0,1,0,1,0,1,1,0,1,0,1)},
    {"MIXO", "MIXOLYDIAN", SC(1,0,1,0,1,1,0,1,0,1,1,0)},
    {"HARMMIN", "HARMONIC MINOR", SC(1,0,1,1,0,1,0,1,1,0,0,1)},
    {"PENTMAJ", "PENTATONIC MAJOR", SC(1,0,1,0,1,0,0,1,0,1,0,0)},
    {"PENTMIN", "PENTATONIC MINOR", SC(1,0,0,1,0,1,0,1,0,0,1,0)},
    {"BLUES", "BLUES", SC(1,0,0,1,0,1,1,1,0,0,1,0)},
    {"WHOLE", "WHOLE TONE", SC(1,0,1,0,1,0,1,0,1,0,1,0)},
    {"DIMIN", "DIMINISHED", SC(1,0,1,1,0,1,1,0,1,1,0,1)},
    {"HIJAZ", "HIJAZ", SC(1,1,0,0,1,1,0,1,1,0,1,0)},
    {"BHAIRAV", "BHAIRAV", SC(1,1,0,0,1,1,0,1,1,0,0,1)},
    {"PERSIAN", "PERSIAN", SC(1,1,0,0,1,1,1,0,1,0,0,1)},
    {"HUNGAR", "HUNGARIAN MINOR", SC(1,0,1,1,0,0,1,1,1,0,0,1)},
    {"HIRAJO", "HIRAJOSHI", SC(1,0,1,1,0,0,0,1,1,0,0,0)},
    {"IN SEN", "IN SEN", SC(1,1,0,0,0,1,0,1,0,0,1,0)},
    {"IWATO", "IWATO", SC(1,1,0,0,0,1,1,0,0,0,1,0)},
    {"KUMOI", "KUMOI", SC(1,0,1,1,0,0,0,1,0,1,0,0)},
    {"PELOG", "PELOG", SC(1,1,0,1,0,0,0,1,1,0,0,0)},
    {"EGYPT", "EGYPTIAN", SC(1,0,1,0,0,1,0,1,0,0,1,0)},
    {"PROMETH", "PROMETHEUS", SC(1,0,1,0,1,0,1,0,0,1,1,0)},
    {"ENIGMA", "ENIGMATIC", SC(1,1,0,0,1,0,1,0,1,0,1,1)},
    {"AUGMENT", "AUGMENTED", SC(1,0,0,1,1,0,0,1,1,0,0,1)},
};
#define NSCALE ((int16_t)(sizeof SCALES / sizeof SCALES[0]))
static const char *SCALE_SHORT[NSCALE];         /* (filled at boot, for the knob's names) */

static uint32_t in_scale(uint32_t note)
{
    uint32_t root = (uint32_t)(G.root + 24) % 12u;
    return SCALES[G.scale % NSCALE].mask >> ((note + 12u - root) % 12u) & 1u;
}
static uint32_t out_note(uint32_t raw)          /* what you hear: transposed, then the nearest note of the scale */
{
    int32_t n = (int32_t)raw + G.root, d;
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    for (d = 0; d < 7; d++) {
        if (n - d >= 0 && in_scale((uint32_t)(n - d))) return (uint32_t)(n - d);
        if (n + d <= 127 && in_scale((uint32_t)(n + d))) return (uint32_t)(n + d);
    }
    return (uint32_t)n;
}
static uint32_t scale_move(uint32_t note, int32_t dir)   /* the next note of the scale up (1) or down (-1) */
{
    uint32_t i;
    for (i = 0; i < 12u; i++) {
        note = (uint32_t)((int32_t)note + dir);
        if (note < 12u || note > 115u) return note < 12u ? 12u : 115u;
        if (in_scale(note)) return note;
    }
    return note;
}

/* ================================================================ notes */
static uint32_t ages, last_out = 255;
static void voice_on(uint32_t raw, uint32_t vel, uint32_t timed_ms)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu, note = out_note(raw);
    if (raw > 127u || !vel) return;
    for (v = 0; v < NV; v++) {                  /* the same note, else a quiet voice, else the oldest */
        uint32_t score = V[v].note == note && (V[v].gate || V[v].stage) ? 0u
                       : V[v].gate ? 0x80000000u + V[v].age : 1u + ((uint32_t)V[v].env >> 10);
        if (score < best) { best = score; pick = v; }
    }
    V[pick].note = (uint8_t)note;
    V[pick].raw = (uint8_t)raw;
    V[pick].vel = (uint8_t)(vel > 127u ? 127u : vel);
    V[pick].age = ++ages;
    V[pick].timed = timed_ms != 0;
    V[pick].off_at = fm1_ms + timed_ms;
    V[pick].gate = 1;
    V[pick].trig = 1;
    last_out = note;
}
static void voice_off(uint32_t raw)             /* the voices a key (or MIDI) started */
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (V[v].raw == raw && !V[v].timed) V[v].gate = 0;
}
static void voices_all_off(void)
{
    uint32_t v;
    for (v = 0; v < NV; v++) V[v].gate = 0;
}
static int quiet(void)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (V[v].gate || V[v].stage) return 0;
    return 1;
}

/* ================================================================ names on the screen */
static const char *const ALG_NAME[6] = {"OCTAVES", "FIFTHS", "FOURTHS", "TRITONES", "THIRDS", "STRETCHED"};
static const char *const SHAPE_NAME[6] = {"SINE", "TRI", "SQUARE", "SAW", "RANDOM", "DRIFT"};
static const char *const DEST_NAME[5] = {"PITCH", "CLIMB", "BRIGHT", "WIDTH", "VOLUME"};
static const char *const AMODE_NAME[6] = {"UP", "DOWN", "UPDOWN", "RANDOM", "PLAYED", "CHORD"};
static const char *const RATE_NAME[6] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const uint8_t RATE_T[6] = {24, 12, 8, 6, 4, 3};
static const char *const ETIME_NAME[7] = {"1/16", "1/8T", "1/8", "3/16", "1/4", "3/8", "1/2"};
static const uint8_t ETIME_T[7] = {6, 8, 12, 18, 24, 36, 48};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const CH_NAME[17] = {"OMNI", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
                                        "14", "15", "16"};
#define REPS_ENDLESS 17

/* ================================================================ the factory sounds */
static const patch_t FACTORY[NPRESET] = {
    /* alg climb bright width blur | a d s r | ta td ts tamt | lrate depth shape dest | echo etime space tone |
       arp mode rate oct gate | move repeats rate gate | level name */
    {0, 18, 0, 55, 12, 35, 60, 80, 62, 40, 60, 30, 10, 20, 0, 0, 2, 22, 4, 45, 85, 0, 0, 3, 1, 60, 1, REPS_ENDLESS, 1, 70, 90, "ENDLESS", 0, 0, 100, 0},
    {0, -30, 6, 45, 20, 20, 55, 70, 58, 30, 55, 20, 12, 25, 10, 5, 2, 30, 3, 40, 80, 0, 0, 3, 1, 60, -1, REPS_ENDLESS, 1, 80, 90, "DESCENT", 0, 0, 100, 0},
    {0, 0, -6, 70, 6, 15, 40, 100, 45, 30, 50, 40, 0, 45, 6, 0, 0, 0, 2, 25, 70, 0, 0, 3, 1, 60, 2, 4, 0, 90, 95, "ORGAN", 0, 0, 100, 0},
    {1, 10, 12, 50, 30, 45, 65, 75, 70, 60, 70, 20, 8, 15, 30, 5, 2, 25, 4, 60, 75, 0, 0, 3, 1, 60, 1, 8, 1, 60, 90, "FIFTHS", 0, 0, 100, 0},
    {3, -55, 0, 35, 45, 5, 50, 30, 50, 5, 45, 0, 24, 55, 40, 2, 2, 35, 3, 30, 65, 1, 2, 3, 2, 50, -2, REPS_ENDLESS, 3, 50, 85, "TRITONE", 0, 0, 100, 0},
    {5, 6, 24, 22, 15, 2, 45, 0, 60, 2, 40, 0, -18, 30, 20, 0, 0, 30, 5, 55, 95, 0, 0, 3, 1, 60, 3, 6, 3, 40, 90, "GLASS", 0, 0, 100, 0},
    {0, 25, 10, 50, 10, 2, 35, 40, 40, 2, 30, 10, 20, 62, 85, 2, 4, 20, 2, 30, 80, 1, 0, 3, 2, 45, 1, REPS_ENDLESS, 3, 35, 90, "PULSE", 0, 0, 100, 0},
    {4, 3, -10, 60, 85, 60, 70, 85, 80, 70, 80, 40, 0, 10, 60, 5, 2, 15, 6, 80, 60, 0, 0, 3, 1, 60, -1, 12, 0, 100, 90, "DUST", 0, 0, 100, 0},
};
static const uint8_t FACTORY_PH[NPRESET][MAXPH + 1] = {     /* n, notes: a phrase for every sound */
    {4, 57, 60, 64, 67}, {4, 69, 67, 64, 60}, {3, 48, 55, 60}, {5, 60, 62, 67, 69, 72},
    {3, 60, 66, 63}, {6, 72, 76, 79, 83, 79, 76}, {4, 60, 60, 67, 63}, {2, 57, 64},
};

/* ================================================================ the clock, the staircase, the arp */
static uint32_t ext_last_ms, ext_seen;          /* an external MIDI clock */
static uint32_t clk_acc, clk_last;              /* the internal one (24 a quarter) */
static uint32_t t24, beat_ms;                   /* free ticks (the arp) */
static uint32_t running, rec, rec_fresh;
static uint32_t st_t, st_idx, st_rep;           /* the staircase: its own ticks, the note, the repeat */
static int32_t st_off;                          /* how far it has climbed, in semitones (wraps at an octave) */
static uint32_t st_lit_ms, st_last = 255;
static uint32_t rnd_s = 0x9E3779B9u;

static uint32_t step_ms(void) { return 15000u / (uint32_t)(G.tempo < 20 ? 20 : G.tempo); }

static void stair_step(void)
{
    uint32_t gate;
    if (!ph_n) return;
    if (st_idx >= ph_n) st_idx = 0;
    gate = step_ms() * RATE_T[P.srate % 6] / 6u * (uint32_t)P.sgate / 100u;
    if (gate < 10u) gate = 10u;
    st_last = (uint32_t)((int32_t)ph_note[st_idx] + st_off);
    if (st_last <= 127u) voice_on(st_last, 100, gate);
    st_lit_ms = fm1_ms;
    if (++st_idx >= ph_n) {                     /* the phrase is done: the next repeat, a step higher (or lower) */
        st_idx = 0;
        st_rep++;
        if (P.sreps != REPS_ENDLESS && st_rep >= (uint32_t)P.sreps) { st_rep = 0; st_off = 0; }
        else if (P.smove) {
            uint32_t n = (uint32_t)((int32_t)ph_note[0] + st_off), k;
            for (k = 0; k < (uint32_t)iabs(P.smove); k++) n = scale_move(n, P.smove > 0 ? 1 : -1);
            st_off = (int32_t)n - ph_note[0];
            if (st_off >= 12) st_off -= 12;     /* (an octave on: the endless sound hides the jump) */
            if (st_off <= -12) st_off += 12;
        }
    }
}

/* the notes held (keys, MIDI) and frozen: what the arp plays */
static uint8_t held[16], held_n, frozen[16], frozen_n, freeze;
static uint32_t arp_i, arp_dir = 1;
static uint8_t arp_notes[64];

static uint32_t arp_build(void)
{
    uint32_t n = 0, i, j, o, base_n = 0;
    uint8_t base[32];
    for (i = 0; i < held_n; i++) base[base_n++] = held[i];
    for (i = 0; i < frozen_n; i++) {
        for (j = 0; j < base_n && base[j] != frozen[i]; j++) {}
        if (j == base_n && base_n < 32u) base[base_n++] = frozen[i];
    }
    if (P.amode != 4)                           /* (PLAYED keeps the order) */
        for (i = 1; i < base_n; i++)
            for (j = i; j && base[j - 1u] > base[j]; j--) { uint8_t t = base[j]; base[j] = base[j - 1u]; base[j - 1u] = t; }
    for (o = 0; o < (uint32_t)P.aoct && o < 4u; o++)
        for (i = 0; i < base_n && n < 64u; i++)
            if (base[i] + 12u * o < 128u) arp_notes[n++] = (uint8_t)(base[i] + 12u * o);
    return n;
}

static void arp_step(void)
{
    uint32_t n = arp_build(), gate = step_ms() * RATE_T[P.arate % 6] / 6u * (uint32_t)P.agate / 100u, i;
    if (!n) { arp_i = 0; return; }
    if (gate < 8u) gate = 8u;
    switch (P.amode) {
    case 1: arp_i = arp_i ? arp_i - 1u : n - 1u; if (arp_i >= n) arp_i = n - 1u; break;
    case 2:
        if (n == 1u) { arp_i = 0; break; }
        if (arp_dir) { if (++arp_i >= n - 1u) { arp_i = n - 1u; arp_dir = 0; } }
        else if (arp_i == 0u || --arp_i == 0u) { arp_i = 0; arp_dir = 1; }
        break;
    case 3: arp_i = rnd32(&rnd_s) % n; break;
    case 5: for (i = 0; i < n && i < NV; i++) voice_on(arp_notes[i], 100, gate); return;
    default: if (++arp_i >= n) arp_i = 0; break;
    }
    voice_on(arp_notes[arp_i % n], 100, gate);
}

static void on_tick(void)                       /* 24 a quarter note, from the internal or a MIDI clock */
{
    t24++;
    if (P.arp && t24 % RATE_T[P.arate % 6] == 0u) arp_step();
    if (t24 % 24u == 0u) beat_ms = fm1_ms;
    if (!running) return;
    if (st_t % RATE_T[P.srate % 6] == 0u) stair_step();
    st_t++;
}

static void clock_service(void)                 /* from the main loop and while the screen draws */
{
    uint32_t now = em_ticks(), el = now - clk_last, per;
    clk_last = now;
    if (ext_seen && fm1_ms - ext_last_ms < 400u) return;   /* a MIDI clock leads */
    ext_seen = 0;
    per = (2500000u / (uint32_t)(G.tempo < 20 ? 20 : G.tempo)) * 24u;   /* ticks of 24 MHz a clock */
    clk_acc += el;
    if (clk_acc > per * 8u) clk_acc = per;      /* (after a long stall: do not race) */
    while (clk_acc >= per) { clk_acc -= per; on_tick(); }
}

static void transport(uint32_t on)
{
    if (on && !running) { st_t = 0; st_idx = 0; st_rep = 0; st_off = 0; running = 1; clk_acc = 0; on_tick(); }
    else if (!on && running) {
        uint32_t v;
        running = 0;
        for (v = 0; v < NV; v++) if (V[v].timed) V[v].gate = 0;
    }
}

/* playing a note: live (or into the arp), and into the phrase while REC is on */
static void note_in(uint32_t note, uint32_t vel)
{
    uint32_t i;
    if (note > 127u) return;
    if (held_n < 16u) { for (i = 0; i < held_n && held[i] != note; i++) {} if (i == held_n) held[held_n++] = (uint8_t)note; }
    if (!P.arp) voice_on(note, vel, 0);
    if (rec) {
        if (rec_fresh) { ph_n = 0; rec_fresh = 0; st_idx = 0; st_rep = 0; st_off = 0; }
        if (ph_n < MAXPH) ph_note[ph_n++] = (uint8_t)note;
        if (ph_n >= MAXPH) rec = 0;             /* full */
    }
}
static void note_out(uint32_t note)
{
    uint32_t i;
    if (note > 127u) return;
    for (i = 0; i < held_n; i++)
        if (held[i] == note) { held[i] = held[--held_n]; break; }
    if (freeze) {
        for (i = 0; i < frozen_n && frozen[i] != note; i++) {}
        if (i == frozen_n && frozen_n < 16u) frozen[frozen_n++] = (uint8_t)note;
    } else if (!P.arp)
        voice_off(note);
}
static void freeze_set(uint32_t on)
{
    uint32_t i, j;
    freeze = (uint8_t)on;
    if (on) {
        for (i = 0; i < held_n && frozen_n < 16u; i++) frozen[frozen_n++] = held[i];
        return;
    }
    for (i = 0; i < frozen_n; i++) {            /* let go of what is not held */
        for (j = 0; j < held_n && held[j] != frozen[i]; j++) {}
        if (j == held_n && !P.arp) voice_off(frozen[i]);
    }
    frozen_n = 0;
}

/* ================================================================ the patch into the engine */
/* the filter's coefficients (Q24) from frequency 0..100 and resonance 0..100: Q 0.7 .. 4, returns the damping k */
static int32_t svf_coef(int32_t f, int32_t res, int32_t *a1, int32_t *a2, int32_t *a3)
{
    int32_t g = SVF_G[(uint32_t)f % 101u];
    int32_t k = (1414 - (res < 0 ? 0 : res > 100 ? 100 : res) * 1164 / 100) * 16777;   /* 1.414 .. 0.25 */
    int64_t den = (1LL << 24) + (((int64_t)g * (g + k)) >> 24);
    *a1 = (int32_t)(((1u << 31) / (uint32_t)(den >> 8)) << 9);
    *a2 = (int32_t)(((int64_t)g * *a1) >> 24);
    *a3 = (int32_t)(((int64_t)g * *a2) >> 24);
    return k;
}
static void engine_set(void)
{
    int32_t c = P.climb;
    E.S = SPACING[P.alg % 6];
    E.climb_inc = c * iabs(c) * 73 / 10;        /* 100: 2 octaves a second */
    E.bright = P.bright * 64;
    E.width = 128 + P.width * 40;               /* 2 .. 64 semitones */
    E.blur = P.blur;
    E.att = ATT_INC[P.a % 101]; E.dec = EXP_COEF[P.d % 101]; E.sus = (uint32_t)P.s * 167772u; E.rel = EXP_COEF[P.r % 101];
    E.tatt = ATT_INC[P.ta % 101]; E.tdec = EXP_COEF[P.td % 101]; E.tsus = (uint32_t)P.ts * 167772u;
    E.tamt = P.tamt * 64;
    E.lfo_inc = LFO_INC[P.lrate % 101]; E.lfo_depth = P.ldepth; E.lfo_shape = P.lshape; E.lfo_dest = P.ldest;
    E.echo_mix = P.echo * 200;
    E.echo_fb = P.echo * 220;
    {
        uint32_t fr = 2500u * ETIME_T[P.etime % 7] / (uint32_t)(G.tempo < 20 ? 20 : G.tempo) * 441u / 10u;
        E.echo_len = (int32_t)(fr > 44000u ? 44000u : fr < 64u ? 64u : fr);
    }
    E.rev_fb = 25000 + P.space * 55;
    E.space = P.space * 260;
    E.lp_k = LP_COEF[P.tone % 101];
    {
        int32_t a1, a2, a3, k;
        k = svf_coef(P.hpf, P.hpq, &a1, &a2, &a3);
        E.hp_a1 = a1; E.hp_a2 = a2; E.hp_a3 = a3; E.hp_k = k;
        svf_coef(P.lpf, P.lpq, &a1, &a2, &a3);
        E.lf_a1 = a1; E.lf_a2 = a2; E.lf_a3 = a3;
    }
    E.level = P.level;
}

/* ================================================================ storage */
static preset_t pbuf;
static void preset_factory(uint32_t i)
{
    uint32_t k;
    P = FACTORY[i % NPRESET];
    ph_n = FACTORY_PH[i % NPRESET][0];
    for (k = 0; k < ph_n; k++) ph_note[k] = FACTORY_PH[i % NPRESET][k + 1u];
}
static void preset_load(uint32_t i)
{
    uint32_t k;
    if (!blob_load(i, &pbuf, sizeof pbuf) && pbuf.ver == PRESET_VER && pbuf.n <= MAXPH) {
        P = pbuf.p;
        ph_n = pbuf.n;
        for (k = 0; k < ph_n; k++) ph_note[k] = pbuf.note[k];
    } else
        preset_factory(i);
    st_idx = 0; st_rep = 0; st_off = 0;
    engine_set();
}
static void preset_save(uint32_t i)
{
    uint32_t k;
    pbuf.ver = PRESET_VER;
    pbuf.p = P;
    pbuf.n = ph_n;
    for (k = 0; k < MAXPH; k++) pbuf.note[k] = k < ph_n ? ph_note[k] : 0;
    blob_save(i, &pbuf, sizeof pbuf);
}

/* ================================================================ the pages */
enum { F_NUM, F_BI, F_NAMES, F_TIME, F_PCT, F_SEMI, F_HZ, F_TEMPO, F_ROOT, F_REPS, F_MOVE, F_CUT };
typedef struct { const char *label; int16_t *v; int16_t lo, hi; uint8_t fmt; const char *const *names; } param_t;
typedef struct { const char *title; uint8_t icon; param_t k[4]; } page_t;
enum { PG_ENGINE, PG_AMP, PG_TONE, PG_LFO, PG_FX, PG_ARP, PG_SEQ, PG_GLO, PG_STAGE, NPAGE };
enum { IC_ENGINE, IC_AMP, IC_TONE, IC_LFO, IC_FX, IC_ARP, IC_SEQ, IC_GLO, IC_STAGE, IC_PLAY, IC_REC, IC_FREEZE };

static const page_t PAGES[NPAGE] = {
    {"ENGINE", IC_ENGINE, {{"CLIMB", &P.climb, -100, 100, F_BI, 0}, {"BRIGHT", &P.bright, -36, 36, F_SEMI, 0},
                           {"WIDTH", &P.width, 0, 100, F_NUM, 0}, {"BLUR", &P.blur, 0, 100, F_NUM, 0}}},
    {"VOLUME ENVELOPE", IC_AMP, {{"ATTACK", &P.a, 0, 100, F_TIME, 0}, {"DECAY", &P.d, 0, 100, F_TIME, 0},
                                 {"SUSTAIN", &P.s, 0, 100, F_PCT, 0}, {"RELEASE", &P.r, 0, 100, F_TIME, 0}}},
    {"BRIGHT ENVELOPE", IC_TONE, {{"ATTACK", &P.ta, 0, 100, F_TIME, 0}, {"DECAY", &P.td, 0, 100, F_TIME, 0},
                                  {"SUSTAIN", &P.ts, 0, 100, F_PCT, 0}, {"AMOUNT", &P.tamt, -36, 36, F_SEMI, 0}}},
    {"LFO", IC_LFO, {{"RATE", &P.lrate, 0, 100, F_HZ, 0}, {"DEPTH", &P.ldepth, 0, 100, F_NUM, 0},
                     {"SHAPE", &P.lshape, 0, 5, F_NAMES, SHAPE_NAME}, {"MOVES", &P.ldest, 0, 4, F_NAMES, DEST_NAME}}},
    {"EFFECTS", IC_FX, {{"ECHO", &P.echo, 0, 100, F_NUM, 0}, {"ECHO TIME", &P.etime, 0, 6, F_NAMES, ETIME_NAME},
                        {"SPACE", &P.space, 0, 100, F_NUM, 0}, {"TONE", &P.tone, 0, 100, F_NUM, 0}}},
    {"ARPEGGIO", IC_ARP, {{"MODE", &P.amode, 0, 5, F_NAMES, AMODE_NAME}, {"RATE", &P.arate, 0, 5, F_NAMES, RATE_NAME},
                          {"OCTAVES", &P.aoct, 1, 4, F_NUM, 0}, {"GATE", &P.agate, 5, 100, F_PCT, 0}}},
    {"STAIRCASE", IC_SEQ, {{"MOVE", &P.smove, -7, 7, F_MOVE, 0}, {"REPEATS", &P.sreps, 1, REPS_ENDLESS, F_REPS, 0},
                           {"RATE", &P.srate, 0, 5, F_NAMES, RATE_NAME}, {"GATE", &P.sgate, 5, 100, F_PCT, 0}}},
    {"GLOBAL", IC_GLO, {{"TEMPO", &G.tempo, 40, 240, F_TEMPO, 0}, {"SCALE", &G.scale, 0, NSCALE - 1, F_NAMES, SCALE_SHORT},
                        {"ROOT", &G.root, -12, 12, F_ROOT, 0}, {"MIDI CHANNEL", &G.ch, 0, 16, F_NAMES, CH_NAME}}},
    {"HOME", IC_STAGE, {{"HP FREQ", &P.hpf, 0, 100, F_CUT, 0}, {"HP RES", &P.hpq, 0, 100, F_NUM, 0},
                         {"LP FREQ", &P.lpf, 0, 100, F_CUT, 0}, {"LP RES", &P.lpq, 0, 100, F_NUM, 0}}},
};

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *put_int(char *p, int32_t v, int sign)
{
    char d[8];
    uint32_t a = (uint32_t)(v < 0 ? -v : v), n = 0;
    if (sign && v) *p++ = v < 0 ? '-' : '+';
    else if (v < 0) *p++ = '-';
    do { d[n++] = (char)('0' + a % 10u); a /= 10u; } while (a);
    while (n) *p++ = d[--n];
    *p = 0;
    return p;
}
static void fmt_value(const param_t *p, char *s)
{
    int32_t v = *p->v;
    uint32_t u;
    char *q = s;
    *q = 0;
    switch (p->fmt) {
    case F_NAMES: put(q, p->names[v - p->lo]); break;
    case F_TIME:
        u = ENV_MS[v % 101];
        if (u < 1000u) { q = put_int(q, (int32_t)u, 0); put(q, " MS"); }
        else { u = (u + 50u) / 100u; q = put_int(q, (int32_t)(u / 10u), 0); *q++ = '.'; q = put_int(q, (int32_t)(u % 10u), 0); put(q, " S"); }
        break;
    case F_HZ:
        u = LFO_CHZ[v % 101];
        q = put_int(q, (int32_t)(u / 100u), 0); *q++ = '.';
        *q++ = (char)('0' + u / 10u % 10u); *q++ = (char)('0' + u % 10u); put(q, " HZ");
        break;
    case F_CUT:
        u = CUT_HZ[v % 101];
        if (u < 1000u) { q = put_int(q, (int32_t)u, 0); put(q, " HZ"); }
        else { u = (u + 50u) / 100u; q = put_int(q, (int32_t)(u / 10u), 0); *q++ = '.'; q = put_int(q, (int32_t)(u % 10u), 0); put(q, " KHZ"); }
        break;
    case F_ROOT: q = put(q, NOTE_NAME[(v + 24) % 12]); if (v) { *q++ = ' '; put_int(q, v, 1); } break;
    case F_REPS: if (v >= REPS_ENDLESS) put(q, "ENDLESS"); else put_int(q, v, 0); break;
    case F_MOVE: if (!v) put(q, "STAY"); else { q = put_int(q, v, 1); put(q, v > 0 ? " UP" : " DOWN"); } break;
    case F_PCT: q = put_int(q, v, 0); put(q, "%"); break;
    case F_SEMI: q = put_int(q, v, 1); if (v) put(q, " ST"); break;
    case F_BI: put_int(q, v, 1); break;
    default: put_int(q, v, 0); break;
    }
}

/* ================================================================ looks: three calm themes */
typedef struct { const char *name; uint16_t bg, fg, dim, line, acc; uint8_t bar; } theme_t;
static const theme_t THEMES[3] = {
    {"MONO", RGB(0, 0, 0), RGB(232, 232, 228), RGB(118, 118, 114), RGB(40, 40, 40), RGB(255, 92, 40), 0},
    {"PAPER", RGB(238, 235, 227), RGB(20, 20, 20), RGB(128, 124, 116), RGB(206, 201, 190), RGB(214, 40, 30), 0},
    {"GRID", RGB(8, 8, 8), RGB(240, 240, 240), RGB(112, 112, 112), RGB(36, 36, 36), RGB(228, 255, 30), 1},
};
static const theme_t *T = &THEMES[EM_THEME % 3];

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)   /* a .. b, t 0..256 */
{
    uint32_t ra = a >> 11, ga = (a >> 5) & 63u, ba = a & 31u, rb = b >> 11, gb = (b >> 5) & 63u, bb = b & 31u;
    uint32_t r = (ra * (256u - t) + rb * t) >> 8, g = (ga * (256u - t) + gb * t) >> 8, bl = (ba * (256u - t) + bb * t) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static uint16_t tx_bg;                          /* what text is blended onto */
static int32_t tx(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    uint16_t ramp[16];
    uint32_t a;
    for (a = 0; a < 16u; a++) ramp[a] = mix565(tx_bg, c, a * 256u / 15u);
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v) cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, ramp[v]);
            }
        x += f->adv[gi];
    }
    return x;
}
static void tx_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c) { tx(cx - text_w(f, s) / 2, y, f, s, c); }
static void tx_r(int32_t rx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c) { tx(rx - text_w(f, s), y, f, s, c); }

/* 12 x 12 icons, drawn in one colour (bit 11 = left) */
static const uint16_t ICON[12][12] = {
    {0x000, 0x00E, 0x00A, 0x03A, 0x022, 0x0E2, 0x082, 0x382, 0x202, 0x202, 0xFFE, 0x000},   /* ENGINE: stairs */
    {0x000, 0x100, 0x180, 0x240, 0x23E, 0x402, 0x402, 0x801, 0x801, 0x801, 0xFFF, 0x000},   /* VOLUME ENV */
    {0x040, 0x444, 0x248, 0x0E0, 0x1F0, 0xFFE, 0x1F0, 0x0E0, 0x248, 0x444, 0x040, 0x000},   /* BRIGHT ENV: a sun */
    {0x000, 0x000, 0x600, 0x900, 0x901, 0x081, 0x081, 0x042, 0x03C, 0x000, 0x000, 0x000},   /* LFO */
    {0x000, 0x000, 0x800, 0x840, 0x844, 0x844, 0x844, 0x844, 0x840, 0x800, 0x000, 0x000},   /* EFFECTS */
    {0x000, 0x007, 0x007, 0x000, 0x070, 0x070, 0x000, 0x700, 0x700, 0x000, 0x000, 0x000},   /* ARPEGGIO */
    {0x000, 0x003, 0x003, 0x01B, 0x01B, 0x0DB, 0x0DB, 0x6DB, 0x6DB, 0x6DB, 0x000, 0x000},   /* STAIRCASE */
    {0x0F0, 0x36C, 0x462, 0x462, 0xFFF, 0x462, 0x462, 0xFFF, 0x462, 0x462, 0x36C, 0x0F0},   /* GLOBAL */
    {0x060, 0x090, 0x108, 0x204, 0x402, 0xE07, 0x204, 0x264, 0x264, 0x264, 0x3FC, 0x000},   /* HOME: a house */
    {0x000, 0x000, 0x100, 0x1C0, 0x1F0, 0x1FC, 0x1F0, 0x1C0, 0x100, 0x000, 0x000, 0x000},   /* PLAY */
    {0x000, 0x000, 0x0F0, 0x1F8, 0x3FC, 0x3FC, 0x3FC, 0x3FC, 0x1F8, 0x0F0, 0x000, 0x000},   /* REC */
    {0x040, 0x0E0, 0x444, 0x248, 0x150, 0xFFE, 0x150, 0x248, 0x444, 0x0E0, 0x040, 0x000},   /* FREEZE */
};
static void icon(int32_t x, int32_t y, uint32_t i, uint16_t c)
{
    uint32_t r, b;
    for (r = 0; r < 12u; r++)
        for (b = 0; b < 12u; b++)
            if (ICON[i][r] >> (11u - b) & 1u) cv_pset(x + (int32_t)b, y + (int32_t)r, c);
}

static int32_t isin(uint32_t a) { return SINE[a & 1023u]; }
static int32_t icos(uint32_t a) { return SINE[(a + 256u) & 1023u]; }
static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) cv_pset(cx + x, cy + y, c);
}
static void circle(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    uint32_t a;
    for (a = 0; a < 1024u; a += 2u) cv_pset(cx + (isin(a) * r >> 15), cy - (icos(a) * r >> 15), c);
}

/* ================================================================ the screen: a title, a picture, four lines */
#define MAIN_Y 32
#define MAIN_H 132
#define FOOT_Y 166
static uint32_t page = PG_STAGE, page_env = PG_AMP, touched = 9, touched_ms;
static char toast[24];
static uint32_t toast_ms;

static void say(const char *s)
{
    uint32_t i;
    for (i = 0; s[i] && i < 23u; i++) toast[i] = s[i];
    toast[i] = 0;
    toast_ms = fm1_ms;
}

static void draw_header(void)
{
    char s[4];
    int32_t x = 230;
    const page_t *pg = &PAGES[page];
    uint16_t bg = T->bar ? T->acc : T->bg, fg = T->bar ? T->bg : T->fg, dim = T->bar ? T->bg : T->dim;
    int toasting = toast[0] && fm1_ms - toast_ms < 1400u;
    cv_begin(240, MAIN_Y, T->bg);
    tx_bg = bg;
    if (T->bar) cv_rect(0, 0, 240, 26, bg);
    icon(10, 7, pg->icon, fg);
    tx(30, 5, &FONT_S, toasting ? toast : pg->title, toasting && !T->bar ? T->acc : fg);
    s[0] = 'P'; s[1] = (char)('1' + G.preset); s[2] = 0;
    tx_r(x, 5, &FONT_S, s, dim);
    x -= 26;
    if (freeze) { icon(x - 12, 7, IC_FREEZE, fg); x -= 18; }
    if (rec) { icon(x - 12, 7, IC_REC, T->bar ? fg : T->acc); x -= 18; }
    if (running) { icon(x - 12, 7, IC_PLAY, fg); x -= 18; }
    if (P.arp) tx_r(x, 5, &FONT_S, "ARP", dim);
    if (!T->bar) cv_rect(10, 27, 220, 1, T->line);
    cv_blit(0, 0);
    tx_bg = T->bg;
}

static void draw_footer(void)
{
    const page_t *pg = &PAGES[page];
    uint32_t i;
    cv_begin(240, 240 - FOOT_Y, T->bg);
    tx_bg = T->bg;
    cv_rect(10, 0, 220, 1, T->line);
    for (i = 0; i < 4u; i++) {                  /* one line a knob, in the knobs' order */
        const param_t *p = &pg->k[i];
        char s[16], n[3] = {(char)('1' + i), 0, 0};
        int32_t y = 4 + (int32_t)i * 18;
        int hot = touched == i && fm1_ms - touched_ms < 1500u;
        if (T->bar) { n[0] = '0'; n[1] = (char)('1' + i); }
        tx(10, y, &FONT_S, n, T->dim);
        tx(T->bar ? 36 : 28, y, &FONT_S, p->label, hot ? T->fg : T->dim);
        fmt_value(p, s);
        tx_r(230, y, &FONT_S, s, hot ? T->acc : T->fg);
    }
    cv_blit(0, FOOT_Y);
}

/* ---- the pictures (a canvas of 240 x 132): drawn once, redrawn when something changes */
static int32_t level_at(int32_t x, int32_t R)   /* a partial's level at x (no envelope) */
{
    int32_t d = x - R / 2, e, b, ie, ib;
    ie = iabs(d) * 256 / (R / 2);
    e = ie >= 256 ? 0 : BELL[ie];
    ib = iabs(d - E.bright) * 256 / E.width;
    b = ib >= 256 ? 0 : BELL[ib];
    return (e * b) >> 15;
}

static void pic_engine(void)
{
    int32_t S = E.S, R = S * (int32_t)NP, c = climb >> 16, k, x, h, base = 104;
    for (x = 0; x < 220; x += 3) {              /* the bell: where the sound is bright */
        h = level_at(x * R / 220, R) * 84 >> 15;
        cv_pset(10 + x, base - h, T->dim);
    }
    cv_rect(10, base + 1, 220, 1, T->line);
    for (k = 0; k < (int32_t)NP; k++) {         /* the partials, where they are now */
        x = k * S + c;
        if (x >= R) x -= R;
        h = level_at(x, R) * 84 >> 15;
        if (h > 0) cv_rect(10 + x * 220 / R, base - h, 1, h, T->fg);
    }
    tx(10, 114, &FONT_S, ALG_NAME[P.alg % 6], T->dim);
    tx_r(230, 114, &FONT_S, P.climb > 0 ? "RISING" : P.climb < 0 ? "FALLING" : "STILL", T->dim);
}

static void pic_env(int tone)
{
    int32_t a = tone ? P.ta : P.a, d = tone ? P.td : P.d, s = tone ? P.ts : P.s, r = P.r;
    int32_t x0 = 10, top = 12, bot = 110, h = bot - top, xa, xd, xs, xr, ys;
    xa = x0 + 4 + a * 52 / 100;
    xd = xa + 4 + d * 52 / 100;
    xs = xd + 36;
    xr = xs + 4 + r * 52 / 100;
    if (xr > 230) xr = 230;
    ys = bot - s * h / 100;
    cv_rect(x0, bot + 1, 220, 1, T->line);
    cv_line(x0, bot, xa, top, T->fg);
    cv_line(xa, top, xd, ys, T->fg);
    cv_line(xd, ys, xs, ys, T->fg);
    cv_line(xs, ys, xr, bot, T->fg);
    if (tone) tx(10, 114, &FONT_S, P.tamt >= 0 ? "OPENS UP" : "CLOSES DOWN", T->dim);
}

static void pic_lfo(void)
{
    int32_t x, y, py = 0, mid = 60, amp = 10 + P.ldepth * 40 / 100;
    for (x = 0; x < 220; x++) {
        uint32_t ph = (uint32_t)x * (0xFFFFFFFFu / 110u);
        int32_t v = P.lshape >= 4 ? (int32_t)(((uint32_t)x / 22u * 2654435761u) >> 16 & 0xFFFFu) - 32768 : lfo_shape_at(ph, P.lshape);
        if (P.lshape == 5 && x % 22) {          /* drift: glide between the random points */
            int32_t a = (int32_t)(((uint32_t)x / 22u * 2654435761u) >> 16 & 0xFFFFu) - 32768;
            int32_t b = (int32_t)(((uint32_t)(x / 22 + 1) * 2654435761u) >> 16 & 0xFFFFu) - 32768;
            v = a + (b - a) * (x % 22) / 22;
        }
        y = mid - (v * amp >> 15);
        if (x) cv_line(10 + x - 1, py, 10 + x, y, T->fg);
        py = y;
    }
    cv_rect(10, mid, 220, 1, T->line);
    tx(10, 114, &FONT_S, "MOVES", T->dim);
    tx(64, 114, &FONT_S, DEST_NAME[P.ldest % 5], T->fg);
}

static void pic_fx(void)                        /* the sound splits: echo and space side by side, then together */
{
    int32_t y1 = 34, y2 = 78, w;
    cv_rect(10, 56, 30, 1, T->dim);
    cv_rect(40, y1, 1, y2 - y1 + 1, T->dim);
    cv_rect(200, y1, 1, y2 - y1 + 1, T->dim);
    cv_rect(200, 56, 30, 1, T->dim);
    tx(52, y1 - 20, &FONT_S, "ECHO", T->fg);
    tx(52, y2 - 20, &FONT_S, "SPACE", T->fg);
    cv_rect(40, y1, 160, 1, T->line);
    cv_rect(40, y2, 160, 1, T->line);
    w = P.echo * 160 / 100; if (w) cv_rect(40, y1 - 1, w, 3, T->fg);
    w = P.space * 160 / 100; if (w) cv_rect(40, y2 - 1, w, 3, T->fg);
    tx(10, 114, &FONT_S, "TONE", T->dim);
    tx_r(230, 114, &FONT_S, P.tone >= 95 ? "OPEN" : P.tone >= 60 ? "SOFT" : "DARK", T->dim);
}

static void pic_arp(void)
{
    uint32_t n = arp_build(), i;
    if (!P.arp) { tx_c(120, 44, &FONT_L, "OFF", T->dim); tx_c(120, 90, &FONT_S, "PRESS ARP TO START", T->dim); return; }
    if (!n) { tx_c(120, 52, &FONT_S, "HOLD SOME KEYS", T->dim); return; }
    {
        uint32_t lo = 127, hi = 0, w = 220u / n;
        if (w > 24u) w = 24u;
        for (i = 0; i < n; i++) { if (arp_notes[i] < lo) lo = arp_notes[i]; if (arp_notes[i] > hi) hi = arp_notes[i]; }
        for (i = 0; i < n; i++) {
            int32_t y = hi > lo ? 100 - (int32_t)(arp_notes[i] - lo) * 84 / (int32_t)(hi - lo) : 60;
            cv_rect(10 + (int32_t)(i * w), y, (int32_t)w - 4, 4, T->fg);
        }
    }
}

static void note_text(char *p, uint32_t n)
{
    p = put(p, NOTE_NAME[n % 12u]);
    put_int(p, (int32_t)(n / 12u) - 1, 0);
}

static void pic_stairs(void)                    /* the phrase as a step, its repeats as a staircase */
{
    uint32_t i, reps = P.sreps >= REPS_ENDLESS ? 6u : (uint32_t)P.sreps, cur;
    int32_t bw, x0 = 10, dir = P.smove > 0 ? 1 : P.smove < 0 ? -1 : 0, rise;
    char s[48], *q = s;
    if (!ph_n) {
        tx_c(120, 36, &FONT_S, "NO PHRASE YET", T->fg);
        tx_c(120, 60, &FONT_S, "PRESS REC, PLAY 2 TO 8", T->dim);
        tx_c(120, 78, &FONT_S, "NOTES, THEN PRESS PLAY", T->dim);
        return;
    }
    if (reps > 6u) reps = 6u;
    if (reps < 1u) reps = 1u;
    bw = 220 / (int32_t)reps;
    rise = dir ? 72 / (int32_t)reps : 0;
    cur = running ? st_rep % reps : 99u;
    for (i = 0; i < reps; i++) {
        int32_t y = dir >= 0 ? 92 - (int32_t)i * rise : 20 + (int32_t)i * rise;
        uint32_t k;
        for (k = 0; k < ph_n; k++) {            /* the phrase, small, in each step */
            int32_t nx = x0 + (int32_t)i * bw + 2 + (int32_t)k * (bw - 6) / (int32_t)ph_n;
            int32_t ny = y - 2 - ((int32_t)ph_note[k] % 12) / 2;
            cv_rect(nx, ny, (bw - 6) / (int32_t)ph_n - 1 > 1 ? (bw - 6) / (int32_t)ph_n - 1 : 1, 2, i == cur ? T->acc : T->dim);
        }
        cv_rect(x0 + (int32_t)i * bw, y + 4, bw - 2, 1, i == cur ? T->acc : T->fg);
    }
    if (P.sreps >= REPS_ENDLESS) tx_r(230, dir >= 0 ? 4 : 98, &FONT_S, "...", T->dim);
    for (i = 0; i < ph_n; i++) { note_text(q, ph_note[i]); while (*q) q++; *q++ = ' '; *q = 0; }
    tx(10, 114, &FONT_S, s, T->dim);
}

static void pic_glo(void)
{
    char s[8];
    uint32_t t = (uint32_t)G.tempo;
    put_int(s, (int32_t)t, 0);
    tx_c(120, 14, &FONT_L, s, T->fg);
    tx_c(120, 50, &FONT_S, ext_seen ? "BPM, FROM MIDI CLOCK" : "BPM", T->dim);
    {
        char r[32], *q = r;
        q = put(q, NOTE_NAME[(G.root + 24) % 12]);
        q = put(q, "  ");
        put(q, SCALES[G.scale % NSCALE].full);
        tx_c(120, 80, &FONT_S, r, T->fg);
    }
    tx_c(120, 112, &FONT_S, "THE KEYS SET THE ROOT", T->dim);
}

static void pic_stage(void)                     /* a circle, the climb as a dot on it, the last note big */
{
    int32_t cx = 120, cy = 64, r = 58, S = E.S;
    uint32_t ang = (uint32_t)((climb >> 16) * 1024 / (S ? S : 1));
    char s[8];
    circle(cx, cy, r, T->line);
    disc(cx + (isin(ang) * r >> 15), cy - (icos(ang) * r >> 15), 3, T->fg);
    if (running && ph_n) {
        uint32_t a2 = st_idx * 1024u / ph_n;
        disc(cx + (isin(a2) * (r - 10) >> 15), cy - (icos(a2) * (r - 10) >> 15), 2, T->acc);
    }
    if (last_out < 128u) { note_text(s, last_out); tx_c(cx, cy - 16, &FONT_L, s, T->fg); }
}

static void draw_main(void)
{
    cv_begin(240, MAIN_H, T->bg);
    tx_bg = T->bg;
    switch (page) {
    case PG_ENGINE: pic_engine(); break;
    case PG_AMP: pic_env(0); break;
    case PG_TONE: pic_env(1); break;
    case PG_LFO: pic_lfo(); break;
    case PG_FX: pic_fx(); break;
    case PG_ARP: pic_arp(); break;
    case PG_SEQ: pic_stairs(); break;
    case PG_GLO: pic_glo(); break;
    default: pic_stage(); break;
    }
    cv_blit(0, MAIN_Y);
}

/* ---- the boot screen: three logos to choose from */
/* ---- the boot logos (EM_LOGO 0..9). Each draws its mark in a 240 x 120 strip; the name sits below it.
 * Mono look: one light colour, its fades towards black, and a single orange point. */
static uint16_t fade(uint32_t t) { return mix565(T->bg, T->fg, t); }   /* t 0..256 */
static void sq(int32_t cx, int32_t cy, int32_t r, uint16_t c) { cv_rect(cx - r, cy - r, 2 * r, 2 * r, c); }

static void logo_rise(void)                     /* 0 RISE: two ramps of bars, bright in the middle, dim at both ends (a Shepard tone) */
{
    int32_t i, n = 16, x0 = 120 - n * 6;
    for (i = 0; i < n; i++) {
        int32_t h = 14 + (i % 8) * 10, d = i * 2 - (n - 1);
        uint32_t t = (uint32_t)(256 - d * d * 200 / ((n - 1) * (n - 1)));
        cv_rect(x0 + i * 12, 100 - h, 4, h, fade(t));
    }
    cv_rect(x0 + 8 * 12, 6, 4, 4, T->acc);      /* where the next ramp begins */
}
static void logo_clock(void)                    /* 1 CLOCK: twelve notes round a circle, each longer than the last, the longest next to the shortest */
{
    uint32_t i;
    for (i = 0; i < 12u; i++) {
        uint32_t a = i * 1024u / 12u;
        int32_t r0 = 50 - 2 - (int32_t)i * 2, r1 = 50, k, sx = isin(a), cy = -icos(a);
        for (k = r0; k <= r1; k++) {
            cv_pset(120 + (sx * k >> 15), 60 + (cy * k >> 15), i == 11u ? T->acc : T->fg);
            cv_pset(121 + (sx * k >> 15), 60 + (cy * k >> 15), i == 11u ? T->acc : T->fg);
        }
    }
}
static void logo_fade(void)                     /* 2 FADE: notes, newest first, each a little smaller and dimmer */
{
    int32_t i, x = 36;
    for (i = 0; i < 8; i++) {
        int32_t r = 9 - i;
        sq(x, 60, r, i ? fade((uint32_t)(256 - i * 30)) : T->acc);
        x += 2 * r + 9;
    }
}
static void logo_freeze(void)                   /* 3 FREEZE: a wave that, past the orange line, is held still */
{
    int32_t x, py = 60;
    for (x = 20; x < 120; x++) {
        int32_t y = 60 - (isin((uint32_t)(x - 20) * 1024u / 40u) * 26 >> 15);
        cv_line(x - 1, py, x, y, T->fg);
        py = y;
    }
    cv_rect(119, 22, 2, 76, T->acc);
    for (x = 124; x < 220; x += 6) cv_rect(x, 59, 3, 2, T->fg);
}
static void logo_spiral(void)                   /* 4 SPIRAL: pitch climbing round and round, the old turns dimmer */
{
    uint32_t a;
    int32_t px = 0, py = 0;
    for (a = 0; a <= 3u * 1024u; a += 6u) {
        int32_t r = 6 + (int32_t)a * 48 / 3072, x = 120 + (isin(a) * r >> 15), y = 60 - (icos(a) * r >> 15);
        if (a) cv_line(px, py, x, y, fade(40u + a * 216u / 3072u));
        px = x; py = y;
    }
    disc(px, py, 3, T->acc);
}
static void logo_grid(void)                     /* 5 GRID: a pixel staircase climbing an 8 x 8 grid */
{
    int32_t i, j;
    for (j = 0; j < 8; j++)
        for (i = 0; i < 8; i++) {
            int32_t x = 64 + i * 14, y = 4 + (7 - j) * 14, on = i == j || i == j + 1;
            if (i == 7 && j == 7) cv_rect(x, y, 12, 12, T->acc);
            else if (on) cv_rect(x, y, 12, 12, T->fg);
            else cv_rect(x + 5, y + 5, 2, 2, T->line);
        }
}
static void logo_echo(void)                     /* 6 ECHO: one note, its echoes spreading and fading */
{
    int32_t i;
    for (i = 5; i >= 1; i--) circle(120, 60, 10 * i + 4, fade((uint32_t)(300 - i * 50)));
    disc(120, 60, 5, T->acc);
}
static void logo_loop(void)                     /* 7 LOOP: an endless figure eight, a fading trail behind an orange head */
{
    uint32_t a, b;
    for (a = 0; a < 1024u; a += 4u) {
        int32_t s, c, x, y;
        b = a + 160u;                           /* the trail ends on the right loop, not at the crossing */
        s = isin(b); c = icos(b);
        x = 120 + (s * 76 >> 15); y = 60 + ((s * c >> 15) * 44 >> 15);
        cv_rect(x, y, 2, 2, fade(20u + a * 236u / 1024u));
    }
    b = 160u + 1020u;
    disc(121 + (isin(b) * 76 >> 15), 61 + ((isin(b) * icos(b) >> 15) * 44 >> 15), 4, T->acc);
}
static void logo_word(void)                     /* 8 WORD: the name climbs a step per letter */
{
    static const char W[] = "ENDLESS";
    char ch[2] = {0, 0};
    int32_t i;
    for (i = 0; i < 7; i++) {
        ch[0] = W[i];
        tx(64 + i * 16, 74 - i * 9, &FONT_L, ch, T->fg);
    }
    cv_rect(64 + 7 * 16, 74 - 6 * 9 + 2, 6, 6, T->acc);
}
static void logo_mono(void)                     /* 9 MONOGRAM: E and M in big pixels, the M's middle a staircase */
{
    static const char *const G[7] = {
        "#####.#...#",
        "#.....##.##",
        "#.....#.#.#",
        "####..#...#",
        "#.....#...#",
        "#.....#...#",
        "#####.#...#",
    };
    int32_t i, j, u = 10, x0 = 120 - 11 * u / 2, y0 = 60 - 7 * u / 2;
    for (j = 0; j < 7; j++)
        for (i = 0; i < 11; i++)
            if (G[j][i] == '#') cv_rect(x0 + i * u, y0 + j * u, u - 1, u - 1, (i == 8 && j == 2) ? T->acc : T->fg);
}

static void draw_boot(uint32_t which)
{
    static const char FADED[] = "MEMORY";
    lcd_fill(0, 0, 240, 240, T->bg);
    tx_bg = T->bg;
    cv_begin(240, 120, T->bg);
    switch (which % 10u) {
    case 0: logo_rise(); break;
    case 1: logo_clock(); break;
    case 2: logo_fade(); break;
    case 3: logo_freeze(); break;
    case 4: logo_spiral(); break;
    case 5: logo_grid(); break;
    case 6: logo_echo(); break;
    case 7: logo_loop(); break;
    case 8: logo_word(); break;
    default: logo_mono(); break;
    }
    cv_blit(0, 20);
    cv_begin(240, 90, T->bg);
    if (which % 10u == 8u) {                    /* MEMORY fades letter by letter */
        char ch[2] = {0, 0};
        int32_t i;
        for (i = 0; i < 6; i++) { ch[0] = FADED[i]; tx(96 + i * 8, 4, &FONT_S, ch, fade((uint32_t)(256 - i * 36))); }
    } else {
        tx_c(120, 4, &FONT_S, "ENDLESS MEMORY", T->fg);
    }
    tx_c(120, 66, &FONT_S, APP_VER, T->dim);
    cv_blit(0, 140);
}

/* ================================================================ MIDI */
static uint32_t idle_ok;
static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        uint32_t ty = st & 0xF0u;
        if (st == 0xF8u) { ext_last_ms = fm1_ms; ext_seen = 1; on_tick(); continue; }
        if (st == 0xFAu) { transport(0); transport(1); continue; }
        if (st == 0xFBu) { transport(1); continue; }
        if (st == 0xFCu) { transport(0); continue; }
        if (G.ch && (st & 15u) != (uint32_t)G.ch - 1u) continue;
        if (ty == 0x90u && d2) note_in(d1, d2);
        else if (ty == 0x80u || ty == 0x90u) note_out(d1);
        else if (ty == 0xE0u) E.bend = ((int32_t)((uint32_t)d2 << 7 | d1) - 8192) * 128 / 8192;   /* +-2 semitones */
        else if (ty == 0xB0u && d1 == 64u) freeze_set(d2 >= 64u);   /* a sustain pedal is FREEZE */
    }
}
static void app_idle(void)
{
    if (!idle_ok) return;
    idle_ok = 0;                                /* (never inside itself) */
    midi_in();
    clock_service();
    idle_ok = 1;
}

/* ================================================================ the main loop */
static void app_main(void)
{
    uint32_t prev = 0, k, t_glob = 0, glob_dirty = 0, main_ms = 0, foot_dirty = 1, head_dirty = 1, main_dirty = 1;
    uint32_t hold_btn = 0xFFu, hold_used = 0, hold_from = 0, save_held = 0, save_to = 0, rec_down_ms = 0, rec_down = 0;
    uint32_t sig = 0, last_sig = 1, head_sig = 0, last_head = 1;
    uint8_t keynote[27] = {0}, keyplays[27] = {0};
    uint32_t led_lit[27];
    for (k = 0; k < 27u; k++) led_lit[k] = 9;
    for (k = 0; k < (uint32_t)NSCALE; k++) SCALE_SHORT[k] = SCALES[k].name;

    if (settings_load(&G, sizeof G) || G.ver != 2u || G.tempo < 40 || G.tempo > 240 || G.preset < 0 || G.preset > 7 ||
        G.scale < 0 || G.scale >= NSCALE || G.root < -12 || G.root > 12) {
        G.ver = 2; G.tempo = 96; G.scale = 0; G.root = 0; G.ch = 0; G.oct = 0; G.preset = 0;
    }
    preset_load((uint32_t)G.preset);
    draw_boot(EM_LOGO);
    {
        uint32_t t0 = fm1_ms;
        while (fm1_ms - t0 < 1500u) platform_poll();
    }
    lcd_fill(0, 0, 240, 240, T->bg);
    clk_last = em_ticks();

    for (;;) {
        uint32_t keys, down, up, b, rel, out_ch = G.ch ? (uint32_t)G.ch - 1u : 0u, base;
        int32_t e;
        platform_poll();
        idle_ok = 0;
        midi_in();
        clock_service();

        /* ---- the keys (on the GLOBAL page they set the root) */
        base = (uint32_t)(53 + 12 * G.oct);
        keys = fm1_in.notes;
        down = (keys & ~prev) | fm1_input_note_edges();
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) {
                keynote[k] = (uint8_t)(base + k);
                if (page == PG_GLO) {
                    int32_t r = (int32_t)keynote[k] - 60;
                    while (r > 12) r -= 12;
                    while (r < -12) r += 12;
                    G.root = (int16_t)r; glob_dirty = 1; t_glob = fm1_ms; foot_dirty = main_dirty = 1;
                    touched = 2; touched_ms = fm1_ms;
                    keyplays[k] = 0;
                } else {
                    keyplays[k] = 1;
                    note_in(keynote[k], 100);
                    midi_send(0x90u | out_ch, out_note(keynote[k]), 100);
                }
            }
            if (up >> k & 1u && keyplays[k]) { keyplays[k] = 0; note_out(keynote[k]); midi_send(0x80u | out_ch, out_note(keynote[k]), 0); }
        }
        for (k = 0; k < NV; k++)                /* notes from the staircase and the arp end by themselves */
            if (V[k].timed && V[k].gate && (int32_t)(fm1_ms - V[k].off_at) >= 0) V[k].gate = 0;

        /* ---- the buttons */
        b = fm1_input_edges(&rel);
        if (b) foot_dirty = head_dirty = main_dirty = 1;
        {
            static const uint8_t PAGE_OF[14] = {0xFF, 0xFF, PG_FX, 0xFF, PG_AMP, PG_LFO, PG_ENGINE, PG_GLO, PG_STAGE,
                                                0xFF, PG_ARP, PG_SEQ, 0xFF, 0xFF};
            uint32_t id;
            for (id = 0; id < 14u; id++) {
                uint32_t pg = PAGE_OF[id];
                if (!(b >> id & 1u) || pg == 0xFFu) continue;
                hold_btn = id; hold_used = 0; hold_from = page;
                if (id == BTN_ENV) pg = page == PG_AMP ? PG_TONE : page == PG_TONE ? PG_AMP : page_env;
                if (id == BTN_ARP) { P.arp = !P.arp; say(P.arp ? "ARP ON" : "ARP OFF"); if (!P.arp) voices_all_off(); }
                page = pg;
                touched = 9;
                if (pg == PG_AMP || pg == PG_TONE) page_env = pg;
            }
            if (hold_btn != 0xFFu && (rel >> hold_btn & 1u)) {   /* let go: back where we were, if a knob was turned */
                if (hold_used && hold_btn != BTN_ARP) page = hold_from;
                hold_btn = 0xFFu;
            }
        }
        if ((b & (1u << BTN_OCT_DOWN) && G.oct > -2) || (b & (1u << BTN_OCT_UP) && G.oct < 2)) {
            static const char *const OCT_NAME[5] = {"OCTAVE -2", "OCTAVE -1", "OCTAVE 0", "OCTAVE +1", "OCTAVE +2"};
            G.oct = (int16_t)(G.oct + (b & (1u << BTN_OCT_UP) ? 1 : -1));
            glob_dirty = 1; t_glob = fm1_ms;
            say(OCT_NAME[G.oct + 2]);
        }
        if (b & (1u << BTN_SEL)) { freeze_set(!freeze); say(freeze ? "FREEZE" : "FREEZE OFF"); }
        if (b & (1u << BTN_PLAY)) { transport(!running); say(running ? "PLAY" : "STOP"); }
        if (b & (1u << BTN_REC)) { rec_down = 1; rec_down_ms = fm1_ms; }
        if (rec_down && (fm1_in.buttons >> BTN_REC & 1u) && fm1_ms - rec_down_ms > 1000u) {
            ph_n = 0; rec = 0; rec_down = 0; st_idx = 0; say("PHRASE ERASED"); main_dirty = head_dirty = 1;
        }
        if (rec_down && (rel >> BTN_REC & 1u)) {
            rec_down = 0;
            rec = !rec;
            rec_fresh = rec;
            say(rec ? "PLAY A NEW PHRASE" : "REC OFF");
        }
        if (b & (1u << BTN_SAVE)) { save_held = 1; save_to = (uint32_t)G.preset; say("SAVE TO P1"); toast[9] = (char)('1' + save_to); }
        if (save_held && (rel >> BTN_SAVE & 1u)) {
            save_held = 0;
            voices_all_off();
            preset_save(save_to);
            G.preset = (int16_t)save_to; glob_dirty = 1; t_glob = fm1_ms;
            say("SAVED TO P1"); toast[10] = (char)('1' + save_to);
        }

        /* ---- the knobs */
        for (k = 0; k < 4u; k++)
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                const param_t *p = &PAGES[page].k[k];
                int32_t v = *p->v + (iabs(e) > 1 && p->hi - p->lo > 20 ? e * 2 : e);
                *p->v = (int16_t)(v < p->lo ? p->lo : v > p->hi ? p->hi : v);
                if (p->v == &G.tempo || p->v == &G.scale || p->v == &G.root || p->v == &G.ch) { glob_dirty = 1; t_glob = fm1_ms; }
                engine_set();
                foot_dirty = main_dirty = 1;
                hold_used = 1;
                touched = k; touched_ms = fm1_ms;
            }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {   /* the tempo, everywhere */
            int32_t v = G.tempo + e;
            char t[16];
            G.tempo = (int16_t)(v < 40 ? 40 : v > 240 ? 240 : v);
            engine_set(); glob_dirty = 1; t_glob = fm1_ms; foot_dirty = main_dirty = 1;
            if (page != PG_GLO) { put_int(put(t, "TEMPO "), G.tempo, 0); say(t); }
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            int32_t v = P.alg + e;
            P.alg = (int16_t)(v < 0 ? 0 : v > 5 ? 5 : v);
            engine_set();
            say(ALG_NAME[P.alg]);
            main_dirty = 1;
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            int32_t v = (save_held ? (int32_t)save_to : G.preset) + e;
            v = v < 0 ? 0 : v > 7 ? 7 : v;
            if (save_held) { save_to = (uint32_t)v; say("SAVE TO P1"); toast[9] = (char)('1' + v); }
            else if (v != G.preset) {
                char t[20];
                G.preset = (int16_t)v;
                preset_load((uint32_t)v);
                glob_dirty = 1; t_glob = fm1_ms;
                t[0] = 'P'; t[1] = (char)('1' + v); t[2] = ' '; t[3] = ' '; put(t + 4, P.name);
                say(t);
            }
            foot_dirty = head_dirty = main_dirty = 1;
        }

        /* ---- the LEDs */
        {
            uint32_t lit = 0, glow = 0, v;
            for (v = 0; v < NV; v++)
                if (V[v].gate && V[v].raw >= base && V[v].raw < base + 27u) lit |= 1u << (V[v].raw - base);
            if (page == PG_GLO) {               /* the root's key */
                int32_t r = 60 + G.root - (int32_t)base;
                if (r >= 0 && r < 27) glow |= 1u << r;
            } else
                for (v = 0; v < ph_n; v++)      /* the phrase glows on the keys */
                    if (ph_note[v] >= base && ph_note[v] < base + 27u) glow |= 1u << (ph_note[v] - base);
            for (k = 0; k < 27u; k++) {
                uint32_t want = lit >> k & 1u ? 2u : glow >> k & 1u ? 1u : 0u;
                if (led_lit[k] != want) {
                    led_lit[k] = want;
                    if (want == 2u) fm1_led_key(KEY_LED(k), 1);
                    else { fm1_led_key(KEY_LED(k), 0); fm1_led_key_glow(KEY_LED(k), want == 1u); }
                }
            }
            fm1_led_key(BTN_OCT_DOWN, G.oct < 0);
            fm1_led_key(BTN_OCT_UP, G.oct > 0);
            fm1_led_key(BTN_SEL, freeze);
            fm1_led_key(BTN_HOME, page == PG_STAGE);
            fm1_led_key(BTN_EDIT, page == PG_ENGINE);
            fm1_led_key(BTN_ENV, page == PG_AMP || page == PG_TONE);
            fm1_led_key(BTN_LFO, page == PG_LFO);
            fm1_led_key(BTN_FX, page == PG_FX);
            fm1_led_key(BTN_ARP, P.arp);
            fm1_led_key(BTN_SEQ, page == PG_SEQ);
            fm1_led_key(BTN_GLO, page == PG_GLO);
            fm1_led_key(BTN_SAVE, save_held);
            fm1_led_key(BTN_PLAY, running);
            fm1_led_key(BTN_REC, rec);
        }

        /* ---- the screen: only what changed; a few pictures follow the sound, at most 6 times a second */
        idle_ok = 1;
        head_sig = (uint32_t)running | rec << 1 | (uint32_t)freeze << 2 | (uint32_t)P.arp << 3 | (uint32_t)G.preset << 4 |
                   (toast[0] && fm1_ms - toast_ms < 1400u) << 8;
        if (head_dirty || head_sig != last_head || (toast[0] && fm1_ms - toast_ms < 1400u && head_dirty)) {
            head_dirty = 0; last_head = head_sig;
            if (toast[0] && fm1_ms - toast_ms >= 1400u) toast[0] = 0;
            draw_header();
        }
        if (page == PG_STAGE) sig = (uint32_t)(climb >> 22) ^ last_out << 10 ^ st_idx << 18 ^ running << 24;
        else if (page == PG_ENGINE) sig = (uint32_t)(climb >> 20);
        else if (page == PG_SEQ) sig = st_rep << 4 ^ ph_n ^ running << 12;
        else if (page == PG_ARP) sig = held_n | (uint32_t)frozen_n << 5;
        else if (page == PG_GLO) sig = ext_seen;
        else sig = 0;
        if (main_dirty || (sig != last_sig && fm1_ms - main_ms >= 160u)) {
            main_dirty = 0; last_sig = sig; main_ms = fm1_ms;
            draw_main();
        }
        if (foot_dirty || (touched < 4u && fm1_ms - touched_ms >= 1500u && fm1_ms - touched_ms < 1600u)) {
            foot_dirty = 0;
            if (touched < 4u && fm1_ms - touched_ms >= 1500u) touched = 9;
            draw_footer();
        }
        idle_ok = 0;

        if (glob_dirty && quiet() && fm1_ms - t_glob > 3000u) {   /* the global settings, when silent */
            settings_save(&G, sizeof G);
            glob_dirty = 0;
        }
    }
}
