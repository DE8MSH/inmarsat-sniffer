/*
 * MQTT output for decoded ACARS messages
 *
 * Publishes each decoded ACARS message as a JSON string to a
 * configurable MQTT topic. Uses libmosquitto with auto-reconnect.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mosquitto.h>

#include "blocking_queue.h"
#include "mqtt.h"

static struct mosquitto *mosq = NULL;
static char topic[256] = "inmarsat-sniffer/acars";
static int connected = 0;

/* Writer thread + bounded queue isolates mqtt_publish_json() from any
 * brief contention on libmosquitto's internal mutex when the uplink
 * is saturated. Drop-on-full, same pattern as feed.c stdout writer. */
#define MQTT_QUEUE_CAPACITY 256
typedef struct {
    char *data;
    int   len;
} mqtt_msg_t;
static Blocking_Queue mqtt_queue;
static pthread_t      mqtt_writer_tid;
static int            mqtt_writer_running = 0;
static atomic_ulong   stat_mqtt_drops = 0;

unsigned long mqtt_get_drops(void) {
    return atomic_load(&stat_mqtt_drops);
}

static void *mqtt_writer_fn(void *arg) {
    (void)arg;
    for (;;) {
        mqtt_msg_t *m = NULL;
        int rc = blocking_queue_take(&mqtt_queue, &m);
        if (rc == BQ_CLOSED) break;
        if (rc != 0 || !m) continue;
        if (mosq && connected && m->data && m->len > 0)
            mosquitto_publish(mosq, NULL, topic, m->len, m->data, 0, false);
        free(m->data);
        free(m);
    }
    return NULL;
}

static void on_connect(struct mosquitto *m, void *user, int rc)
{
    (void)m; (void)user;
    if (rc == 0) {
        connected = 1;
        fprintf(stderr, "MQTT: connected\n");
    } else {
        connected = 0;
        fprintf(stderr, "MQTT: connect failed (rc=%d)\n", rc);
    }
}

static void on_disconnect(struct mosquitto *m, void *user, int rc)
{
    (void)m; (void)user;
    connected = 0;
    if (rc != 0)
        fprintf(stderr, "MQTT: disconnected (rc=%d), will reconnect\n", rc);
}

int mqtt_init(const char *host, int port,
              const char *user, const char *pass,
              const char *topic_prefix)
{
    mosquitto_lib_init();

    mosq = mosquitto_new("inmarsat-sniffer", true, NULL);
    if (!mosq) {
        fprintf(stderr, "MQTT: mosquitto_new failed\n");
        return -1;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_reconnect_delay_set(mosq, 1, 30, true);

    if (user && user[0]) {
        if (mosquitto_username_pw_set(mosq, user, pass) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "MQTT: auth setup failed\n");
            return -1;
        }
    }

    if (topic_prefix && topic_prefix[0])
        snprintf(topic, sizeof(topic), "%s", topic_prefix);

    int rc = mosquitto_connect_async(mosq, host, port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT: connect to %s:%d failed: %s\n",
                host, port, mosquitto_strerror(rc));
        return -1;
    }

    mosquitto_loop_start(mosq);

    if (blocking_queue_init(&mqtt_queue, MQTT_QUEUE_CAPACITY) == 0) {
        if (pthread_create(&mqtt_writer_tid, NULL, mqtt_writer_fn, NULL) == 0)
            mqtt_writer_running = 1;
        else
            blocking_queue_destroy(&mqtt_queue);
    }

    fprintf(stderr, "MQTT: publishing to %s:%d topic=%s\n", host, port, topic);
    return 0;
}

void mqtt_publish_json(const char *json, int len)
{
    if (!mqtt_writer_running || len <= 0) return;
    mqtt_msg_t *m = (mqtt_msg_t *)malloc(sizeof(*m));
    char *copy = (char *)malloc(len);
    if (!m || !copy) { free(m); free(copy); atomic_fetch_add(&stat_mqtt_drops, 1); return; }
    memcpy(copy, json, len);
    m->data = copy; m->len = len;
    if (blocking_queue_add(&mqtt_queue, m) != 0) {
        free(copy); free(m);
        atomic_fetch_add(&stat_mqtt_drops, 1);
    }
}

void mqtt_cleanup(void)
{
    if (mqtt_writer_running) {
        blocking_queue_close(&mqtt_queue);
        pthread_join(mqtt_writer_tid, NULL);
        for (;;) {
            mqtt_msg_t *m = NULL;
            if (blocking_queue_poll(&mqtt_queue, &m) != 0 || !m) break;
            free(m->data); free(m);
        }
        blocking_queue_destroy(&mqtt_queue);
        mqtt_writer_running = 0;
    }
    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        mosq = NULL;
    }
    mosquitto_lib_cleanup();
}
