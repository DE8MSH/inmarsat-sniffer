/*
 * Inmarsat L-band constants and common definitions
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __INMARSAT_H__
#define __INMARSAT_H__

/* L-band downlink range */
#define INMARSAT_L_BAND_LOW    1525000000.0
#define INMARSAT_L_BAND_HIGH   1559000000.0

/* Operating modes */
typedef enum {
    MODE_AUTO = 0,     /* auto-select based on SDR bandwidth */
    MODE_AERO,         /* Aero channels only */
    MODE_STDC,         /* STD-C / EGC only */
    MODE_FULL,         /* Aero + STD-C simultaneously */
} op_mode_t;

/* IQ file format */
typedef enum {
    FMT_CI8 = 0,       /* interleaved int8 (signed) */
    FMT_CU8,           /* interleaved uint8 (RTL-SDR raw, center 128) */
    FMT_CI16,           /* interleaved int16 */
    FMT_CF32,           /* interleaved float32 */
} iq_format_t;

/* Channel type identifiers */
typedef enum {
    CHAN_STDC_EGC = 0,  /* STD-C Enhanced Group Call */
    CHAN_AERO_600,      /* Aero 600 baud BPSK */
    CHAN_AERO_1200,     /* Aero 1200 baud BPSK */
    CHAN_AERO_10500,    /* Aero 10500 baud OQPSK */
    CHAN_AERO_8400,     /* Aero 8400 baud OQPSK */
} channel_type_t;

/* Satellite identifiers */
typedef enum {
    SAT_4F3 = 0,       /* I4-F3, 98W, AORW */
    SAT_3F5,           /* I3-F5, 54W, AORE */
    SAT_AF1,           /* I4-AF1, 25E, IOR */
    SAT_F1,            /* I4-F1, 143.5E, POR */
    SAT_COUNT,
} satellite_id_t;

#endif
