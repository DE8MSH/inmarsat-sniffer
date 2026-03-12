/*
 * Aero frame parser and ACARS message extraction
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __AERO_DECODE_H__
#define __AERO_DECODE_H__

#include <stdint.h>

/* Decoded Aero ACARS message */
typedef struct {
    char reg[16];           /* aircraft registration */
    char flight[16];        /* flight number */
    char label[4];          /* ACARS label (2 chars) */
    char mode;              /* ACARS mode character */
    char block_id;          /* block identifier */
    char ack;               /* acknowledgement character */
    double lat, lon;        /* position (NaN if unknown) */
    int alt_ft;             /* altitude in feet (-1 if unknown) */
    int has_position;
    char text[4096];        /* message text */
    int text_len;
    int channel_id;
    uint64_t timestamp;
    /* Raw ACARS frame for libacars parsing */
    const uint8_t *raw_data;
    int raw_len;
} aero_message_t;

/* Callback for decoded Aero messages */
typedef void (*aero_msg_cb_t)(const aero_message_t *msg, void *user);

/* Aero decoder state */
typedef struct aero_decoder aero_decoder_t;

/* Create an Aero decoder.
 * cb: callback for decoded messages */
aero_decoder_t *aero_decoder_create(aero_msg_cb_t cb, void *user);

/* Feed soft bits from BPSK or OQPSK demodulator.
 * channel_id: which channel these bits came from
 * baud_rate: 600, 1200, 8400, or 10500 */
void aero_decoder_feed(aero_decoder_t *d, const float *soft_bits,
                        int num_bits, int channel_id, int baud_rate);

/* Destroy the decoder. */
void aero_decoder_destroy(aero_decoder_t *d);

#endif
