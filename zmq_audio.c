/*
 * ZMQ audio output — SDRReceiver-compatible per-channel USB audio for JAERO.
 * Frame: [topic] [uint32 rate] [int16 samples]. Hilbert FIR + delay line
 * port from SDRReceiver (MIT, Jeroen Beijer).
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * Portions Copyright (c) 2021 Jeroen Beijer (SDRReceiver, MIT)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <complex.h>
#include <zmq.h>

#include "zmq_audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_ZMQ_CHANNELS 32
#define HILBERT_TAPS     125
#define DELAY_SAMPLES    ((HILBERT_TAPS - 1) / 2)   /* 62 */

/* 125-tap Hilbert FIR coefficients, computed once and shared across
 * channels (same formula as SDRReceiver's FIRHilbert constructor). */
static float hilbert_taps[HILBERT_TAPS];
static int hilbert_designed = 0;

static void design_hilbert(void)
{
    if (hilbert_designed) return;
    int len = HILBERT_TAPS;
    float tmp[HILBERT_TAPS];
    float sumsq = 0;
    for (int n = 0; n < len; n++) {
        if (n == len / 2) {
            tmp[n] = 0;
        } else {
            double x = M_PI * (n - len / 2);
            tmp[n] = (float)((1.0 - cos(x)) / x);
        }
        sumsq += tmp[n] * tmp[n];
    }
    float gain = sqrtf(sumsq);
    /* SDRReceiver reverses the coefficients during load */
    for (int i = 0; i < len; i++)
        hilbert_taps[i] = tmp[len - i - 1] / gain;
    hilbert_designed = 1;
}

typedef struct {
    void *socket;
    int active;
    int channel_id;
    char topic[32];

    /* BFO oscillator — shifts baseband to audio_center_hz */
    double bfo_phase;
    double bfo_inc;
    double audio_center;

    /* Hilbert filter state (per channel, fresh history) */
    float  hilb_hist[HILBERT_TAPS];
    int    hilb_idx;

    /* Delay line for real part — matches Hilbert group delay */
    float  delay_buf[DELAY_SAMPLES + 1];
    int    delay_idx;

    /* Output gain (SDRReceiver: gain / 100 from ini config).
     * MSK channels default 0.05, OQPSK 0.03. */
    double gain;
} zmq_chan_t;

static void *zmq_ctx = NULL;
static void *zmq_pub = NULL;
static zmq_chan_t channels[MAX_ZMQ_CHANNELS];
static int num_channels = 0;

int zmq_audio_init(int base_port)
{
    zmq_ctx = zmq_ctx_new();
    if (!zmq_ctx) return -1;

    zmq_pub = zmq_socket(zmq_ctx, ZMQ_PUB);
    if (!zmq_pub) return -1;

    char addr[64];
    snprintf(addr, sizeof(addr), "tcp://0.0.0.0:%d", base_port);
    if (zmq_bind(zmq_pub, addr) != 0) {
        fprintf(stderr, "ZMQ: failed to bind %s\n", addr);
        return -1;
    }

    memset(channels, 0, sizeof(channels));
    design_hilbert();
    fprintf(stderr, "ZMQ audio: tcp://127.0.0.1:%d "
            "(topics VFO01..VFOnn, SDRReceiver-compatible Hilbert USB)\n",
            base_port);
    return 0;
}

static zmq_chan_t *get_channel(int channel_id, double samp_rate,
                                double audio_center_hz)
{
    for (int i = 0; i < num_channels; i++) {
        if (channels[i].channel_id == channel_id)
            return &channels[i];
    }

    if (num_channels >= MAX_ZMQ_CHANNELS) return NULL;

    zmq_chan_t *ch = &channels[num_channels++];
    memset(ch, 0, sizeof(*ch));
    ch->channel_id = channel_id;
    ch->audio_center = audio_center_hz;
    ch->bfo_phase = 0;
    ch->bfo_inc = 2.0 * M_PI * audio_center_hz / samp_rate;
    ch->active = 1;
    ch->socket = zmq_pub;
    ch->hilb_idx = 0;
    ch->delay_idx = 0;
    /* Empirical gain to match SDRReceiver's int16 audio level into JAERO. */
    ch->gain = (audio_center_hz >= 4000.0) ? 3.0 : 5.0;
    snprintf(ch->topic, sizeof(ch->topic), "VFO%02d", channel_id);
    extern int verbose;
    if (verbose)
        fprintf(stderr, "ZMQ: channel %d topic=%s audio_center=%.0f Hz gain=%.3f\n",
                channel_id, ch->topic, audio_center_hz, ch->gain);
    return ch;
}

/* 125-tap Hilbert FIR convolution on the imaginary stream */
static inline float hilbert_step(zmq_chan_t *ch, float im)
{
    ch->hilb_hist[ch->hilb_idx] = im;
    ch->hilb_idx = (ch->hilb_idx + 1) % HILBERT_TAPS;

    float sum = 0;
    int idx = ch->hilb_idx;
    for (int i = 0; i < HILBERT_TAPS; i++) {
        sum += hilbert_taps[i] * ch->hilb_hist[idx];
        idx = (idx + 1) % HILBERT_TAPS;
    }
    return sum;
}

/* Matching delay line for the real stream (62 samples) */
static inline float delay_step(zmq_chan_t *ch, float re)
{
    ch->delay_buf[ch->delay_idx] = re;
    ch->delay_idx = (ch->delay_idx + 1) % (DELAY_SAMPLES + 1);
    return ch->delay_buf[ch->delay_idx];
}

void zmq_audio_send(int channel_id, const float complex *samples,
                     int num_samples, double samp_rate,
                     double audio_center_hz)
{
    zmq_chan_t *ch = get_channel(channel_id, samp_rate, audio_center_hz);
    if (!ch || !ch->socket) return;

    int16_t *pcm = malloc(num_samples * sizeof(int16_t));
    if (!pcm) return;

    for (int i = 0; i < num_samples; i++) {
        float re = crealf(samples[i]);
        float im = cimagf(samples[i]);

        /* BFO mix: shift complex baseband to audio_center_hz */
        if (ch->bfo_inc != 0.0) {
            float ca = (float)cos(ch->bfo_phase);
            float sa = (float)sin(ch->bfo_phase);
            float new_re = re * ca - im * sa;
            float new_im = re * sa + im * ca;
            re = new_re;
            im = new_im;
            ch->bfo_phase += ch->bfo_inc;
            if (ch->bfo_phase > 2.0 * M_PI) ch->bfo_phase -= 2.0 * M_PI;
        }

        /* USB demod: audio = delay(re) - hilbert(im) */
        float delayed_re = delay_step(ch, re);
        float hilb_im    = hilbert_step(ch, im);
        float usb = delayed_re - hilb_im;

        /* Scale to int16 (SDRReceiver: audio * gain * 32768) */
        double scaled = (double)usb * ch->gain * 32768.0;
        if (scaled >  32767.0) scaled =  32767.0;
        if (scaled < -32768.0) scaled = -32768.0;
        pcm[i] = (int16_t)scaled;
    }

    /* [topic][rate][samples]. DONTWAIT so subscriber churn never blocks
     * the channelizer thread. */
    uint32_t rate = (uint32_t)samp_rate;
    zmq_send(ch->socket, ch->topic, strlen(ch->topic), ZMQ_SNDMORE | ZMQ_DONTWAIT);
    zmq_send(ch->socket, &rate, sizeof(rate), ZMQ_SNDMORE | ZMQ_DONTWAIT);
    zmq_send(ch->socket, pcm, num_samples * sizeof(int16_t), ZMQ_DONTWAIT);

    free(pcm);
}

void zmq_audio_cleanup(void)
{
    if (zmq_pub) {
        zmq_close(zmq_pub);
        zmq_pub = NULL;
    }
    if (zmq_ctx) {
        zmq_ctx_destroy(zmq_ctx);
        zmq_ctx = NULL;
    }
}
