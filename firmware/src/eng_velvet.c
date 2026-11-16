/* SPDX-License-Identifier: GPL-3.0-only
 * VELVET MOTION for the M-VAVE FM-1. Engine written for this firmware; the sound models follow the
 * VELVET MOTION 0.56 desktop instrument (its EngineModels.h and OrganModel.h), redone in fixed point. */
/* VELVET: the VELVET MOTION engines in one engine. MODE picks the model; TONE, DCY and MOVE are the
 * desktop plugin's Tone, Decay and Motion and mean the same thing in every mode.
 *
 *   EP     Soul Keys: electric piano. Six harmonics (1 2 3 4 6 8) of one phase, each fading at its own rate;
 *          TONE and the key's velocity brighten the upper five; MOVE is a 4.2 Hz tremolo.
 *   BELL   Soul Keys "Bells": six inharmonic partials (1, 2.71, 4.12, 5.43, 6.79, 8.21), long decay.
 *   VIBE   Soul Keys "Vibes": six bar partials (1, 3.99, 9.97, 16.1, 22.4, 30.2), 5.2 Hz tremolo.
 *   BASS   Funk Bass: saw + pulse + a sine an octave down, into a low-pass. RES and ENV add the
 *          resonant, envelope-swept filter of the plugin's "Ladder" style; MOVE sways the pulse width.
 *   SYNTH  Velvet Synths: two saws, MOVE spreads them (1 .. 10 cents), low-pass with RES and ENV.
 *   STRNG  Velvet Synths "Strings": five saws spread by MOVE (3 .. 18 cents), the same filter.
 *   ORGAN  Soul Organ: nine drawbars (16' .. 1') from one phase. BARS morphs through eight registrations;
 *          CLICK is the key click, PERC the decaying third harmonic. (Felucca's CHORUS send stands in
 *          for the rotary speaker.)
 *
 * WARM is the plugin's Warmth: a soft saturation, here per voice.
 *
 * How the decays work without exp() or a multiply per sample: partial i of the keys fades as
 * exp(-(1 + c i) t / tau). With A = exp(-t / tau) and B = exp(-c t / tau) that is A * B^i, so a voice keeps
 * only A and B, steps them once per block (32 samples) and ramps each partial's level across the block.
 *
 * Voice state: ph[0..2] phases; s[0], s[1] the filter (or A and B for the keys); s[2] the motion LFO phase;
 * s[3] the filter / percussion envelope; s[4] the click level and noise; s[5..7] three more phases. */

static const char *const N_VLV_MODE[] = {"EP", "BELL", "VIBE", "BASS", "SYNTH", "STRNG", "ORGAN"};
enum { VM_EP, VM_BELL, VM_VIBE, VM_BASS, VM_SYNTH, VM_STRNG, VM_ORGAN, VM_NMODE };

static const param_desc_t VLV_NONE = {"-", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t VLV_VELO = {"VELO", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t VLV_BARS = {"BARS", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t VLV_CLICK = {"CLICK", F_PCT, 0, 127, 64, 0, 0};
static const param_desc_t VLV_PERC = {"PERC", F_PCT, 0, 127, 64, 0, 0};

static uint32_t vlv_mode(const track_t *t) { return (uint32_t)t->p[P_E0] < VM_NMODE ? (uint32_t)t->p[P_E0] : VM_EP; }

/* EDIT 2's three mode-dependent values: what they are called in the current MODE */
static const param_desc_t *velvet_desc(const track_t *t, uint32_t k)
{
    uint32_t mode = vlv_mode(t);
    if (k < 4u || k > 6u)
        return 0;
    if (mode <= VM_VIBE)
        return k == 4u ? &VLV_VELO : &VLV_NONE;
    if (mode == VM_ORGAN)
        return k == 4u ? &VLV_BARS : k == 5u ? &VLV_CLICK : &VLV_PERC;
    return k == 6u ? &VLV_NONE : 0;                   /* BASS SYNTH STRNG: RES, ENV as declared */
}

/* 1 - exp(-CTL / (FS tau)) for small steps is CTL / (FS tau): the per-block decay factor in Q30 of a time
 * constant tau_ms (>= 20). 2^30 * 32 * 1000 / 44100 = 779,137,000 at 44.1 kHz */
#define VLV_STEP_NUM ((uint32_t)(1073741824ull * CTL * 1000u / FS))
static inline int32_t vlv_decay_k(int32_t tau_ms)
{
    return (int32_t)(1073741824u - VLV_STEP_NUM / (uint32_t)(tau_ms < 20 ? 20 : tau_ms));
}
static inline int32_t vlv_mul30(int32_t a, int32_t k) { return (int32_t)(((int64_t)a * k) >> 30); }
/* a phase increment times a ratio in Q8; 0 when the partial would pass 0.45 FS (it is left out) */
static inline uint32_t vlv_partial_inc(uint32_t inc, uint32_t ratio_q8)
{
    uint64_t x = ((uint64_t)inc * ratio_q8) >> 8;
    return x > 1932735283ull ? 0u : (uint32_t)x;      /* 0.45 * 2^32 */
}
/* the motion LFO: one sine per voice, stepped per block; hz100 = its rate x 100 */
static inline int32_t vlv_lfo(voice_t *v, uint32_t hz100)
{
    int32_t s = sine_i((uint32_t)v->s[2]);
    v->s[2] = (int32_t)((uint32_t)v->s[2] + hz100 * (uint32_t)(4294967296ull * CTL / 100u / FS));
    return s;                                         /* Q15 */
}
/* WARM: tanh(x d) / d with d = 1 .. 4, as the plugin's Warmth; 0 = clean (not called) */
static inline int32_t vlv_warm(int32_t s, int32_t warm)
{
    int32_t d = 256 + warm * 6;                       /* Q8: 1.0 .. 3.98 */
    return softclip((s * d) >> 8) * 256 / d;
}

static void velvet_note_on(track_t *t, voice_t *v)
{
    uint32_t mode = vlv_mode(t), k;
    for (k = 0; k < 8u; k++)
        v->s[k] = 0;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;
    if (mode <= VM_VIBE) {
        v->s[0] = v->s[1] = 1 << 30;                  /* A, B: every partial at full level */
    } else if (mode == VM_STRNG) {                    /* the ensemble starts spread, as the plugin's (0 .19 .41 .63 .83) */
        v->ph[1] = 816043786u; v->ph[2] = 1760936591u; v->s[5] = (int32_t)2705829396u; v->s[6] = (int32_t)3564822855u;
    } else if (mode == VM_SYNTH) {
        v->ph[1] = 0x40000000u;
    }
    v->s[3] = 1 << 30;                                /* filter / percussion envelope: 1.0 */
    v->s[4] = 32767;                                  /* click level */
    v->s[7] = mode == VM_ORGAN ? 0x1234567 + (int32_t)v->age : 0;   /* ORGAN: click noise (the keys use s[7] as a phase) */
}

/* ---- keys: EP, BELL, VIBE ---------------------------------------------------------------------- */
static const uint16_t VLV_KEY_RATIO[3][6] = {         /* Q8 */
    {256, 512, 768, 1024, 1536, 2048},
    {256, 694, 1055, 1390, 1738, 2102},
    {256, 1021, 2552, 4122, 5734, 7731},
};
static const uint16_t VLV_KEY_WEIGHT[3][6] = {        /* Q15 */
    {32767, 4260, 11469, 1966, 2949, 1311},
    {32767, 17039, 9830, 6226, 3932, 2294},
    {32767, 9830, 4260, 2130, 983, 492},
};
static __attribute__((noinline)) void velvet_keys(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m,
                                                  uint32_t mode)
{
    const int16_t *p = t->p;
    int32_t tone = clamp(p[P_E1] + (m->cutoff >> 8), 0, 127), decay = p[P_E2];
    int32_t motion = clamp(p[P_E3] + ((m->shape - (64 << 8)) >> 8), 0, 127), dyn = p[P_E4], warm = p[P_E7];
    int32_t vel = v->vel, tau, cq, bright, trem, depth, lfo, a0, b0, a1, b1, pa, pb = 0;
    int32_t amp[6];
    uint32_t inc[6], ph[6], i, k;
    /* the fundamental's time constant and how much faster each higher partial fades (c) */
    if (mode == VM_EP) { tau = 350 + decay * 3500 / 127; cq = 70; depth = 20; }
    else if (mode == VM_BELL) { tau = 450 + decay * 5000 / 127; cq = 75; depth = 12; }
    else { tau = 300 + decay * 3800 / 127; cq = 75; depth = 45; }
    a0 = v->s[0]; b0 = v->s[1];
    a1 = vlv_mul30(a0, vlv_decay_k(tau));
    b1 = vlv_mul30(b0, vlv_decay_k(tau * 100 / cq));
    v->s[0] = a1; v->s[1] = b1;
    /* upper partials: (base + tone) (floor + velocity) (1 + VELO (velocity - 1/2) 1.4), Q12 */
    if (mode == VM_EP)
        bright = ((614 + tone * 4096 / 127) * (1024 + vel * 3072 / 127)) >> 12;
    else
        bright = ((492 + tone * 4710 / 127) * (1229 + vel * 2867 / 127)) >> 12;
    bright = (bright * (4096 + ((dyn * 5734 / 127) * (vel * 2 - 127) / 254))) >> 12;
    if (bright < 0)
        bright = 0;
    lfo = vlv_lfo(v, mode == VM_VIBE ? 520u : 420u);
    /* 1 - depth motion + depth motion sin (EP) or cos (BELL, VIBE: starts at full level), Q15 */
    if (mode != VM_EP)
        lfo = sine_i((uint32_t)v->s[2] + 0x40000000u);
    trem = 32767 - ((depth * motion * 328 / 127) * (32767 - lfo) >> 15);
    /* each partial's level for this block: weight x A B^i, x 0.6 so the fundamental sits below full scale
     * (room for the rest). A block is 0.7 ms and the fastest partial fades 1 % in that time: no ramp is needed */
    pa = a0 >> 15; (void)pb;
    for (i = 0; i < 6u; i++) {
        int32_t w = VLV_KEY_WEIGHT[mode][i];
        if (i)
            w = (w * bright) >> 12;
        w = (w * trem) >> 15;
        amp[i] = (((pa * w) >> 15) * 19661) >> 15;
        pa = (pa * (b0 >> 15)) >> 15;
        inc[i] = vlv_partial_inc(m->inc, VLV_KEY_RATIO[mode][i]);
        if (!inc[i])
            amp[i] = 0;                               /* above 0.45 FS: left out */
    }
    ph[0] = v->ph[0]; ph[1] = v->ph[1]; ph[2] = v->ph[2];
    ph[3] = (uint32_t)v->s[5]; ph[4] = (uint32_t)v->s[6]; ph[5] = (uint32_t)v->s[7];
    if (mode == VM_EP) {                              /* harmonics 1 2 3 4 6 8: whole multiples of one phase */
        uint32_t p0 = ph[0], pinc = m->inc;
        int32_t g0 = amp[0], g1 = amp[1], g2 = amp[2], g3 = amp[3], g4 = amp[4], g5 = amp[5];
        for (k = 0; k < n; k++) {
            int32_t s = sine_i(p0) * g0 + sine_i(p0 * 2u) * g1 + sine_i(p0 * 3u) * g2;
            if (g3 | g4 | g5)
                s += sine_i(p0 * 4u) * g3 + sine_i(p0 * 6u) * g4 + sine_i(p0 * 8u) * g5;
            s >>= 15;
            p0 += pinc;
            if (warm)
                s = vlv_warm(s, warm);
            out[k] += voice_amp(s, m, k) * 2;
        }
        ph[0] = p0;
    } else {
        for (k = 0; k < n; k++) {
            int32_t s = 0;
            for (i = 0; i < 6u; i++) {
                s += sine_i(ph[i]) * amp[i];
                ph[i] += inc[i];
            }
            s >>= 15;
            if (warm)
                s = vlv_warm(s, warm);
            out[k] += voice_amp(s, m, k) * 2;
        }
    }
    v->ph[0] = ph[0]; v->ph[1] = ph[1]; v->ph[2] = ph[2];
    v->s[5] = (int32_t)ph[3]; v->s[6] = (int32_t)ph[4]; v->s[7] = (int32_t)ph[5];
}

/* ---- BASS, SYNTH, STRNG: oscillators into the low-pass ---------------------------------------- */
/* TONE -> the filter's cutoff value (0..127 << 8, dsp.c: 30 Hz .. 16 kHz). The plugin's ranges are
 * BASS 100 Hz .. 4.5 kHz (values 24.4 .. 101.3) and SYNTH 220 Hz .. 14.3 kHz (40.3 .. 124.8); ENV opens it by
 * up to five octaves (70 values) and falls back with DCY */
static __attribute__((noinline)) void velvet_subtractive(track_t *t, voice_t *v, int32_t *out, uint32_t n,
                                                         const vmod_t *m, uint32_t mode)
{
    const int16_t *p = t->p;
    int32_t tone = p[P_E1], decay = p[P_E2], warm = p[P_E7];
    int32_t motion = clamp(p[P_E3] + ((m->shape - (64 << 8)) >> 8), 0, 127);
    int32_t res = p[P_E4], fenv = p[P_E5], env = v->s[3] >> 15, cut;
    int32_t ic1 = v->s[0], ic2 = v->s[1];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], ph3 = (uint32_t)v->s[5], ph4 = (uint32_t)v->s[6];
    uint32_t inc = m->inc, k;
    tsvf_t flt;
    if (mode == VM_BASS) {
        cut = 6246 + tone * 19686 / 127;
        v->s[3] = vlv_mul30(v->s[3], vlv_decay_k(35 + decay * 850 / 127));
    } else {
        cut = 10317 + tone * 21632 / 127;
        v->s[3] = vlv_mul30(v->s[3], vlv_decay_k(80 + decay * 900 / 127));
    }
    cut += ((fenv * 17920 / 127) * env >> 15) + m->cutoff;
    tsvf_coef(&flt, cut, res);
    if (mode == VM_BASS) {
        int32_t lfo = vlv_lfo(v, 65u);
        uint32_t pw = 0x80000000u + (uint32_t)((motion * 5412 / 127) * lfo) * 4u;   /* 0.5 +- 0.16 */
        uint32_t incs = inc >> 1;
        for (k = 0; k < n; k++) {
            int32_t s = (osc_saw(ph0, inc) * 14746 + osc_pulse(ph0, inc, pw) * 6554 + sine_i(ph1) * 11469) >> 15;
            ph0 += inc; ph1 += incs;
            if (warm)
                s = vlv_warm(s, warm);
            s = soft_knee(tsvf_lp(&flt, s >> 1, &ic1, &ic2), 16000) * 2;
            out[k] += voice_amp(s, m, k) * 2;
        }
    } else if (mode == VM_SYNTH) {
        uint32_t inc2 = cents_inc(m->pitch16, 1 + motion * 9 / 127, m->fine);
        for (k = 0; k < n; k++) {
            int32_t s = (osc_saw(ph0, inc) + osc_saw(ph1, inc2)) >> 1;
            ph0 += inc; ph1 += inc2;
            if (warm)
                s = vlv_warm(s, warm);
            s = soft_knee(tsvf_lp(&flt, s >> 1, &ic1, &ic2), 16000) * 2;
            out[k] += voice_amp(s, m, k) * 2;
        }
    } else {                                          /* STRNG: five saws at -1 -.45 0 +.47 +1 of the spread */
        int32_t spread = (3 + motion * 15 / 127) * 2367 / 1000;      /* cents -> 1/4096 of the increment */
        int32_t u = (int32_t)(inc >> 12);
        uint32_t i0 = inc - (uint32_t)(u * spread), i1 = inc - (uint32_t)(u * spread * 45 / 100);
        uint32_t i3 = inc + (uint32_t)(u * spread * 47 / 100), i4 = inc + (uint32_t)(u * spread);
        for (k = 0; k < n; k++) {
            int32_t s = (osc_saw(ph0, i0) + osc_saw(ph1, i1) + osc_saw(ph2, inc) + osc_saw(ph3, i3) + osc_saw(ph4, i4)) * 6554 >> 15;
            ph0 += i0; ph1 += i1; ph2 += inc; ph3 += i3; ph4 += i4;
            if (warm)
                s = vlv_warm(s, warm);
            s = soft_knee(tsvf_lp(&flt, s >> 1, &ic1, &ic2), 16000) * 2;
            out[k] += voice_amp(s, m, k) * 2;
        }
    }
    v->ph[0] = ph0; v->ph[1] = ph1; v->ph[2] = ph2; v->s[5] = (int32_t)ph3; v->s[6] = (int32_t)ph4;
    v->s[0] = ic1; v->s[1] = ic2;
}

/* ---- ORGAN ------------------------------------------------------------------------------------ */
/* the nine footages as multiples of the 16' pipe, and eight registrations (0..8 per bar, as drawbars are
 * numbered) that BARS morphs through: soft flutes first, the full organ last */
static const uint8_t VLV_BAR_MUL[9] = {1, 3, 2, 4, 6, 8, 10, 12, 16};
static const uint8_t VLV_REG[8][9] = {
    {0, 0, 8, 0, 0, 0, 0, 0, 0},                      /* one flute */
    {0, 0, 8, 4, 0, 0, 0, 0, 0},
    {6, 2, 8, 4, 2, 1, 0, 0, 0},                      /* VELVET MOTION's default bars */
    {8, 8, 8, 0, 0, 0, 0, 0, 0},                      /* jazz */
    {8, 0, 8, 8, 0, 0, 0, 0, 4},                      /* gospel, bright top */
    {8, 6, 8, 6, 4, 4, 2, 0, 0},
    {8, 8, 8, 6, 5, 4, 3, 2, 2},
    {8, 8, 8, 8, 8, 8, 8, 8, 8},                      /* full */
};
static __attribute__((noinline)) void velvet_organ(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t tone = clamp(p[P_E1] + (m->cutoff >> 8), 0, 127), decay = p[P_E2], warm = p[P_E7];
    int32_t bars = p[P_E4] * 7, click = p[P_E5], perc = p[P_E6];
    int32_t lo = bars / 127, fr = (bars - lo * 127) * 256 / 127, hi = lo < 7 ? lo + 1 : 7;
    int32_t level[9], penv = v->s[3] >> 15, cl = v->s[4], nst = v->s[7];
    uint32_t base = m->inc >> 1, ph = v->ph[0], i, k, top = 9u, pon;
    for (i = 0; i < 9u; i++) {
        int32_t b = VLV_REG[lo][i] * (256 - fr) + VLV_REG[hi][i] * fr;        /* 0 .. 2048 */
        int32_t r = VLV_BAR_MUL[i];                   /* the plugin's ratio is r / 2 */
        /* TONE: the upper bars are shaded by 1 / (1 + (1 - tone) ratio^2 0.18) */
        int32_t shade = 32768 * 256 / (256 + ((127 - tone) * r * r * 46 / (4 * 127)));
        level[i] = (b * shade) >> 11;                 /* Q15 at a full bar */
        if (vlv_partial_inc(base, (uint32_t)r << 8) == 0u)
            level[i] = 0;
    }
    while (top > 1u && !level[top - 1u])
        top--;
    pon = perc && penv > 32 && vlv_partial_inc(base, 6u << 8);
    perc = (perc * 387 * penv) >> 15;                 /* 1.5 x the percussion, Q15 at 127 */
    v->s[3] = vlv_mul30(v->s[3], vlv_decay_k(45 + decay * 700 / 127));
    click = click * 83;                               /* 0.32 x, Q15 */
    for (k = 0; k < n; k++) {
        int32_t s = 0;
        for (i = 0; i < top; i++)
            s += (sine_i(ph * VLV_BAR_MUL[i]) * level[i]) >> 15;
        if (pon)
            s += (sine_i(ph * 6u) * perc) >> 15;
        if (cl > 8) {                                 /* key click: noise fading in about 3.5 ms */
            s += ((((int32_t)(noise32(&nst) >> 17) - 16384) * 2 * click >> 15) * cl) >> 15;
            cl = (cl * 32556) >> 15;
        }
        ph += base;
        s = (s * 7537) >> 15;                         /* 0.23 */
        if (warm)
            s = vlv_warm(s, warm);
        out[k] += voice_amp(s, m, k) * 2;
    }
    v->ph[0] = ph; v->s[4] = cl; v->s[7] = nst;
}

static void velvet_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    uint32_t mode = vlv_mode(t);
    if (mode <= VM_VIBE)
        velvet_keys(t, v, out, n, m, mode);
    else if (mode == VM_ORGAN)
        velvet_organ(t, v, out, n, m);
    else
        velvet_subtractive(t, v, out, n, m, mode);
}

#include "velvet_presets.h"      /* VELVET_PRESETS[]: tools/gen_velvet_presets.py, from the plugin's factory sounds */

static const engine_t ENG_VELVET = {
    .name = "VELVET",
    .page_title = {"VELVET", "SHAPE"},
    .edit = {
        {"MODE", F_ENUM, 0, VM_NMODE - 1, 0, N_VLV_MODE, 0},
        {"TONE", F_PCT, 0, 127, 64, 0, 0},
        {"DCY", F_PCT, 0, 127, 70, 0, 0},
        {"MOVE", F_PCT, 0, 127, 25, 0, 0},
        {"RES", F_PCT, 0, 127, 64, 0, 0},
        {"ENV", F_PCT, 0, 127, 64, 0, 0},
        {"-", F_PCT, 0, 127, 64, 0, 0},
        {"WARM", F_PCT, 0, 127, 19, 0, 0},
    },
    .presets = VELVET_PRESETS,
    .npresets = NELEM(VELVET_PRESETS),
    .note_on = velvet_note_on,
    .render = velvet_render,
    .desc = velvet_desc,
    .knob = {P_E1, P_E2, P_E3, P_E7},               /* HOME: Tone, Decay, Motion, Warmth: the plugin's front panel */
};
