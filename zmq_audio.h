/*
 * ZMQ audio output for JAERO compatibility
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __ZMQ_AUDIO_H__
#define __ZMQ_AUDIO_H__

#include <complex.h>

int zmq_audio_init(int base_port);
/* audio_center_hz: target audio frequency for the USB-like mix.
 * JAERO defaults: 1000 Hz for MSK 600/1200, 8000 Hz for OQPSK 10500/8400. */
void zmq_audio_send(int channel_id, const float complex *samples,
                     int num_samples, double samp_rate,
                     double audio_center_hz);
void zmq_audio_cleanup(void);

#endif
