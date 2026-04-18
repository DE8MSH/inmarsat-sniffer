/*
 * JSON feed output and UDP forwarding
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "feed.h"

extern int feed_enabled;
extern int jaero_format_enabled;
extern char *satellite_name;
#define UDP_MAX 4
extern char *udp_hosts[UDP_MAX];
extern int udp_ports[UDP_MAX];
extern int udp_count;

static int udp_sockets[UDP_MAX];
static struct sockaddr_in udp_addrs[UDP_MAX];
static int initialized = 0;

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
    initialized = 1;
}

static void send_json(const char *json, int len) {
    /* stdout feed */
    if (feed_enabled) {
        fwrite(json, 1, len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
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

    double ts = now_unix();
    char time_utc[32];
    time_t t = (time_t)ts;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(time_utc, sizeof(time_utc), "%Y-%m-%d %H:%M:%S", &tm);

    const char *sat = satellite_name ? satellite_name : "";

    extern char *station_id;
    const char *sid = station_id ? station_id : "";

    /* JAERO/dumpvdl2-compatible JSON field names */
    int len;
    if (msg->has_position && !isnan(msg->lat) && !isnan(msg->lon)) {
        len = snprintf(buf, sizeof(buf),
            "{\"source\":\"inmarsat-sniffer\","
            "\"station_id\":\"%s\","
            "\"satellite\":\"%s\","
            "\"TIME\":%.0f,\"TIME_UTC\":\"%s\","
            "\"NONACARS\":false,"
            "\"REG\":\"%s\",\"FLIGHT\":\"%s\","
            "\"MODE\":\"%c\",\"LABEL\":\"%s\","
            "\"BI\":\"%c\",\"TAK\":\"%c\","
            "\"MESSAGE\":\"%s\","
            "\"lat\":%.6f,\"lon\":%.6f,\"alt\":%d,"
            "\"channel\":%d}",
            sid, sat, ts, time_utc,
            msg->reg, msg->flight,
            msg->mode ? msg->mode : ' ', msg->label,
            msg->block_id ? msg->block_id : ' ',
            msg->ack ? msg->ack : ' ',
            escaped,
            msg->lat, msg->lon, msg->alt_ft,
            msg->channel_id);
    } else {
        len = snprintf(buf, sizeof(buf),
            "{\"source\":\"inmarsat-sniffer\","
            "\"station_id\":\"%s\","
            "\"satellite\":\"%s\","
            "\"TIME\":%.0f,\"TIME_UTC\":\"%s\","
            "\"NONACARS\":false,"
            "\"REG\":\"%s\",\"FLIGHT\":\"%s\","
            "\"MODE\":\"%c\",\"LABEL\":\"%s\","
            "\"BI\":\"%c\",\"TAK\":\"%c\","
            "\"MESSAGE\":\"%s\","
            "\"channel\":%d}",
            sid, sat, ts, time_utc,
            msg->reg, msg->flight,
            msg->mode ? msg->mode : ' ', msg->label,
            msg->block_id ? msg->block_id : ' ',
            msg->ack ? msg->ack : ' ',
            escaped,
            msg->channel_id);
    }

    if (len > 0 && len < (int)sizeof(buf))
        send_json(buf, len);

    /* JAERO text format 3 output to stderr */
    if (jaero_format_enabled) {
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

        fprintf(stderr, "\n%02d:%02d:%02d %02d-%02d-%02d UTC "
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
            fprintf(stderr, "\n\t");
            for (int i = 0; i < msg->text_len; i++) {
                char c = msg->text[i];
                if (c == '\r') continue;
                if (c == '\n') fprintf(stderr, "\n\t");
                else fputc(c, stderr);
            }
        }
        fprintf(stderr, "\n");
    }
}

void feed_shutdown(void) {
    for (int i = 0; i < udp_count; i++) {
        if (udp_sockets[i] >= 0) {
            close(udp_sockets[i]);
            udp_sockets[i] = -1;
        }
    }
    initialized = 0;
}
