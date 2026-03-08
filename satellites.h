/*
 * Built-in satellite channel frequency tables
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __SATELLITES_H__
#define __SATELLITES_H__

#include "inmarsat.h"

/* Per-channel definition */
typedef struct {
    double frequency;           /* center frequency in Hz */
    channel_type_t type;        /* channel type (STD-C, Aero 600/1200/8400/10500) */
    int channel_id;             /* channel number within satellite */
} channel_def_t;

/* Per-satellite definition */
typedef struct {
    const char *name;           /* e.g. "I4-F3" */
    const char *designator;     /* e.g. "4F3" */
    double position;            /* orbital position in degrees (negative = west) */
    const char *region;         /* e.g. "AORW" */
    double stdc_egc_freq;       /* STD-C EGC frequency in Hz (0 if unknown) */
    const channel_def_t *channels;
    int num_channels;
    double freq_min;            /* lowest channel frequency */
    double freq_max;            /* highest channel frequency */
} satellite_t;

/* Look up satellite by designator string (e.g. "4F3", "3F5").
 * Returns NULL if not found. */
const satellite_t *satellite_lookup(const char *designator);

/* List all known satellites to stderr. */
void satellite_list(void);

#endif
