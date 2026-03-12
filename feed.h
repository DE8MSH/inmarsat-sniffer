/*
 * JSON feed output and UDP forwarding
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __FEED_H__
#define __FEED_H__

#include "stdc_decode.h"
#include "aero_decode.h"

/* Initialize the feed subsystem (opens UDP sockets, etc).
 * Call after parse_options(). */
void feed_init(void);

/* Send a decoded STD-C message as JSON.
 * Writes to stdout if --feed is enabled.
 * Sends via UDP if --udp endpoints are configured. */
void feed_stdc_message(const stdc_message_t *msg);

/* Send a decoded Aero/ACARS message as JSON.
 * Output format is compatible with JAERO's JSON feed. */
void feed_aero_message(const aero_message_t *msg);

/* Shut down the feed subsystem. */
void feed_shutdown(void);

#endif
