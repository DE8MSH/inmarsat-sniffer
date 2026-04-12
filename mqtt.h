/*
 * MQTT output for decoded ACARS messages
 *
 * Optional — compiled only when libmosquitto is present.
 * Publishes each ACARS message as JSON to a configurable topic.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __INMARSAT_MQTT_H__
#define __INMARSAT_MQTT_H__

int mqtt_init(const char *host, int port,
              const char *user, const char *pass,
              const char *topic_prefix);
void mqtt_publish_json(const char *json, int len);
void mqtt_cleanup(void);

#endif
