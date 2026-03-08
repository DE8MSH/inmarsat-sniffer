/*
 * inmarsat-sniffer: Inmarsat L-band decoder
 * Decodes STD-C (EGC) and Aero (ACARS) from a single SDR
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <err.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <complex.h>
#include <unistd.h>

#ifdef HAVE_SOAPYSDR
#include "soapysdr.h"
#endif

#ifdef HAVE_SDRPLAY
#include "sdrplay.h"
#endif

#ifdef HAVE_RTLSDR
#include "rtlsdr.h"
#endif

#include "sdr.h"
#include "inmarsat.h"
#include "satellites.h"
#include "options.h"
#include "vita49.h"

#define C_FEK_BLOCKING_QUEUE_IMPLEMENTATION
#define C_FEK_FAIR_LOCK_IMPLEMENTATION
#include "blocking_queue.h"

/* ---- Global configuration ---- */
double samp_rate = 0;
double center_freq = 0;
int ppm_correction = 0;
int verbose = 0;
int live = 0;
iq_format_t iq_format = FMT_CI8;
op_mode_t op_mode = MODE_AUTO;
char *satellite_name = NULL;

#ifdef HAVE_SOAPYSDR
int soapy_num = -1;
char *soapy_args = NULL;
#define SOAPY_SETTINGS_MAX 8
char *soapy_setting_keys[SOAPY_SETTINGS_MAX];
char *soapy_setting_vals[SOAPY_SETTINGS_MAX];
int soapy_setting_count = 0;
#define SOAPY_GAINS_MAX 8
char *soapy_gain_elem_names[SOAPY_GAINS_MAX];
double soapy_gain_elem_vals[SOAPY_GAINS_MAX];
int soapy_gain_elem_count = 0;
#endif

double soapy_gain_val = 40.0;
int bias_tee = 0;
int vita49_enabled = 0;
char *vita49_endpoint = NULL;
int web_enabled = 0;
int web_port = 8888;
int feed_enabled = 0;
#define UDP_MAX 4
char *udp_hosts[UDP_MAX];
int udp_ports[UDP_MAX];
int udp_count = 0;
int basestation_enabled = 0;
char *basestation_endpoint = NULL;
char *aircraft_db_path = NULL;
int update_db_flag = 0;
char *station_id = NULL;
int mqtt_enabled = 0;
char *mqtt_host = NULL;
int mqtt_port = 1883;
char *mqtt_user = NULL;
char *mqtt_pass = NULL;
char *mqtt_topic = NULL;

#ifdef HAVE_SDRPLAY
char *sdrplay_serial = NULL;
int sdrplay_gain_val = -1;
#endif
#ifdef HAVE_RTLSDR
int rtl_dev_index = -1;
#endif
#ifdef HAVE_HACKRF
char *hackrf_serial = NULL;
int hackrf_lna_gain = 40;
int hackrf_vga_gain = 20;
int hackrf_amp_enable = 0;
#endif
#ifdef HAVE_BLADERF
int bladerf_num = -1;
int bladerf_gain_val = 40;
#endif
#ifdef HAVE_UHD
char *usrp_serial = NULL;
int usrp_gain_val = 40;
#endif

FILE *in_file = NULL;
volatile sig_atomic_t running = 1;
pid_t self_pid;

#define SAMPLES_QUEUE_SIZE 2048
#define DECODED_QUEUE_SIZE 256
Blocking_Queue samples_queue;
Blocking_Queue decoded_queue;

atomic_ulong stat_samples_total = 0;
atomic_ulong stat_stdc_frames = 0;
atomic_ulong stat_aero_frames = 0;
atomic_ulong stat_drops = 0;

void push_samples(sample_buf_t *buf) {
    atomic_fetch_add(&stat_samples_total, buf->num);
    if (blocking_queue_add(&samples_queue, buf) == BQ_FULL) {
        atomic_fetch_add(&stat_drops, 1);
        free(buf);
    }
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

static void *spewer_thread(void *arg) {
    FILE *f = (FILE *)arg;
    size_t block = 32768;
    while (running) {
        sample_buf_t *s;
        size_t r;
        switch (iq_format) {
        case FMT_CI8:
            s = malloc(sizeof(*s) + block * 2);
            s->format = SAMPLE_FMT_INT8;
            r = fread(s->samples, 2, block, f);
            break;
        case FMT_CF32:
            s = malloc(sizeof(*s) + block * 8);
            s->format = SAMPLE_FMT_FLOAT;
            r = fread(s->samples, 8, block, f);
            break;
        default:
            s = malloc(sizeof(*s));
            s->format = SAMPLE_FMT_INT8;
            r = 0;
            break;
        }
        if (r == 0) { free(s); break; }
        s->num = r;
        s->hw_timestamp_ns = 0;
        if (blocking_queue_put(&samples_queue, s) != 0) { free(s); break; }
    }
    running = 0;
    kill(self_pid, SIGINT);
    return NULL;
}

int main(int argc, char **argv) {
    self_pid = getpid();
    parse_options(argc, argv);

    fprintf(stderr, "inmarsat-sniffer starting\n");

    if (satellite_name) {
        const satellite_t *sat = satellite_lookup(satellite_name);
        if (!sat)
            errx(1, "Unknown satellite: %s", satellite_name);
        fprintf(stderr, "Satellite: %s (%s)\n", sat->name, sat->region);
    }

    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    blocking_queue_init(&samples_queue, SAMPLES_QUEUE_SIZE);
    blocking_queue_init(&decoded_queue, DECODED_QUEUE_SIZE);

    pthread_t input_tid;
    if (vita49_enabled) {
        pthread_create(&input_tid, NULL, vita49_thread, NULL);
    } else if (live) {
#ifdef HAVE_SDRPLAY
        if (sdrplay_serial) {
            void *ctx = sdrplay_setup(sdrplay_serial);
            pthread_create(&input_tid, NULL, sdrplay_stream_thread, ctx);
        } else
#endif
#ifdef HAVE_SOAPYSDR
        {
            SoapySDRDevice *device;
            if (soapy_args)
                device = soapy_setup(-1, soapy_args);
            else
                device = soapy_setup(soapy_num, NULL);
            pthread_create(&input_tid, NULL, soapy_stream_thread, device);
        }
#else
        errx(1, "No SDR backend available");
#endif
    } else {
        pthread_create(&input_tid, NULL, spewer_thread, in_file);
    }

    /* Main loop — just drain the queue for now */
    while (running) {
        sample_buf_t *buf;
        int ret = blocking_queue_poll(&samples_queue, &buf);
        if (ret == BQ_CLOSED) break;
        if (ret != 0) { usleep(1000); continue; }
        free(buf);
    }

    fprintf(stderr, "\nShutting down...\n");
    blocking_queue_close(&samples_queue);
    blocking_queue_close(&decoded_queue);
    pthread_join(input_tid, NULL);

    return 0;
}
