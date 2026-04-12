/*
 * MQTT output for decoded ACARS messages
 *
 * Publishes each decoded ACARS message as a JSON string to a
 * configurable MQTT topic. Uses libmosquitto with auto-reconnect.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mosquitto.h>

#include "mqtt.h"

static struct mosquitto *mosq = NULL;
static char topic[256] = "inmarsat-sniffer/acars";
static int connected = 0;

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

    fprintf(stderr, "MQTT: publishing to %s:%d topic=%s\n", host, port, topic);
    return 0;
}

void mqtt_publish_json(const char *json, int len)
{
    if (!mosq || !connected) return;
    mosquitto_publish(mosq, NULL, topic, len, json, 0, false);
}

void mqtt_cleanup(void)
{
    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        mosq = NULL;
    }
    mosquitto_lib_cleanup();
}
