/*
 * DBPSK demodulator for STD-C / EGC (1200 baud)
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __DEMOD_DBPSK_H__
#define __DEMOD_DBPSK_H__

#include <complex.h>
#include <stdint.h>

/* Soft bit callback: delivers demodulated soft bits.
 * Positive = 1, negative = 0. Magnitude indicates confidence. */
typedef void (*dbpsk_bit_cb_t)(const float *soft_bits, int num_bits,
                                void *user);

typedef struct dbpsk_demod dbpsk_demod_t;

/* Create a DBPSK demodulator.
 * samp_rate: input sample rate (from channelizer)
 * symbol_rate: baud rate (1200 for STD-C)
 * cb: callback for demodulated soft bits */
dbpsk_demod_t *dbpsk_demod_create(double samp_rate, double symbol_rate,
                                   dbpsk_bit_cb_t cb, void *user);

/* Process baseband IQ samples from channelizer. */
void dbpsk_demod_process(dbpsk_demod_t *d, const float complex *samples,
                          int num_samples);

/* Destroy the demodulator. */
void dbpsk_demod_destroy(dbpsk_demod_t *d);

#endif
