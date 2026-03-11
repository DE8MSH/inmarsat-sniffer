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
#include "channelizer.h"
#include "demod_dbpsk.h"
#include "stdc_decode.h"
#include "vita49.h"
#include "simd_kernels.h"

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
atomic_ulong stat_stdc_crc_ok = 0;
atomic_ulong stat_stdc_crc_fail = 0;
atomic_ulong stat_stdc_ber_sum = 0;
atomic_ulong stat_stdc_ber_count = 0;
atomic_int  stat_stdc_synced = 0;

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
        case FMT_CU8: {
            s = malloc(sizeof(*s) + block * 2);
            s->format = SAMPLE_FMT_INT8;
            uint8_t *tmp = malloc(block * 2);
            r = fread(tmp, 2, block, f);
            for (size_t i = 0; i < r * 2; i++)
                s->samples[i] = (int8_t)(tmp[i] - 128);
            free(tmp);
            break;
        }
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
    while (running && samples_queue.queue_size > 0)
        usleep(10000);
    running = 0;
    kill(self_pid, SIGINT);
    return NULL;
}

static void print_status(void) {
    unsigned long stdc = atomic_load(&stat_stdc_frames);
    unsigned long drops = atomic_load(&stat_drops);
    unsigned long sc_ok = atomic_load(&stat_stdc_crc_ok);
    unsigned long sc_fail = atomic_load(&stat_stdc_crc_fail);
    unsigned long sb_sum = atomic_load(&stat_stdc_ber_sum);
    unsigned long sb_cnt = atomic_load(&stat_stdc_ber_count);
    int synced = atomic_load(&stat_stdc_synced);
    float stdc_ber = sb_cnt ? (float)sb_sum / (sb_cnt * 10000.0f) : 0;
    fprintf(stderr, "\r[STD-C: %lu %s BER:%.2f CRC:%lu/%lu | drop:%lu]   ",
            stdc, synced ? "SYNC" : "SRCH",
            stdc_ber, sc_ok, sc_ok + sc_fail, drops);
}

static dbpsk_demod_t *stdc_demod = NULL;
static stdc_decoder_t *stdc_decoder = NULL;
static channelizer_t *channelizer = NULL;

static const char *get_stdc_type_str(stdc_msg_type_t type) {
    switch (type) {
    case STDC_MSG_EGC_SINGLE:
    case STDC_MSG_EGC_DOUBLE_1:
    case STDC_MSG_EGC_DOUBLE_2:     return "EGC";
    case STDC_MSG_BULLETIN:          return "Bulletin";
    case STDC_MSG_ANNOUNCEMENT:      return "Announcement";
    default:                         return "STD-C";
    }
}

static void stdc_message_cb(const stdc_message_t *msg, void *user) {
    (void)user;
    const char *type_str = get_stdc_type_str(msg->type);
    fprintf(stderr, "\n[%s] %s\n", type_str, msg->text);
}

static void stdc_bits_cb(const float *soft_bits, int num_bits, void *user) {
    (void)user;
    if (stdc_decoder)
        stdc_decoder_feed(stdc_decoder, soft_bits, num_bits);
}

static void channel_output_cb(int channel_id, channel_type_t type,
                                float complex *samples, int num_samples,
                                void *user) {
    (void)user; (void)channel_id;
    if (type == CHAN_STDC_EGC && stdc_demod) {
        dbpsk_demod_process(stdc_demod, samples, num_samples);
    }
}

int main(int argc, char **argv) {
    self_pid = getpid();
    parse_options(argc, argv);

    simd_init(0);

    fprintf(stderr, "inmarsat-sniffer starting\n");

    const satellite_t *sat = NULL;
    if (satellite_name) {
        sat = satellite_lookup(satellite_name);
        if (!sat)
            errx(1, "Unknown satellite: %s", satellite_name);
        fprintf(stderr, "Satellite: %s (%s)\n", sat->name, sat->region);

        double lo = 1e12, hi = 0;
        for (int i = 0; i < sat->num_channels; i++) {
            if (sat->channels[i].frequency < lo) lo = sat->channels[i].frequency;
            if (sat->channels[i].frequency > hi) hi = sat->channels[i].frequency;
        }
        if (center_freq == 0)
            center_freq = (lo + hi) / 2.0;
        if (samp_rate == 0) {
            samp_rate = (hi - lo) * 1.2;
            if (samp_rate < 2400000) samp_rate = 2400000;
        }
    }
    if (samp_rate == 0) samp_rate = 2400000;
    if (center_freq == 0) center_freq = 1545100000.0;

    fprintf(stderr, "Center: %.3f MHz  Rate: %.3f MHz\n",
            center_freq / 1e6, samp_rate / 1e6);

    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    blocking_queue_init(&samples_queue, SAMPLES_QUEUE_SIZE);
    blocking_queue_init(&decoded_queue, DECODED_QUEUE_SIZE);

    pthread_t input_tid;
    void *rtl_dev = NULL;
    if (vita49_enabled) {
        pthread_create(&input_tid, NULL, vita49_thread, NULL);
    } else if (live) {
#ifdef HAVE_RTLSDR
        if (rtl_dev_index >= 0) {
            rtl_dev = rtlsdr_backend_setup(rtl_dev_index);
            pthread_create(&input_tid, NULL, rtlsdr_stream_thread, rtl_dev);
        } else
#endif
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

    if (sat) {
        channelizer = channelizer_create(center_freq, samp_rate,
                                          channel_output_cb, NULL);
        if (!channelizer)
            errx(1, "Failed to create channelizer");

        for (int i = 0; i < sat->num_channels; i++) {
            const channel_def_t *cd = &sat->channels[i];
            double offset = fabs(cd->frequency - center_freq);
            if (offset > samp_rate / 2.0) continue;
            if (op_mode == MODE_AERO && cd->type == CHAN_STDC_EGC) continue;
            if (op_mode == MODE_STDC && cd->type != CHAN_STDC_EGC) continue;
            channelizer_add_channel(channelizer, cd->frequency,
                                     cd->type, cd->channel_id);
        }

        /* Initialize STD-C demod/decode chain */
        for (int i = 0; i < sat->num_channels; i++) {
            if (sat->channels[i].type == CHAN_STDC_EGC &&
                (op_mode != MODE_AERO)) {
                double output_rate = samp_rate / (int)(samp_rate / (1200.0 * 4.0));
                stdc_decoder = stdc_decoder_create(stdc_message_cb, NULL);
                stdc_demod = dbpsk_demod_create(output_rate, 1200.0,
                                                  stdc_bits_cb, NULL);
                if (stdc_demod && stdc_decoder)
                    fprintf(stderr, "STD-C EGC decoder: active\n");
                break;
            }
        }
    }

    unsigned long status_interval = 0;
    while (running) {
        sample_buf_t *buf;
        int ret = blocking_queue_poll(&samples_queue, &buf);
        if (ret == BQ_CLOSED) break;
        if (ret != 0) { usleep(1000); continue; }

        if (channelizer) {
            if (buf->format == SAMPLE_FMT_INT8)
                channelizer_process_i8(channelizer, buf->samples, buf->num);
            else
                channelizer_process(channelizer, (float *)buf->samples, buf->num);
        }
        free(buf);

        if (++status_interval % 100 == 0)
            print_status();
    }

    fprintf(stderr, "\nShutting down...\n");

#ifdef HAVE_RTLSDR
    if (rtl_dev)
        rtlsdr_backend_close(rtl_dev);
#endif

    blocking_queue_close(&samples_queue);
    blocking_queue_close(&decoded_queue);
    pthread_join(input_tid, NULL);

    dbpsk_demod_destroy(stdc_demod);
    stdc_decoder_destroy(stdc_decoder);
    channelizer_destroy(channelizer);

    return 0;
}
