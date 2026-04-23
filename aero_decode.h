/*
 * Aero ACARS decoded-message struct
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AERO_DECODE_H
#define AERO_DECODE_H

#include <stdint.h>

typedef struct {
    char reg[16];
    char flight[16];
    char label[4];
    char mode;
    char block_id;
    char ack;
    double lat, lon;
    int alt_ft;
    int has_position;
    char text[4096];
    int text_len;
    int channel_id;
    uint64_t timestamp;
    uint32_t aes_id;
    uint8_t ges_id;
    uint8_t qno;
    uint8_t refno;
    int downlink;              /* 1 = aircraft->ground, 0 = ground->aircraft */
    const char *arinc622_json; /* libacars proto-tree JSON, or NULL */
    const uint8_t *raw_data;
    int raw_len;
} aero_message_t;

#endif
