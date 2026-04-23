/*
 * Learned waypoints: runtime cache of waypoint name -> coordinate
 *
 * Aircraft FPN (Flight Plan) messages in ACARS carry waypoint names with
 * their coordinates from the aircraft's Jeppesen/Navigraph AIRAC database.
 * This module caches (NAME, COORD) pairs seen in FPN messages and serves
 * them as a fallback when the bundled waypoint database doesn't have an
 * entry for a name referenced in a later position report.
 *
 * Phase 1: runtime-only, no persistence. Cache is cleared on process exit.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LEARNED_WAYPOINTS_H
#define LEARNED_WAYPOINTS_H

/*
 * Parse an FPN (Flight Plan) message body for WAYPOINT,COORD pairs and
 * add them to the runtime cache. Returns number of waypoints learned.
 *
 * Expected format embedded in text:
 *   ...NAME,N34288W083591..NAME2,N33144W084056..
 * Where N34288W083591 = 34° 28.8' N, 083° 59.1' W (degrees + tenths-of-minutes).
 */
int learned_wp_parse_fpn(const char *text);

/*
 * Look up a waypoint by name in the runtime cache.
 * Returns 1 if found (lat/lon populated), 0 if not.
 */
int learned_wp_lookup(const char *ident, double *lat, double *lon);

/*
 * Return the total number of unique waypoints learned in this session.
 */
int learned_wp_count(void);

/*
 * Print statistics (called at shutdown).
 */
void learned_wp_print_stats(void);

/*
 * Free cache resources.
 */
void learned_wp_destroy(void);

#endif
