/*
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDR_H
#define SDR_H

#include <stdint.h>

#define SAMPLE_FMT_INT8   0
#define SAMPLE_FMT_FLOAT  1

/* Clock/time source configuration for SDR backends */
#define CLOCK_SRC_INTERNAL  0
#define CLOCK_SRC_EXTERNAL  1
#define CLOCK_SRC_GPSDO     2

typedef struct _sample_buf_t {
    unsigned num;
    int format;           /* SAMPLE_FMT_INT8 or SAMPLE_FMT_FLOAT */
    uint64_t hw_timestamp_ns;  /* hardware timestamp in ns (0 = not available) */
    int8_t samples[];     /* for SAMPLE_FMT_FLOAT: cast to float* (4x larger) */
} sample_buf_t;

void push_samples(sample_buf_t *buf);

#endif
