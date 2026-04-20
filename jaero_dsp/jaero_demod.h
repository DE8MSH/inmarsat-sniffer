/*
 * jaero_demod.h -- C API for JAERO burst demodulators
 *
 * DSP code originally from JAERO by Jonathan Olds, MIT license.
 * Stripped of Qt dependencies and wrapped for use as a plain C/C++ library.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2015 Jonathan Olds (original JAERO DSP)
 */
#ifndef JAERO_DEMOD_H
#define JAERO_DEMOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*jaero_soft_bits_cb)(const unsigned char *bits, int num_bits, int channel_id, void *user);
typedef void (*jaero_acars_cb)(const uint8_t *data, int len, int channel_id,
                               uint32_t aes_id, uint8_t ges_id, void *user);

/* C-channel assignment (voice/data session setup). Ground station tells
 * aircraft to use a specific RX/TX frequency for a call. */
typedef void (*jaero_cassign_cb)(int channel_id, uint8_t type,
                                 uint32_t aes_id, uint8_t ges_id,
                                 double rx_mhz, double tx_mhz,
                                 void *user);

typedef struct jaero_msk_demod jaero_msk_demod_t;
typedef struct jaero_oqpsk_demod jaero_oqpsk_demod_t;
typedef struct jaero_pmsk_demod jaero_pmsk_demod_t;  /* continuous MSK for P-channel */
typedef struct jaero_oqpsk_cont_demod jaero_oqpsk_cont_demod_t; /* continuous OQPSK for H/H+/L */

/* --- Burst MSK demodulator (R/T channel bursts, 600/1200 baud) --- */
jaero_msk_demod_t *jaero_msk_create(double sample_rate, double symbol_rate, int channel_id, jaero_soft_bits_cb cb, void *user);
void jaero_msk_feed(jaero_msk_demod_t *d, const int16_t *audio, int num_samples);
void jaero_msk_feed_iq(jaero_msk_demod_t *d, const double *iq_interleaved, int num_samples);
void jaero_msk_feed_soft_bits(jaero_msk_demod_t *d, const short *bits, int num_bits);
void jaero_msk_destroy(jaero_msk_demod_t *d);
void jaero_msk_set_acars_callback(jaero_msk_demod_t *d, jaero_acars_cb cb, void *user);

/* --- Continuous MSK demodulator (P-channel, 600/1200 baud) --- */
jaero_pmsk_demod_t *jaero_pmsk_create(double sample_rate, double symbol_rate, int channel_id, jaero_soft_bits_cb cb, void *user);
void jaero_pmsk_feed_iq(jaero_pmsk_demod_t *d, const double *iq_interleaved, int num_samples);
void jaero_pmsk_feed_audio(jaero_pmsk_demod_t *d, const int16_t *audio, int num_samples);
void jaero_pmsk_destroy(jaero_pmsk_demod_t *d);
void jaero_pmsk_set_acars_callback(jaero_pmsk_demod_t *d, jaero_acars_cb cb, void *user);
void jaero_pmsk_set_cassign_callback(jaero_pmsk_demod_t *d, jaero_cassign_cb cb, void *user);
double jaero_pmsk_get_mse(jaero_pmsk_demod_t *d);
double jaero_pmsk_get_ebno(jaero_pmsk_demod_t *d);
int    jaero_pmsk_is_locked(jaero_pmsk_demod_t *d);

/* Lightweight AeroL-only decoder (no MSK demod, just frame decode).
 * Feed soft bits from an external demod via jaero_msk_feed_soft_bits(). */
jaero_msk_demod_t *jaero_aerol_create(double symbol_rate, int channel_id,
                                       jaero_acars_cb acars_cb, void *user);

jaero_oqpsk_demod_t *jaero_oqpsk_create(double sample_rate, double symbol_rate, int channel_id, jaero_soft_bits_cb cb, void *user);
void jaero_oqpsk_feed(jaero_oqpsk_demod_t *d, const int16_t *audio, int num_samples);
void jaero_oqpsk_destroy(jaero_oqpsk_demod_t *d);
void jaero_oqpsk_set_acars_callback(jaero_oqpsk_demod_t *d, jaero_acars_cb cb, void *user);

/* --- Continuous OQPSK demodulator (Aero H/H+/L, 10500 baud forward link) --- */
jaero_oqpsk_cont_demod_t *jaero_oqpsk_cont_create(double sample_rate, double symbol_rate, int channel_id, jaero_soft_bits_cb cb, void *user);
void jaero_oqpsk_cont_feed_audio(jaero_oqpsk_cont_demod_t *d, const int16_t *audio, int num_samples);
void jaero_oqpsk_cont_feed_iq(jaero_oqpsk_cont_demod_t *d, const double *iq_interleaved, int num_samples);
void jaero_oqpsk_cont_destroy(jaero_oqpsk_cont_demod_t *d);
void jaero_oqpsk_cont_set_acars_callback(jaero_oqpsk_cont_demod_t *d, jaero_acars_cb cb, void *user);
void jaero_oqpsk_cont_set_cassign_callback(jaero_oqpsk_cont_demod_t *d, jaero_cassign_cb cb, void *user);
double jaero_oqpsk_cont_get_mse(jaero_oqpsk_cont_demod_t *d);
double jaero_oqpsk_cont_get_ebno(jaero_oqpsk_cont_demod_t *d);
int    jaero_oqpsk_cont_is_locked(jaero_oqpsk_cont_demod_t *d);

#ifdef __cplusplus
}
#endif

#endif /* JAERO_DEMOD_H */
