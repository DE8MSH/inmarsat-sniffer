/*
 * DBPSK demodulator for STD-C / EGC (1200 baud)
 *
 * Differential BPSK -- data is encoded in the phase change between
 * consecutive symbols, not the absolute phase. This means no carrier
 * recovery or PLL is needed -- just compare adjacent symbol phases.
 *
 * Clock recovery uses a simple Mueller-Muller timing error detector.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <complex.h>

#include "demod_dbpsk.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SOFT_BIT_BUF 512

struct dbpsk_demod {
    double samp_rate;
    double symbol_rate;
    double sps;            /* samples per symbol */

    /* Timing recovery (Mueller-Muller) */
    double mu;             /* fractional sample offset [0, 1) */
    double mu_gain;        /* timing loop gain */
    int skip;              /* integer samples to skip */

    /* Previous samples for interpolation */
    float complex prev_sample;
    float complex prev_symbol;

    /* Matched filter (integrate & dump approximation) */
    float complex accum;
    int accum_count;
    int accum_target;

    /* Output */
    dbpsk_bit_cb_t cb;
    void *user;
    float soft_bits[SOFT_BIT_BUF];
    int num_soft_bits;
};

dbpsk_demod_t *dbpsk_demod_create(double samp_rate, double symbol_rate,
                                   dbpsk_bit_cb_t cb, void *user) {
    dbpsk_demod_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->samp_rate = samp_rate;
    d->symbol_rate = symbol_rate;
    d->sps = samp_rate / symbol_rate;
    d->mu = 0.0;
    d->mu_gain = 0.01;   /* conservative timing loop bandwidth */
    d->skip = 0;
    d->prev_sample = 0;
    d->prev_symbol = 0;
    d->accum = 0;
    d->accum_count = 0;
    d->accum_target = (int)(d->sps + 0.5);
    d->cb = cb;
    d->user = user;
    d->num_soft_bits = 0;

    return d;
}

static void flush_bits(dbpsk_demod_t *d) {
    if (d->num_soft_bits > 0 && d->cb) {
        d->cb(d->soft_bits, d->num_soft_bits, d->user);
        d->num_soft_bits = 0;
    }
}

void dbpsk_demod_process(dbpsk_demod_t *d, const float complex *samples,
                          int num_samples) {
    for (int i = 0; i < num_samples; i++) {
        float complex s = samples[i];

        /* Skip samples for timing adjustment */
        if (d->skip > 0) {
            d->skip--;
            d->prev_sample = s;
            continue;
        }

        /* Integrate (matched filter approximation) */
        d->accum += s;
        d->accum_count++;

        if (d->accum_count >= d->accum_target) {
            /* Symbol decision point */
            float complex sym = d->accum / (float)d->accum_count;

            /* Differential decode: multiply current symbol by conjugate
             * of previous symbol. The real part gives the soft bit. */
            float complex diff = sym * conjf(d->prev_symbol);
            float soft = crealf(diff);

            if (d->num_soft_bits >= SOFT_BIT_BUF)
                flush_bits(d);

            d->soft_bits[d->num_soft_bits++] = soft;

            /* Mueller-Muller timing error using midpoint sample */
            float complex mid = d->prev_sample;
            float ted = crealf(sym - d->prev_symbol) * crealf(mid);

            /* Update timing */
            d->mu += d->mu_gain * ted;

            /* Adjust samples per symbol */
            int new_target = (int)(d->sps + d->mu + 0.5);
            if (new_target < 1) new_target = 1;
            d->accum_target = new_target;

            /* Keep mu bounded */
            if (d->mu > 0.5) {
                d->mu -= 1.0;
                d->skip = 1;
            } else if (d->mu < -0.5) {
                d->mu += 1.0;
                /* repeat a sample -- handled by reduced accum_target */
            }

            d->prev_symbol = sym;
            d->accum = 0;
            d->accum_count = 0;
        }

        d->prev_sample = s;
    }

    /* Flush any accumulated bits */
    if (d->num_soft_bits >= SOFT_BIT_BUF / 2)
        flush_bits(d);
}

void dbpsk_demod_destroy(dbpsk_demod_t *d) {
    if (!d) return;
    flush_bits(d);
    free(d);
}
