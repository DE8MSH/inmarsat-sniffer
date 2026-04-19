/*
 * ZMQ audio output for JAERO compatibility
 *
 * Publishes per-channel audio as int16 PCM via ZMQ PUB sockets,
 * compatible with SDRReceiver / gr-JAERO format:
 *   Frame: [topic string] [uint32 rate] [int16[] samples]
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
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

typedef struct {
    void *socket;
    double mixer_phase;
    double mixer_inc;
    double audio_center;
    int active;
    int channel_id;
    char topic[32];
} zmq_chan_t;

static void *zmq_ctx = NULL;
static void *zmq_pub = NULL;       /* single PUB socket for all channels */
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
    fprintf(stderr, "ZMQ audio: tcp://127.0.0.1:%d "
            "(one port, topics VFO01..VFOnn)\n", base_port);
    return 0;
}

static zmq_chan_t *get_channel(int channel_id, double samp_rate, double audio_center_hz)
{
    for (int i = 0; i < num_channels; i++) {
        if (channels[i].channel_id == channel_id)
            return &channels[i];
    }

    if (num_channels >= MAX_ZMQ_CHANNELS) return NULL;

    zmq_chan_t *ch = &channels[num_channels++];
    ch->channel_id = channel_id;
    ch->audio_center = audio_center_hz;
    ch->mixer_phase = 0;
    ch->mixer_inc = 2.0 * M_PI * audio_center_hz / samp_rate;
    ch->active = 1;
    ch->socket = zmq_pub;  /* all channels share one socket */
    snprintf(ch->topic, sizeof(ch->topic), "VFO%02d", channel_id);
    fprintf(stderr, "ZMQ: channel %d topic=%s audio_center=%.0f Hz\n",
            channel_id, ch->topic, audio_center_hz);
    return ch;
}

void zmq_audio_send(int channel_id, const float complex *samples,
                     int num_samples, double samp_rate,
                     double audio_center_hz)
{
    zmq_chan_t *ch = get_channel(channel_id, samp_rate, audio_center_hz);
    if (!ch || !ch->socket) return;

    /* Convert complex baseband to real audio at audio_center_hz */
    int16_t *pcm = malloc(num_samples * sizeof(int16_t));
    if (!pcm) return;

    for (int i = 0; i < num_samples; i++) {
        double ca = cos(ch->mixer_phase);
        double sa = sin(ch->mixer_phase);
        float complex mixer = (float)ca + (float)sa * I;
        double audio = creal((double complex)samples[i] * mixer);
        ch->mixer_phase += ch->mixer_inc;
        if (ch->mixer_phase > 2.0 * M_PI)
            ch->mixer_phase -= 2.0 * M_PI;

        /* Scale to int16 (SDRReceiver uses gain=5 for 600/1200 baud) */
        double scaled = audio * 5.0 * 32768.0;
        if (scaled > 32767.0) scaled = 32767.0;
        if (scaled < -32768.0) scaled = -32768.0;
        pcm[i] = (int16_t)scaled;
    }

    /* Send: [topic] [rate] [samples] */
    uint32_t rate = (uint32_t)samp_rate;
    zmq_send(ch->socket, ch->topic, strlen(ch->topic), ZMQ_SNDMORE);
    zmq_send(ch->socket, &rate, sizeof(rate), ZMQ_SNDMORE);
    zmq_send(ch->socket, pcm, num_samples * sizeof(int16_t), 0);

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
