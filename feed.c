/*
 * JSON feed output and UDP forwarding
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "blocking_queue.h"
#include "feed.h"

extern int feed_enabled;
extern int jaero_format_enabled;
extern char *jaero_format_host;
extern int jaero_format_port;
extern char *satellite_name;

static int jaero_udp_sock = -1;
static struct sockaddr_in jaero_udp_addr;
#define UDP_MAX 4
extern char *udp_hosts[UDP_MAX];
extern int udp_ports[UDP_MAX];
extern int udp_count;

static int udp_sockets[UDP_MAX];
static struct sockaddr_in udp_addrs[UDP_MAX];
static int initialized = 0;

/* ---- Stdout writer thread ----
 * fwrite(stdout) blocks when a downstream pipe consumer stalls and the
 * 64KB kernel pipe buffer fills. Historically this stalled whichever
 * thread produced the message — for STD-C, that's the main channelizer
 * consumer, so samples_queue would fill and stat_drops would climb to
 * tens of thousands while decoding froze. Decouple the decode pipeline
 * from stdout by handing JSON strings to a dedicated writer thread. */
#define FEED_QUEUE_CAPACITY 256
typedef struct {
    char *data;
    int   len;
} feed_json_msg_t;

static Blocking_Queue feed_queue;
static pthread_t feed_writer_tid;
static int       feed_writer_running = 0;
static atomic_ulong stat_feed_drops = 0;

unsigned long feed_get_json_drops(void) {
    return atomic_load(&stat_feed_drops);
}

static void *feed_writer_fn(void *arg) {
    (void)arg;
    for (;;) {
        feed_json_msg_t *m = NULL;
        int rc = blocking_queue_take(&feed_queue, &m);
        if (rc == BQ_CLOSED) break;
        if (rc != 0 || !m) continue;
        if (m->data && m->len > 0) {
            fwrite(m->data, 1, m->len, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
        free(m->data);
        free(m);
    }
    return NULL;
}

void feed_init(void) {
    for (int i = 0; i < udp_count; i++) {
        udp_sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sockets[i] < 0) {
            perror("feed: socket");
            continue;
        }

        memset(&udp_addrs[i], 0, sizeof(udp_addrs[i]));
        udp_addrs[i].sin_family = AF_INET;
        udp_addrs[i].sin_port = htons(udp_ports[i]);
        if (inet_aton(udp_hosts[i], &udp_addrs[i].sin_addr) == 0) {
            fprintf(stderr, "feed: invalid address %s\n", udp_hosts[i]);
            close(udp_sockets[i]);
            udp_sockets[i] = -1;
        }
    }

    /* JAERO format 3 UDP socket */
    if (jaero_format_enabled && jaero_format_host) {
        jaero_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (jaero_udp_sock >= 0) {
            memset(&jaero_udp_addr, 0, sizeof(jaero_udp_addr));
            jaero_udp_addr.sin_family = AF_INET;
            jaero_udp_addr.sin_port = htons(jaero_format_port);
            if (inet_aton(jaero_format_host, &jaero_udp_addr.sin_addr) == 0) {
                fprintf(stderr, "feed: invalid jaero-format address %s\n", jaero_format_host);
                close(jaero_udp_sock);
                jaero_udp_sock = -1;
            } else {
                fprintf(stderr, "JAERO format 3 UDP: %s:%d\n",
                        jaero_format_host, jaero_format_port);
            }
        }
    }

    /* Spawn stdout writer thread if --feed enabled. Keeps decode pipeline
     * immune to downstream pipe stalls. */
    if (feed_enabled) {
        if (blocking_queue_init(&feed_queue, FEED_QUEUE_CAPACITY) == 0) {
            if (pthread_create(&feed_writer_tid, NULL,
                               feed_writer_fn, NULL) == 0) {
                feed_writer_running = 1;
            } else {
                perror("feed: pthread_create");
                blocking_queue_destroy(&feed_queue);
            }
        } else {
            fprintf(stderr, "feed: failed to init writer queue\n");
        }
    }

    initialized = 1;
}

static void send_json(const char *json, int len) {
    /* stdout feed — hand off to writer thread, drop on full */
    if (feed_enabled && feed_writer_running) {
        feed_json_msg_t *m = (feed_json_msg_t *)malloc(sizeof(*m));
        char *copy = (char *)malloc(len);
        if (!m || !copy) {
            free(m);
            free(copy);
            atomic_fetch_add(&stat_feed_drops, 1);
        } else {
            memcpy(copy, json, len);
            m->data = copy;
            m->len  = len;
            if (blocking_queue_add(&feed_queue, m) != 0) {
                free(copy);
                free(m);
                atomic_fetch_add(&stat_feed_drops, 1);
            }
        }
    }

    /* UDP endpoints */
    for (int i = 0; i < udp_count; i++) {
        if (udp_sockets[i] >= 0) {
            sendto(udp_sockets[i], json, len, 0,
                   (struct sockaddr *)&udp_addrs[i],
                   sizeof(udp_addrs[i]));
        }
    }

    /* MQTT publish */
#ifdef HAVE_MQTT
    {
        extern int mqtt_enabled;
        extern void mqtt_publish_json(const char *, int);
        if (mqtt_enabled)
            mqtt_publish_json(json, len);
    }
#endif
}

/* Escape a string for JSON output */
static int json_escape(char *out, int maxlen, const char *in, int inlen) {
    int pos = 0;
    for (int i = 0; i < inlen && pos < maxlen - 6; i++) {
        char c = in[i];
        switch (c) {
        case '"':  out[pos++] = '\\'; out[pos++] = '"'; break;
        case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
        case '\n': out[pos++] = '\\'; out[pos++] = 'n'; break;
        case '\r': out[pos++] = '\\'; out[pos++] = 'r'; break;
        case '\t': out[pos++] = '\\'; out[pos++] = 't'; break;
        default:
            if (c >= 0x20)
                out[pos++] = c;
            break;
        }
    }
    out[pos] = '\0';
    return pos;
}

static double now_unix(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void feed_stdc_message(const stdc_message_t *msg) {
    if (!initialized) return;
    if (!feed_enabled && udp_count == 0) return;

    char buf[8192];
    char escaped[4096];
    json_escape(escaped, sizeof(escaped), msg->text, msg->text_len);

    const char *type_str;
    switch (msg->type) {
    case STDC_MSG_EGC_SINGLE:
    case STDC_MSG_EGC_DOUBLE_1:
    case STDC_MSG_EGC_DOUBLE_2:
        type_str = "egc";
        break;
    case STDC_MSG_BULLETIN:
        type_str = "bulletin";
        break;
    case STDC_MSG_ANNOUNCEMENT:
        type_str = "announcement";
        break;
    case STDC_MSG_CHANNEL_CLEAR:
    case STDC_MSG_ACK_REQUEST:
    case STDC_MSG_MSG_ACK:
    case STDC_MSG_CHAN_ASSIGNMENT:
        type_str = "signalling";
        break;
    case STDC_MSG_LOGIN_ACK:
        type_str = "login_ack";
        break;
    case STDC_MSG_MESSAGE_DATA:
        type_str = "message";
        break;
    case STDC_MSG_NET_UPDATE:
        type_str = "net_update";
        break;
    case STDC_MSG_LES_LIST:
        type_str = "les_list";
        break;
    case STDC_MSG_INDIVIDUAL_POLL:
        type_str = "poll";
        break;
    case STDC_MSG_CONFIRMATION:
        type_str = "confirmation";
        break;
    default:
        type_str = "stdc";
        break;
    }

    const char *sat_str = satellite_name ? satellite_name : "";
    const char *sat_name = msg->sat_name ? msg->sat_name : "";
    const char *les_name = msg->les_name ? msg->les_name : "";

    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"source\":\"inmarsat-sniffer\","
        "\"type\":\"%s\",\"satellite\":\"%s\","
        "\"descriptor\":\"0x%02X\"",
        type_str, sat_str, msg->descriptor);

    /* Include sat/LES info when available */
    if (msg->sat_id >= 0 && msg->les_id > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"sat_id\":%d,\"sat_name\":\"%s\""
            ",\"les_id\":%d,\"les_name\":\"%s\"",
            msg->sat_id, sat_name, msg->les_id, les_name);
    }

    /* Service/priority for EGC */
    if (msg->service_code > 0 || msg->priority > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"service\":%d,\"priority\":%d",
            msg->service_code, msg->priority);
    }

    /* MES ID if present */
    if (msg->mes_id > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"mes_id\":%d", msg->mes_id);
    }

    /* Channel info */
    if (msg->logical_channel > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"channel\":%d", msg->logical_channel);
    }

    /* Frequency info */
    if (msg->uplink_mhz > 0.0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"uplink_mhz\":%.4f", msg->uplink_mhz);
    }
    if (msg->downlink_mhz > 0.0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"downlink_mhz\":%.4f", msg->downlink_mhz);
    }

    /* Text */
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        ",\"text\":\"%s\"", escaped);

    /* Position */
    if (msg->has_position && !isnan(msg->lat) && !isnan(msg->lon)) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"lat\":%.6f,\"lon\":%.6f", msg->lat, msg->lon);
    }

    /* Timestamp */
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        ",\"timestamp\":%.3f}", now_unix());

    if (pos > 0 && pos < (int)sizeof(buf))
        send_json(buf, pos);
}

void feed_aero_message(const aero_message_t *msg) {
    if (!initialized) return;
    if (!feed_enabled && udp_count == 0) return;

    char buf[8192];
    char escaped[4096];
    json_escape(escaped, sizeof(escaped), msg->text, msg->text_len);

    /* Split timestamp into sec / usec for JAERO JSONdump schema */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long sec  = (long)ts.tv_sec;
    long usec = ts.tv_nsec / 1000;

    extern char *station_id;
    const char *sid = station_id ? station_id : "";

    /* Reg field — strip our leading dot to match JAERO JSONdump convention
     * ("reg": "OO-SFC", not ".OO-SFC"). */
    const char *reg = msg->reg;
    if (reg && reg[0] == '.') reg++;

    /* AES/GES hex, uppercase, zero-padded */
    char aes_hex[8], ges_hex[4];
    snprintf(aes_hex, sizeof(aes_hex), "%06X", msg->aes_id & 0xFFFFFFu);
    snprintf(ges_hex, sizeof(ges_hex), "%02X", msg->ges_id & 0xFFu);

    /* src / dst swap on direction: downlink=aircraft->ground means src=aes,
     * dst=ges. uplink (our typical P-channel decode) means src=ges, dst=aes.
     * Matches JAERO mainwindow.cpp JSONdump src/dst assignment. */
    const char *src_addr = msg->downlink ? aes_hex : ges_hex;
    const char *src_type = msg->downlink ? "Aircraft Earth Station" : "Ground Earth Station";
    const char *dst_addr = msg->downlink ? ges_hex : aes_hex;
    const char *dst_type = msg->downlink ? "Ground Earth Station" : "Aircraft Earth Station";

    /* Build nested JSONdump-compliant object. Fields match JAERO 1.0.4.11+
     * mainwindow.cpp ACARSitem_to_HumanText() "JSONdump" branch. The
     * arinc622 sub-object, when present, is the libacars proto-tree JSON
     * serialisation — the author-provided schema in issue #10 shows an
     * arinc622 object; we pass through whatever libacars produced rather
     * than reshaping, since Airframes/Acarshub already consume libacars
     * output via dumpvdl2. */
    const char *arinc = msg->arinc622_json;
    int len = snprintf(buf, sizeof(buf),
        "{"
            /* app.name must literally be "JAERO" for Acarshub's dumpJSON
             * parser to extract fields — anything else and Acarshub stores
             * empty strings (per issue #10 testing). Same quirk iridium-sniffer
             * had to work around. */
            "\"app\":{\"name\":\"JAERO\",\"ver\":\"inmarsat-sniffer VFO%02d\"},"
            "\"isu\":{"
                "\"acars\":{"
                    "\"mode\":\"%c\","
                    "\"ack\":\"%c\","
                    "\"blk_id\":\"%c\","
                    "\"label\":\"%.2s\","
                    "\"reg\":\"%s\""
                    "%s%s%s"       /* optional flight */
                    ",\"msg_text\":\"%s\""
                    "%s%s"         /* optional ",\"arinc622\":{...}" */
                "},"
                "\"refno\":\"%02X\","
                "\"qno\":\"%02X\","
                "\"src\":{\"addr\":\"%s\",\"type\":\"%s\"},"
                "\"dst\":{\"addr\":\"%s\",\"type\":\"%s\"}"
            "},"
            "\"t\":{\"sec\":%ld,\"usec\":%ld}"
            "%s%s%s"               /* optional station */
        "}",
        msg->channel_id,
        msg->mode ? msg->mode : ' ',
        msg->ack  ? msg->ack  : ' ',
        msg->block_id ? msg->block_id : ' ',
        msg->label,
        reg ? reg : "",
        (msg->flight[0] ? ",\"flight\":\"" : ""),
        (msg->flight[0] ? msg->flight      : ""),
        (msg->flight[0] ? "\""             : ""),
        escaped,
        (arinc ? ",\"arinc622\":" : ""),
        (arinc ? arinc            : ""),
        msg->refno, msg->qno,
        src_addr, src_type,
        dst_addr, dst_type,
        sec, usec,
        (sid[0] ? ",\"station\":\""     : ""),
        (sid[0] ? sid                    : ""),
        (sid[0] ? "\""                   : ""));

    if (len > 0 && len < (int)sizeof(buf))
        send_json(buf, len);

    /* JAERO text format 3 output */
    if (jaero_format_enabled) {
        char jbuf[4096];
        int jpos = 0;
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);

        /* Right-pad registration to 7 chars with leading dots (JAERO convention) */
        char reg_padded[8];
        int rlen = (int)strlen(msg->reg);
        int pad = 7 - rlen;
        if (pad < 0) pad = 0;
        memset(reg_padded, '.', pad);
        strncpy(reg_padded + pad, msg->reg, 7 - pad);
        reg_padded[7] = '\0';

        char label1 = msg->label[1];
        if ((unsigned char)label1 == 127) label1 = 'd';

        jpos += snprintf(jbuf + jpos, sizeof(jbuf) - jpos,
                "%02d:%02d:%02d %02d-%02d-%02d UTC "
                "AES:%06X GES:%02X %c %s %c %c%c %c",
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100,
                msg->aes_id, msg->ges_id,
                msg->mode ? msg->mode : '2',
                reg_padded,
                msg->ack ? msg->ack : ' ',
                msg->label[0] ? msg->label[0] : '_',
                label1 ? label1 : '_',
                msg->block_id ? msg->block_id : ' ');

        if (msg->text_len > 0) {
            jpos += snprintf(jbuf + jpos, sizeof(jbuf) - jpos, "\n\t");
            for (int i = 0; i < msg->text_len && jpos < (int)sizeof(jbuf) - 4; i++) {
                char c = msg->text[i];
                if (c == '\r') continue;
                if (c == '\n') { jbuf[jpos++] = '\n'; jbuf[jpos++] = '\t'; }
                else jbuf[jpos++] = c;
            }
        }
        jbuf[jpos++] = '\n';
        jbuf[jpos] = '\0';

        /* Send over UDP if configured */
        if (jaero_udp_sock >= 0) {
            sendto(jaero_udp_sock, jbuf, jpos, 0,
                   (struct sockaddr *)&jaero_udp_addr, sizeof(jaero_udp_addr));
        } else {
            /* Fall back to stderr if no UDP endpoint */
            fprintf(stderr, "\n%s", jbuf);
        }
    }
}

void feed_shutdown(void) {
    if (feed_writer_running) {
        blocking_queue_close(&feed_queue);
        pthread_join(feed_writer_tid, NULL);
        /* Drain any leftover entries */
        for (;;) {
            feed_json_msg_t *m = NULL;
            int rc = blocking_queue_poll(&feed_queue, &m);
            if (rc != 0 || !m) break;
            free(m->data);
            free(m);
        }
        blocking_queue_destroy(&feed_queue);
        feed_writer_running = 0;
    }
    for (int i = 0; i < udp_count; i++) {
        if (udp_sockets[i] >= 0) {
            close(udp_sockets[i]);
            udp_sockets[i] = -1;
        }
    }
    if (jaero_udp_sock >= 0) {
        close(jaero_udp_sock);
        jaero_udp_sock = -1;
    }
    initialized = 0;
}
