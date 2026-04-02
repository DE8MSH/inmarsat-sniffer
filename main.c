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

#ifdef HAVE_ZMQ
#include "zmq_audio.h"
int zmq_enabled = 0;
int zmq_base_port = 6001;
#endif

#include "jaero_dsp/jaero_demod.h"
#define MAX_JAERO_DEMODS 32
#define AUDIO_CENTER_HZ 8000.0
#define PMSK_AUDIO_HZ   1000.0
#define AUDIO_GAIN 5.0

#define CHAN_RING_SIZE (1 << 18)

typedef struct {
    jaero_pmsk_demod_t  *pmsk;
    jaero_msk_demod_t   *burstmsk;
    jaero_oqpsk_demod_t *oqpsk;
    int channel_id;
    int baud_rate;
    int channel_type;
    double sample_rate;
    double mixer_phase;
    double mixer_inc;

    float complex *ring;
    atomic_uint ring_head;
    atomic_uint ring_tail;
    atomic_ulong drops;

    pthread_t thread;
    atomic_int thread_run;
} jaero_chan_t;

static jaero_chan_t jaero_chans[MAX_JAERO_DEMODS];
static int num_jaero_chans = 0;

static void *chan_worker_fn(void *arg)
{
    jaero_chan_t *jc = (jaero_chan_t *)arg;
    const int BATCH = 4096;
    float complex batch[BATCH];
    double iq_dbl[BATCH * 2];

    while (atomic_load(&jc->thread_run)) {
        unsigned head = atomic_load(&jc->ring_head);
        unsigned tail = atomic_load(&jc->ring_tail);
        unsigned avail = head - tail;
        if (avail == 0) {
            struct timespec ts = {0, 500 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }
        unsigned take = avail;
        if (take > BATCH) take = BATCH;
        unsigned tail_mod = tail & (CHAN_RING_SIZE - 1);
        unsigned first = CHAN_RING_SIZE - tail_mod;
        if (first > take) first = take;
        memcpy(batch, &jc->ring[tail_mod], first * sizeof(float complex));
        if (take > first)
            memcpy(&batch[first], &jc->ring[0], (take - first) * sizeof(float complex));
        atomic_store(&jc->ring_tail, tail + take);

        if (jc->pmsk) {
            for (unsigned i = 0; i < take; i++) {
                iq_dbl[i*2]   = crealf(batch[i]);
                iq_dbl[i*2+1] = cimagf(batch[i]);
            }
            jaero_pmsk_feed_iq(jc->pmsk, iq_dbl, take);
        } else if (jc->oqpsk) {
            int16_t pcm[BATCH];
            for (unsigned i = 0; i < take; i++) {
                double ca = cos(jc->mixer_phase);
                double sa = sin(jc->mixer_phase);
                double audio = creal((double complex)batch[i] * (ca + sa * I));
                jc->mixer_phase += jc->mixer_inc;
                if (jc->mixer_phase > 2.0 * M_PI) jc->mixer_phase -= 2.0 * M_PI;
                double scaled = audio * AUDIO_GAIN * 32768.0;
                if (scaled > 32767.0) scaled = 32767.0;
                if (scaled < -32768.0) scaled = -32768.0;
                pcm[i] = (int16_t)scaled;
            }
            jaero_oqpsk_feed(jc->oqpsk, pcm, take);
        }
    }
    return NULL;
}

static void chan_push(jaero_chan_t *jc, const float complex *samples, int n)
{
    unsigned head = atomic_load(&jc->ring_head);
    unsigned tail = atomic_load(&jc->ring_tail);
    unsigned free_slots = CHAN_RING_SIZE - (head - tail);
    if ((unsigned)n > free_slots) {
        atomic_fetch_add(&jc->drops, 1);
        n = (int)free_slots;
        if (n <= 0) return;
    }
    unsigned head_mod = head & (CHAN_RING_SIZE - 1);
    unsigned first = CHAN_RING_SIZE - head_mod;
    if ((unsigned)first > (unsigned)n) first = (unsigned)n;
    memcpy(&jc->ring[head_mod], samples, first * sizeof(float complex));
    if ((unsigned)n > first)
        memcpy(&jc->ring[0], &samples[first], ((unsigned)n - first) * sizeof(float complex));
    atomic_store(&jc->ring_head, head + (unsigned)n);
}

static void chan_init_thread(jaero_chan_t *jc)
{
    jc->ring = (float complex *)malloc(CHAN_RING_SIZE * sizeof(float complex));
    atomic_init(&jc->ring_head, 0);
    atomic_init(&jc->ring_tail, 0);
    atomic_init(&jc->drops, 0);
    atomic_init(&jc->thread_run, 1);
    pthread_create(&jc->thread, NULL, chan_worker_fn, jc);
}

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
#include "aero_decode.h"
#include "vita49.h"
#include "feed.h"
#include "web.h"
#include "acars_position.h"
#include "waypoint_db.h"
#include "learned_waypoints.h"

#ifdef HAVE_LIBACARS
#include <libacars/libacars.h>
#include <libacars/acars.h>
#include <libacars/adsc.h>
#include <libacars/cpdlc.h>
#include <libacars/list.h>
#include <libacars/reassembly.h>
#include <libacars/vstring.h>
#include <libacars/version.h>
static la_reasm_ctx *acars_reasm_ctx = NULL;
#endif

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
int vita49_enabled = 0;
char *vita49_endpoint = NULL;
int web_enabled = 0;
int web_port = 8888;
int feed_enabled = 0;
#define UDP_MAX 4
char *udp_hosts[UDP_MAX];
int udp_ports[UDP_MAX];
int udp_count = 0;
int mqtt_enabled = 0;
char *mqtt_host = NULL;
int mqtt_port = 1883;
char *mqtt_user = NULL;
char *mqtt_pass = NULL;
char *mqtt_topic = NULL;
int basestation_enabled = 0;
char *basestation_endpoint = NULL;
char *aircraft_db_path = NULL;
int update_db_flag = 0;
char *station_id = NULL;

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
atomic_ulong stat_aero_crc_ok = 0;
atomic_ulong stat_aero_crc_fail = 0;
atomic_ulong stat_aero_bursts = 0;
atomic_ulong stat_aero_msgs = 0;
atomic_ulong stat_pos_adsc = 0;
atomic_ulong stat_pos_text = 0;
atomic_ulong stat_pos_waypoint = 0;
atomic_ulong stat_stdc_ber_sum = 0;
atomic_ulong stat_stdc_ber_count = 0;
atomic_ulong stat_aero_ber_sum = 0;
atomic_ulong stat_aero_ber_count = 0;
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
        case FMT_CI16: {
            s = malloc(sizeof(*s) + block * 2);
            s->format = SAMPLE_FMT_INT8;
            int16_t *tmp = malloc(block * 4);
            r = fread(tmp, 4, block, f);
            for (size_t i = 0; i < r * 2; i++)
                s->samples[i] = (int8_t)(tmp[i] >> 8);
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
    unsigned long bursts = atomic_load(&stat_aero_bursts);
    unsigned long msgs = atomic_load(&stat_aero_msgs);
    unsigned long ac_ok = atomic_load(&stat_aero_crc_ok);
    float stdc_ber = sb_cnt ? (float)sb_sum / (sb_cnt * 10000.0f) : 0;
    fprintf(stderr, "\r[STD-C: %lu %s BER:%.2f CRC:%lu/%lu | "
            "Aero: %lu bursts %lu msgs CRC:%lu | drop:%lu]   ",
            stdc, synced ? "SYNC" : "SRCH",
            stdc_ber, sc_ok, sc_ok + sc_fail,
            bursts, msgs, ac_ok, drops);
}

static dbpsk_demod_t *stdc_demod = NULL;
static stdc_decoder_t *stdc_decoder = NULL;
static channelizer_t *channelizer = NULL;
static int next_dynamic_channel_id = 100;

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
    feed_stdc_message(msg);
    if (web_enabled)
        web_add_stdc(msg);
}

static void stdc_bits_cb(const float *soft_bits, int num_bits, void *user) {
    (void)user;
    if (stdc_decoder)
        stdc_decoder_feed(stdc_decoder, soft_bits, num_bits);
}

/* JAERO aerol ACARS callback: receives decoded ISU userdata from AeroL's
 * full decode chain. acarsitem.valid checked inside jaero_demod.cpp, so
 * every call here is a CRC-verified ACARS frame. */
static void jaero_acars_data_cb(const uint8_t *data, int len,
                                  int channel_id, void *user) {
    (void)user;
    atomic_fetch_add(&stat_aero_msgs, 1);
    atomic_fetch_add(&stat_aero_crc_ok, 1);

    if (verbose) {
        fprintf(stderr, "\n[JAERO-DECODED ch%d] %d bytes\n  hex: ", channel_id, len);
        for (int i = 0; i < len && i < 120; i++)
            fprintf(stderr, "%02X ", data[i]);
        if (len > 120) fprintf(stderr, "...");
        fprintf(stderr, "\n  txt: ");
        for (int i = 0; i < len && i < 120; i++) {
            uint8_t c = data[i] & 0x7F;
            fputc((c >= 0x20 && c < 0x7F) ? c : '.', stderr);
        }
        fprintf(stderr, "\n");
    }

#ifdef HAVE_LIBACARS
    /* Scan for SOH byte to find ACARS start (AeroL prepends FF FF preamble) */
    int soh_idx = -1;
    for (int i = 0; i < len - 12 && i < 6; i++) {
        if ((data[i] & 0x7F) == 0x01) { soh_idx = i; break; }
    }
    if (soh_idx >= 0 && len - soh_idx > 13) {
        const uint8_t *acars_start = data + soh_idx + 1;
        int acars_len = len - soh_idx - 1;
        struct timeval tv;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        tv.tv_sec = ts.tv_sec;
        tv.tv_usec = ts.tv_nsec / 1000;

        la_proto_node *tree = la_acars_parse_and_reassemble(
            acars_start, acars_len, LA_MSG_DIR_AIR2GND, acars_reasm_ctx, tv);
        if (tree) {
            la_proto_node *acars_node = la_proto_tree_find_acars(tree);
            if (acars_node && acars_node->data) {
                la_acars_msg *amsg = (la_acars_msg *)acars_node->data;
                if (amsg->reasm_status != LA_REASM_IN_PROGRESS && !amsg->err) {
                    fprintf(stderr, "\n[ACARS ch%d] reg=%s label=%.2s",
                            channel_id,
                            amsg->reg[0] ? amsg->reg : "?",
                            amsg->label);
                    if (amsg->txt && amsg->txt[0])
                        fprintf(stderr, "\n  %s", amsg->txt);
                    fprintf(stderr, "\n");

                    if (verbose) {
                        la_vstring *vstr = la_proto_tree_format_text(NULL, tree);
                        if (vstr && vstr->str)
                            fprintf(stderr, "%s", vstr->str);
                        if (vstr) la_vstring_destroy(vstr, true);
                    }

                    aero_message_t outmsg;
                    memset(&outmsg, 0, sizeof(outmsg));
                    outmsg.channel_id = channel_id;
                    outmsg.lat = NAN;
                    outmsg.lon = NAN;
                    outmsg.alt_ft = -1;
                    outmsg.has_position = 0;
                    outmsg.mode = amsg->mode;
                    outmsg.block_id = amsg->block_id;
                    outmsg.ack = amsg->ack;
                    strncpy(outmsg.reg, amsg->reg, sizeof(outmsg.reg) - 1);
                    strncpy(outmsg.flight, amsg->flight_id, sizeof(outmsg.flight) - 1);
                    strncpy(outmsg.label, amsg->label, sizeof(outmsg.label) - 1);
                    if (amsg->txt) {
                        int tl = (int)strlen(amsg->txt);
                        if (tl > (int)sizeof(outmsg.text) - 1)
                            tl = sizeof(outmsg.text) - 1;
                        memcpy(outmsg.text, amsg->txt, tl);
                        outmsg.text_len = tl;
                    }

                    /* Position extraction from ACARS text */
                    double lat = 0, lon = 0;
                    int alt_ft = -99999;
                    int have_pos = 0;
                    if (!have_pos) {
                        have_pos = acars_extract_text_position(amsg->label,
                                                                amsg->txt,
                                                                &lat, &lon);
                        if (have_pos) atomic_fetch_add(&stat_pos_text, 1);
                    }
                    if (!have_pos) {
                        have_pos = acars_extract_waypoint_position(amsg->label,
                                                                    amsg->txt,
                                                                    &lat, &lon);
                        if (have_pos) atomic_fetch_add(&stat_pos_waypoint, 1);
                    }
                    if (have_pos) {
                        outmsg.lat = lat;
                        outmsg.lon = lon;
                        outmsg.alt_ft = alt_ft == -99999 ? -1 : alt_ft;
                        outmsg.has_position = 1;
                        fprintf(stderr, "  pos=%.4f,%.4f\n", lat, lon);
                    }

                    /* Harvest waypoints from FPN messages */
                    if (amsg->txt && amsg->label[0] == 'H' &&
                        amsg->label[1] == '1' &&
                        strncmp(amsg->txt, "FPN", 3) == 0) {
                        learned_wp_parse_fpn(amsg->txt);
                    }

                    feed_aero_message(&outmsg);
                    if (web_enabled)
                        web_add_aero(&outmsg);
                }
            }
            la_proto_tree_destroy(tree);
        }
    }
#else
    aero_message_t outmsg;
    memset(&outmsg, 0, sizeof(outmsg));
    outmsg.channel_id = channel_id;
    outmsg.lat = NAN;
    outmsg.lon = NAN;
    outmsg.alt_ft = -1;
    outmsg.has_position = 0;
    feed_aero_message(&outmsg);
    if (web_enabled)
        web_add_aero(&outmsg);
#endif
}

/* JAERO demod callback — counts bursts for status line. AeroL handles decode. */
static void jaero_bits_cb(const unsigned char *bits, int num_bits,
                            int channel_id, void *user) {
    (void)user; (void)bits; (void)num_bits; (void)channel_id;
    atomic_fetch_add(&stat_aero_bursts, 1);
}

static void channel_output_cb(int channel_id, channel_type_t type,
                                float complex *samples, int num_samples,
                                void *user) {
    (void)user;

#ifdef HAVE_ZMQ
    if (zmq_enabled) {
        double output_rate = channelizer_output_rate(channelizer, channel_id);
        if (output_rate > 0)
            zmq_audio_send(channel_id, samples, num_samples, output_rate);
    }
#endif

    if (type == CHAN_STDC_EGC && stdc_demod) {
        dbpsk_demod_process(stdc_demod, samples, num_samples);
        return;
    }

    if (type == CHAN_AERO_600 || type == CHAN_AERO_1200) {
        int baud = (type == CHAN_AERO_1200) ? 1200 : 600;
        double output_rate = channelizer_output_rate(channelizer, channel_id);
        if (output_rate <= 0) return;

        jaero_chan_t *jc = NULL;
        for (int i = 0; i < num_jaero_chans; i++) {
            if (jaero_chans[i].channel_id == channel_id) { jc = &jaero_chans[i]; break; }
        }
        if (!jc && num_jaero_chans < MAX_JAERO_DEMODS) {
            jc = &jaero_chans[num_jaero_chans++];
            jc->channel_id = channel_id;
            jc->baud_rate = baud;
            jc->pmsk = NULL;
            jc->burstmsk = NULL;
            jc->oqpsk = NULL;
            jc->mixer_phase = 0;
            jc->mixer_inc = 2.0 * M_PI * PMSK_AUDIO_HZ / output_rate;
            jc->pmsk = jaero_pmsk_create(output_rate, (double)baud,
                                          channel_id, jaero_bits_cb, NULL);
            if (jc->pmsk)
                jaero_pmsk_set_acars_callback(jc->pmsk,
                                               jaero_acars_data_cb, NULL);
            fprintf(stderr, "[PMSK ch%d] baud=%d rate=%.0f (continuous P-channel)\n",
                    channel_id, baud, output_rate);
            if (jc->pmsk)
                chan_init_thread(jc);
        }
        if (!jc || !jc->pmsk) return;
        chan_push(jc, samples, num_samples);
        return;
    }

    if (type == CHAN_AERO_8400 || type == CHAN_AERO_10500) {
        int baud = (type == CHAN_AERO_10500) ? 10500 : 8400;
        double output_rate = channelizer_output_rate(channelizer, channel_id);
        if (output_rate <= 0) return;

        jaero_chan_t *jc = NULL;
        for (int i = 0; i < num_jaero_chans; i++) {
            if (jaero_chans[i].channel_id == channel_id) { jc = &jaero_chans[i]; break; }
        }
        if (!jc && num_jaero_chans < MAX_JAERO_DEMODS) {
            jc = &jaero_chans[num_jaero_chans++];
            jc->channel_id = channel_id;
            jc->baud_rate = baud;
            jc->pmsk = NULL;
            jc->burstmsk = NULL;
            jc->mixer_phase = 0;
            jc->mixer_inc = 2.0 * M_PI * AUDIO_CENTER_HZ / output_rate;
            jc->oqpsk = jaero_oqpsk_create(output_rate, (double)baud,
                                             channel_id, jaero_bits_cb, NULL);
            fprintf(stderr, "[OQPSK-INIT] ch%d baud=%d rate=%.0f\n",
                    channel_id, baud, output_rate);
            if (jc->oqpsk)
                chan_init_thread(jc);
        }
        if (!jc || !jc->oqpsk) return;
        chan_push(jc, samples, num_samples);
        return;
    }
}

int main(int argc, char **argv) {
    self_pid = getpid();
    parse_options(argc, argv);

    simd_init(0);
    feed_init();

    if (web_enabled) {
        if (web_init(web_port) != 0)
            errx(1, "Failed to start web dashboard");
    }

    const satellite_t *sat = NULL;
    if (satellite_name) {
        sat = satellite_lookup(satellite_name);
        if (!sat)
            errx(1, "Unknown satellite: %s", satellite_name);
        fprintf(stderr, "Satellite: %s (%s)\n", sat->name, sat->region);

        double lo = 1e12, hi = 0;
        for (int i = 0; i < sat->num_channels; i++) {
            if (op_mode == MODE_AERO && sat->channels[i].type == CHAN_STDC_EGC) continue;
            if (op_mode == MODE_STDC && sat->channels[i].type != CHAN_STDC_EGC) continue;
            if (sat->channels[i].frequency < lo) lo = sat->channels[i].frequency;
            if (sat->channels[i].frequency > hi) hi = sat->channels[i].frequency;
        }
        if (lo > hi) { lo = sat->freq_min; hi = sat->freq_max; }
        if (center_freq == 0) center_freq = (lo + hi) / 2.0;
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

    /* Load waypoint DB for text-based position extraction */
    {
        char wp_path[512];
        ssize_t exe_len = readlink("/proc/self/exe", wp_path, sizeof(wp_path) - 1);
        if (exe_len > 0) {
            wp_path[exe_len] = '\0';
            char *slash = strrchr(wp_path, '/');
            if (slash) {
                snprintf(slash + 1, sizeof(wp_path) - (slash + 1 - wp_path),
                         "../data/waypoints.csv");
                if (waypoint_db_load(wp_path) < 0) {
                    snprintf(slash + 1, sizeof(wp_path) - (slash + 1 - wp_path),
                             "data/waypoints.csv");
                    waypoint_db_load(wp_path);
                }
            }
        }
    }

#ifdef HAVE_ZMQ
    if (zmq_enabled) {
        if (zmq_audio_init(zmq_base_port) != 0)
            errx(1, "Failed to initialize ZMQ audio");
    }
#endif

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

        int added = 0;
        for (int i = 0; i < sat->num_channels; i++) {
            const channel_def_t *cd = &sat->channels[i];
            double offset = fabs(cd->frequency - center_freq);
            if (offset > samp_rate / 2.0) continue;
            if (op_mode == MODE_AERO && cd->type == CHAN_STDC_EGC) continue;
            if (op_mode == MODE_STDC && cd->type != CHAN_STDC_EGC) continue;
            if (channelizer_add_channel(channelizer, cd->frequency,
                                         cd->type, cd->channel_id) == 0)
                added++;
        }
        fprintf(stderr, "Channelizer: %d channels active\n", added);

        for (int i = 0; i < sat->num_channels; i++) {
            if (sat->channels[i].type == CHAN_STDC_EGC && op_mode != MODE_AERO) {
                double output_rate = samp_rate / (int)(samp_rate / (1200.0 * 4.0));
                stdc_decoder = stdc_decoder_create(stdc_message_cb, NULL);
                stdc_demod = dbpsk_demod_create(output_rate, 1200.0,
                                                  stdc_bits_cb, NULL);
                if (stdc_demod && stdc_decoder)
                    fprintf(stderr, "STD-C EGC decoder: active\n");
                break;
            }
        }

        if (op_mode != MODE_STDC) {
            int have_aero = 0;
            for (int i = 0; i < sat->num_channels; i++) {
                channel_type_t ct = sat->channels[i].type;
                if (ct == CHAN_AERO_600 || ct == CHAN_AERO_1200 ||
                    ct == CHAN_AERO_8400 || ct == CHAN_AERO_10500) {
                    have_aero = 1; break;
                }
            }
            if (have_aero) {
                fprintf(stderr, "Aero decoder: JAERO/AeroL embedded\n");
#ifdef HAVE_LIBACARS
                acars_reasm_ctx = la_reasm_ctx_new();
                if (acars_reasm_ctx)
                    fprintf(stderr, "libacars %s: ACARS reassembly active\n",
                            LA_VERSION);
#endif
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
    for (int i = 0; i < num_jaero_chans; i++) {
        if (jaero_chans[i].ring) {
            atomic_store(&jaero_chans[i].thread_run, 0);
            pthread_join(jaero_chans[i].thread, NULL);
        }
    }
    for (int i = 0; i < num_jaero_chans; i++) {
        if (jaero_chans[i].pmsk)  jaero_pmsk_destroy(jaero_chans[i].pmsk);
        if (jaero_chans[i].oqpsk) jaero_oqpsk_destroy(jaero_chans[i].oqpsk);
        free(jaero_chans[i].ring);
        jaero_chans[i].ring = NULL;
    }
    stdc_decoder_destroy(stdc_decoder);
    channelizer_destroy(channelizer);

#ifdef HAVE_ZMQ
    if (zmq_enabled) {
        extern void zmq_audio_destroy(void);
        zmq_audio_destroy();
    }
#endif

    return 0;
}
