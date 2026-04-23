/*
 * Embedded web dashboard with live map
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WEB_H
#define WEB_H

#include "stdc_decode.h"
#include "aero_decode.h"

/* Start the web server on the given port.
 * Returns 0 on success, -1 on error. */
int web_init(int port);

/* Add a decoded STD-C message to the map state. */
void web_add_stdc(const stdc_message_t *msg);

/* Add a decoded Aero/ACARS message to the map state. */
void web_add_aero(const aero_message_t *msg);

/* Shut down the web server. */
void web_shutdown(void);

#endif
