/*
 * DDC channelizer -- per-channel digital downconversion
 *
 * Mixes each known channel to baseband via NCO, lowpass filters,
 * and decimates to the minimum sample rate needed by the demodulator.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __CHANNELIZER_H__
#define __CHANNELIZER_H__

#include <stdint.h>
#include <complex.h>
#include "inmarsat.h"

/* Maximum number of simultaneous channels */
#define MAX_CHANNELS 32

/* Per-channel output callback. Called with baseband IQ samples
 * at the decimated rate whenever a block is ready. */
typedef void (*channel_cb_t)(int channel_id, channel_type_t type,
                              float complex *samples, int num_samples,
                              void *user);

/* Channelizer state (opaque) */
typedef struct channelizer channelizer_t;

/* Create a new channelizer.
 * center_freq: SDR center frequency in Hz
 * samp_rate: SDR sample rate in Hz
 * cb: callback invoked with per-channel baseband samples
 * user: passed to callback */
channelizer_t *channelizer_create(double center_freq, double samp_rate,
                                   channel_cb_t cb, void *user);

/* Add a channel to the channelizer.
 * freq: channel center frequency in Hz
 * type: channel type (determines output bandwidth and decimation)
 * channel_id: user-defined channel identifier
 * Returns 0 on success. */
int channelizer_add_channel(channelizer_t *ch, double freq,
                             channel_type_t type, int channel_id);

/* Process a block of wideband IQ samples from the SDR.
 * samples: interleaved float IQ (num_samples complex pairs)
 * num_samples: number of complex samples */
void channelizer_process(channelizer_t *ch, const float *samples,
                          int num_samples);

/* Process int8 IQ samples (converted internally). */
void channelizer_process_i8(channelizer_t *ch, const int8_t *samples,
                             int num_samples);

/* Check if a frequency is already covered by an existing channel.
 * Returns 1 if a channel within tolerance Hz already exists. */
int channelizer_has_freq(channelizer_t *ch, double freq, double tolerance);

/* Get actual output sample rate for a channel after decimation. */
double channelizer_output_rate(channelizer_t *ch, int channel_id);

/* Destroy the channelizer and free resources. */
void channelizer_destroy(channelizer_t *ch);

#endif
