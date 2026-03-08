/*
 * DDC channelizer -- per-channel digital downconversion
 *
 * Architecture:
 *   1. NCO mix each channel to baseband (rotating phasor)
 *   2. Multi-stage FIR decimate (each stage <= 16x, cascaded)
 *   3. Output at ~4x symbol rate for the demodulator
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "simd_kernels.h"
#include <stdio.h>
#include <complex.h>

#include "channelizer.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Each decimation stage has a short FIR lowpass that anti-aliases
 * before downsampling. With decimation <= 16 per stage and 31 taps,
 * the filter works well. Stages cascade: e.g. 1000x = 10 * 10 * 10.
 */
#define MAX_STAGES 4
#define STAGE_FIR_TAPS 31
#define CLEANUP_FIR_TAPS 127  /* final narrowband filter at output rate */

typedef struct {
    int decimation;
    int count;
    float fir_taps[STAGE_FIR_TAPS];  /* real-valued lowpass */
    float complex fir_hist[STAGE_FIR_TAPS];
    int fir_idx;
} decim_stage_t;

/* Per-channel state */
typedef struct {
    int active;
    int channel_id;
    channel_type_t type;

    /* NCO */
    double nco_freq;
    float complex nco_phasor;
    float complex nco_current;
    int nco_renorm;

    /* Cascaded decimation stages */
    decim_stage_t stages[MAX_STAGES];
    int num_stages;

    /* Final cleanup filter (non-decimating, runs at output rate).
     * Double-buffer trick: hist is 2×NTAPS so any contiguous window of
     * NTAPS samples is available without circular-wrap copies. */
    float cleanup_taps[CLEANUP_FIR_TAPS];
    float complex cleanup_hist[CLEANUP_FIR_TAPS * 2];
    int cleanup_idx;
    int has_cleanup;

    /* Output buffer */
    float complex *out_buf;
    int out_len;
    int out_cap;
} channel_state_t;

struct channelizer {
    double center_freq;
    double samp_rate;
    channel_cb_t cb;
    void *user;

    channel_state_t channels[MAX_CHANNELS];
    int num_channels;
};

/* Design lowpass FIR (windowed sinc, Blackman window) */
static void design_lowpass(float *taps, int num_taps, double cutoff) {
    int M = num_taps - 1;
    double sum = 0;

    for (int i = 0; i < num_taps; i++) {
        double n = i - M / 2.0;
        double h;
        if (fabs(n) < 1e-10)
            h = 2.0 * cutoff;
        else
            h = sin(2.0 * M_PI * cutoff * n) / (M_PI * n);

        double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / M)
                        + 0.08 * cos(4.0 * M_PI * i / M);
        h *= w;
        sum += h;
        taps[i] = (float)h;
    }

    for (int i = 0; i < num_taps; i++)
        taps[i] /= (float)sum;
}

/* Process one complex sample through a decimation stage.
 * Returns 1 when an output sample is ready in *out. */
static inline int decim_stage_process(decim_stage_t *st,
                                       float complex in,
                                       float complex *out) {
    st->fir_hist[st->fir_idx] = in;
    st->fir_idx = (st->fir_idx + 1) % STAGE_FIR_TAPS;

    if (++st->count < st->decimation)
        return 0;
    st->count = 0;

    /* Compute FIR output */
    float complex acc = 0;
    int idx = st->fir_idx;
    for (int t = 0; t < STAGE_FIR_TAPS; t++) {
        idx--;
        if (idx < 0) idx = STAGE_FIR_TAPS - 1;
        acc += st->fir_hist[idx] * st->fir_taps[t];
    }
    *out = acc;
    return 1;
}

/* Factor total_decim into stages of at most max_per_stage.
 * Returns number of stages, fills decim[] array. */
static int plan_decimation(int total, int max_per_stage,
                            int *decim, int max_stages) {
    int n = 0;
    int remaining = total;

    while (remaining > 1 && n < max_stages) {
        if (remaining <= max_per_stage) {
            decim[n++] = remaining;
            remaining = 1;
        } else {
            /* Find largest factor <= max_per_stage */
            int best = 2;
            for (int d = max_per_stage; d >= 2; d--) {
                if (remaining % d == 0) {
                    best = d;
                    break;
                }
            }
            decim[n++] = best;
            remaining /= best;
        }
    }

    /* If we couldn't fully factor, add remaining as final stage */
    if (remaining > 1 && n < max_stages)
        decim[n++] = remaining;

    return n;
}

static double target_output_rate(channel_type_t type) {
    switch (type) {
    case CHAN_STDC_EGC:   return 1200.0 * 16.0;
    case CHAN_AERO_600:   return 48000.0;   /* 80 SPS — also used by ZMQ to JAERO */
    case CHAN_AERO_1200:  return 48000.0;  /* 40 SPS */
    case CHAN_AERO_10500: return 10500.0 * 8.0;
    case CHAN_AERO_8400:  return 8400.0 * 8.0;
    default: return 48000.0;
    }
}

/* Signal bandwidth for each channel type (Hz).
 * Used to set the final channelizer filter width so only
 * the target signal passes through to the demod. */
static double signal_bandwidth(channel_type_t type) {
    switch (type) {
    case CHAN_STDC_EGC:   return 2400.0;    /* BPSK 1200 baud */
    case CHAN_AERO_600:   return 6000.0;    /* ±3 kHz cleanup */
    case CHAN_AERO_1200:  return 6000.0;
    case CHAN_AERO_10500: return 15000.0;   /* OQPSK wider signal */
    case CHAN_AERO_8400:  return 10000.0;   /* matches SDRReceiver */
    default: return 0;                      /* 0 = use default anti-alias */
    }
}

channelizer_t *channelizer_create(double center_freq, double samp_rate,
                                   channel_cb_t cb, void *user) {
    channelizer_t *ch = calloc(1, sizeof(*ch));
    if (!ch) return NULL;

    ch->center_freq = center_freq;
    ch->samp_rate = samp_rate;
    ch->cb = cb;
    ch->user = user;
    ch->num_channels = 0;

    return ch;
}

int channelizer_add_channel(channelizer_t *ch, double freq,
                             channel_type_t type, int channel_id) {
    if (ch->num_channels >= MAX_CHANNELS)
        return -1;

    channel_state_t *c = &ch->channels[ch->num_channels];
    memset(c, 0, sizeof(*c));

    c->active = 1;
    c->channel_id = channel_id;
    c->type = type;

    /* NCO setup */
    c->nco_freq = freq - ch->center_freq;
    double phase_inc = -2.0 * M_PI * c->nco_freq / ch->samp_rate;
    c->nco_phasor = cosf((float)phase_inc) + sinf((float)phase_inc) * I;
    c->nco_current = 1.0f;
    c->nco_renorm = 0;

    /* Compute total decimation */
    double out_rate = target_output_rate(type);
    int total_decim = (int)(ch->samp_rate / out_rate);
    if (total_decim < 1) total_decim = 1;

    /* Plan cascaded stages (max 16x per stage for good filter quality) */
    int stage_decims[MAX_STAGES];
    c->num_stages = plan_decimation(total_decim, 16, stage_decims, MAX_STAGES);

    /* Initialize each stage with its own anti-alias filter */
    double stage_rate = ch->samp_rate;
    for (int i = 0; i < c->num_stages; i++) {
        decim_stage_t *st = &c->stages[i];
        st->decimation = stage_decims[i];
        st->count = 0;
        st->fir_idx = 0;
        memset(st->fir_hist, 0, sizeof(st->fir_hist));

        double cutoff = 0.4 / st->decimation;
        design_lowpass(st->fir_taps, STAGE_FIR_TAPS, cutoff);

        stage_rate /= st->decimation;
    }

    /* Final cleanup filter: narrow bandpass at output rate.
     * The decimation stages only do anti-alias filtering which leaves
     * a wide passband. This 127-tap FIR at the output rate (e.g. 48 kHz)
     * isolates just the target signal bandwidth. */
    double sig_bw = signal_bandwidth(type);
    c->cleanup_idx = 0;
    memset(c->cleanup_hist, 0, sizeof(c->cleanup_hist));
    if (sig_bw > 0 && sig_bw < stage_rate * 0.8) {
        double cleanup_cutoff = sig_bw / (2.0 * stage_rate);
        design_lowpass(c->cleanup_taps, CLEANUP_FIR_TAPS, cleanup_cutoff);
        c->has_cleanup = 1;
    } else {
        c->has_cleanup = 0;
    }

    /* Output buffer */
    c->out_cap = (int)(stage_rate * 1.1) + 256;
    c->out_buf = malloc(c->out_cap * sizeof(float complex));
    c->out_len = 0;

    ch->num_channels++;
    return 0;
}

void channelizer_process(channelizer_t *ch, const float *samples,
                          int num_samples) {
    for (int s = 0; s < num_samples; s++) {
        float re = samples[s * 2];
        float im = samples[s * 2 + 1];
        float complex input = re + im * I;

        for (int c = 0; c < ch->num_channels; c++) {
            channel_state_t *cs = &ch->channels[c];
            if (!cs->active) continue;

            /* NCO mix to baseband */
            float complex x = input * cs->nco_current;
            cs->nco_current *= cs->nco_phasor;

            if (++cs->nco_renorm >= 1024) {
                cs->nco_renorm = 0;
                float mag = cabsf(cs->nco_current);
                if (mag > 0.0f)
                    cs->nco_current /= mag;
            }

            /* Cascade through decimation stages */
            int produced = 1;
            for (int i = 0; i < cs->num_stages && produced; i++) {
                float complex out;
                produced = decim_stage_process(&cs->stages[i], x, &out);
                x = out;
            }
            if (!produced) continue;

            /* Apply cleanup filter if active.
             * Double-buffer: write sample at [idx] AND [idx+NTAPS] so a
             * contiguous NTAPS-sample window starting at idx+1 (or
             * wrapped) is always flat in memory. Then call simd_fir_ccf
             * with n=1 for one vectorized dot-product. */
            if (cs->has_cleanup) {
                cs->cleanup_hist[cs->cleanup_idx] = x;
                cs->cleanup_hist[cs->cleanup_idx + CLEANUP_FIR_TAPS] = x;
                cs->cleanup_idx = (cs->cleanup_idx + 1) % CLEANUP_FIR_TAPS;

                /* Window of NTAPS oldest→newest starts at cleanup_idx
                 * in the double buffer (no wrap needed). */
                float complex out;
                simd_fir_ccf(cs->cleanup_taps, CLEANUP_FIR_TAPS,
                             &cs->cleanup_hist[cs->cleanup_idx], &out, 1);
                x = out;
            }

            /* Accumulate output */
            if (cs->out_len < cs->out_cap)
                cs->out_buf[cs->out_len++] = x;
        }
    }

    /* Flush output buffers */
    int flush_threshold = 32;  /* lowered for low-rate channels */
    for (int c = 0; c < ch->num_channels; c++) {
        channel_state_t *cs = &ch->channels[c];
        if (cs->out_len >= flush_threshold) {
            if (ch->cb)
                ch->cb(cs->channel_id, cs->type, cs->out_buf,
                       cs->out_len, ch->user);
            cs->out_len = 0;
        }
    }
}

void channelizer_process_i8(channelizer_t *ch, const int8_t *samples,
                             int num_samples) {
    int block = 4096;
    float complex cbuf[block];

    for (int off = 0; off < num_samples; off += block) {
        int n = num_samples - off;
        if (n > block) n = block;

        /* SIMD-accelerated int8 IQ → float complex conversion.
         * simd_convert_i8_cf is wired by simd_init() to the best
         * available path (AVX2 / SSE4.2 / NEON / scalar). */
        simd_convert_i8_cf(samples + off * 2, cbuf, n);

        channelizer_process(ch, (float *)cbuf, n);
    }
}

int channelizer_has_freq(channelizer_t *ch, double freq, double tolerance) {
    if (!ch) return 0;
    for (int c = 0; c < ch->num_channels; c++) {
        double ch_freq = ch->center_freq + ch->channels[c].nco_freq;
        if (fabs(ch_freq - freq) < tolerance)
            return 1;
    }
    return 0;
}

double channelizer_output_rate(channelizer_t *ch, int channel_id) {
    for (int c = 0; c < ch->num_channels; c++) {
        if (ch->channels[c].channel_id == channel_id) {
            double rate = ch->samp_rate;
            for (int i = 0; i < ch->channels[c].num_stages; i++)
                rate /= ch->channels[c].stages[i].decimation;
            return rate;
        }
    }
    return 0;
}

void channelizer_destroy(channelizer_t *ch) {
    if (!ch) return;

    for (int c = 0; c < ch->num_channels; c++)
        free(ch->channels[c].out_buf);
    free(ch);
}
