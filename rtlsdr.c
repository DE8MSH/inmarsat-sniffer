/*
 * RTL-SDR native backend for inmarsat-sniffer
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <err.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rtl-sdr.h>

#include "sdr.h"
#include "inmarsat.h"

extern double samp_rate;
extern double center_freq;
extern double soapy_gain_val;
extern int bias_tee;
extern int verbose;
extern volatile sig_atomic_t running;

extern void push_samples(sample_buf_t *buf);

/* ---- Device listing ---- */

void rtlsdr_backend_list(void) {
    uint32_t count = rtlsdr_get_device_count();
    if (count == 0) {
        printf("  (no RTL-SDR devices found)\n");
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        char manufact[256], product[256], serial[256];
        rtlsdr_get_device_usb_strings(i, manufact, product, serial);
        printf("  rtl-%u    %s %s (serial: %s)\n", i, manufact, product, serial);
    }
}

/* ---- Setup ---- */

void *rtlsdr_backend_setup(int dev_index) {
    rtlsdr_dev_t *dev = NULL;
    int r;

    uint32_t count = rtlsdr_get_device_count();
    if (count == 0)
        errx(1, "No RTL-SDR devices found");

    if (dev_index < 0 || (uint32_t)dev_index >= count)
        errx(1, "RTL-SDR device index %d out of range (0-%u)", dev_index, count - 1);

    fprintf(stderr, "RTL-SDR: opening device %d (%s)\n",
            dev_index, rtlsdr_get_device_name((uint32_t)dev_index));

    r = rtlsdr_open(&dev, (uint32_t)dev_index);
    if (r < 0)
        errx(1, "Failed to open RTL-SDR device %d", dev_index);

    /* Sample rate */
    r = rtlsdr_set_sample_rate(dev, (uint32_t)samp_rate);
    if (r < 0)
        warnx("RTL-SDR: failed to set sample rate %.0f Hz", samp_rate);

    uint32_t actual_rate = rtlsdr_get_sample_rate(dev);
    if (actual_rate != (uint32_t)samp_rate) {
        fprintf(stderr, "RTL-SDR: actual sample rate: %u Hz\n", actual_rate);
        samp_rate = (double)actual_rate;
    }

    /* PPM correction */
    extern int ppm_correction;
    if (ppm_correction != 0) {
        rtlsdr_set_freq_correction(dev, ppm_correction);
        fprintf(stderr, "RTL-SDR: PPM correction: %d\n", ppm_correction);
    }

    /* Center frequency */
    r = rtlsdr_set_center_freq(dev, (uint32_t)center_freq);
    if (r < 0)
        warnx("RTL-SDR: failed to set center freq %.0f Hz", center_freq);

    fprintf(stderr, "RTL-SDR: tuned to %.3f MHz @ %.3f Msps\n",
            center_freq / 1e6, samp_rate / 1e6);

    /* Gain: manual mode, set from --soapy-gain value (reused) */
    rtlsdr_set_tuner_gain_mode(dev, 1);

    /* Find nearest supported gain value */
    int num_gains = rtlsdr_get_tuner_gains(dev, NULL);
    if (num_gains > 0) {
        int *gains = malloc((size_t)num_gains * sizeof(int));
        rtlsdr_get_tuner_gains(dev, gains);

        int target = (int)(soapy_gain_val * 10.0);  /* tenths of dB */
        int best = gains[0];
        int best_diff = abs(target - gains[0]);
        for (int i = 1; i < num_gains; i++) {
            int diff = abs(target - gains[i]);
            if (diff < best_diff) {
                best = gains[i];
                best_diff = diff;
            }
        }
        free(gains);

        rtlsdr_set_tuner_gain(dev, best);
        fprintf(stderr, "RTL-SDR: gain set to %.1f dB\n", best / 10.0);
    }

    /* Bias tee */
    if (bias_tee) {
        rtlsdr_set_bias_tee(dev, 1);
        fprintf(stderr, "RTL-SDR: bias tee enabled\n");
    }

    /* Reset buffer before streaming */
    rtlsdr_reset_buffer(dev);

    return dev;
}

/* ---- Async streaming callback ---- */

static void rtlsdr_async_cb(unsigned char *buf, uint32_t len, void *ctx) {
    (void)ctx;

    if (!running)
        return;

    /* RTL-SDR gives unsigned 8-bit IQ pairs (center at 128).
     * Convert to signed int8 by subtracting 128 (XOR 0x80). */
    sample_buf_t *s = malloc(sizeof(*s) + len);
    if (!s)
        return;

    s->format = SAMPLE_FMT_INT8;
    s->num = len / 2;  /* IQ pairs */
    s->hw_timestamp_ns = 0;

    for (uint32_t i = 0; i < len; i++)
        s->samples[i] = (int8_t)(buf[i] - 128);

    push_samples(s);
}

/* ---- Streaming thread ---- */

void *rtlsdr_stream_thread(void *arg) {
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)arg;

    /* rtlsdr_read_async blocks until rtlsdr_cancel_async is called */
    rtlsdr_read_async(dev, rtlsdr_async_cb, NULL, 0, 0);

    running = 0;
    return NULL;
}

/* ---- Cleanup ---- */

void rtlsdr_backend_close(void *ctx) {
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    if (dev) {
        rtlsdr_cancel_async(dev);
        rtlsdr_close(dev);
    }
}
