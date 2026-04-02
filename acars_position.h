/*
 * Aircraft position extraction from ACARS text payloads.
 *
 * Ported from iridium-sniffer (same author, same license).
 * Uses a label whitelist to avoid picking up waypoints or flight-plan
 * coordinates instead of the aircraft's current position.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __ACARS_POSITION_H__
#define __ACARS_POSITION_H__

/*
 * Attempt to extract the aircraft's current position from an ACARS
 * message's label + text body. Returns 1 on success (and fills *lat,
 * *lon), 0 otherwise.
 */
int acars_extract_text_position(const char *label, const char *text,
                                 double *lat, double *lon);

/*
 * Waypoint-name fallback: scan ACARS text for 5-letter ICAO waypoint
 * idents and look them up via waypoint_db. Used only when coordinate
 * extraction fails but the message is on a position-carrying label.
 * Returns 1 on success.
 */
int acars_extract_waypoint_position(const char *label, const char *text,
                                     double *lat, double *lon);

#endif
