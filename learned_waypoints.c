/*
 * Learned waypoints: runtime cache of waypoint name -> coordinate
 *
 * Parses FPN (Flight Plan) messages from ACARS to extract (NAME, COORD)
 * pairs and cache them in memory. When a later message references a
 * waypoint by name without coordinates, this cache is checked as a
 * fallback to the bundled waypoint database.
 *
 * This is Phase 1: runtime-only, no persistence. Cache is cleared on exit.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "learned_waypoints.h"

#define LEARNED_WP_MAX 4096

typedef struct {
    char ident[6];
    double lat, lon;
    int sightings;
} learned_entry_t;

static learned_entry_t cache[LEARNED_WP_MAX];
static int cache_count = 0;
static int stat_fpn_parsed = 0;
static int stat_sightings = 0;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Parse the degrees-and-tenths-of-minutes format used in FPN messages.
 *   N34288 -> 34° 28.8' N -> 34.48 (positive)
 *   W083591 -> 083° 59.1' W -> -83.985 (negative)
 *
 * Returns 1 on success, 0 on malformed input.
 */
static int parse_dm_coord(const char *s, int lon_width, double *out)
{
    char dir = s[0];
    if (dir != 'N' && dir != 'S' && dir != 'E' && dir != 'W')
        return 0;

    int deg_digits = lon_width ? 3 : 2;
    int min_digits = 3;
    int total = deg_digits + min_digits;

    for (int i = 1; i <= total; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
    }

    int deg = 0;
    for (int i = 0; i < deg_digits; i++)
        deg = deg * 10 + (s[1 + i] - '0');

    int min10 = 0;
    for (int i = 0; i < min_digits; i++)
        min10 = min10 * 10 + (s[1 + deg_digits + i] - '0');

    /* Minutes*10 must be < 600 (minutes range 0-59.9) */
    if (min10 >= 600)
        return 0;

    double val = deg + (min10 / 600.0);

    /* Range check */
    if (lon_width && val > 180.0) return 0;
    if (!lon_width && val > 90.0) return 0;

    if (dir == 'S' || dir == 'W')
        val = -val;

    *out = val;
    return 1;
}

/*
 * Add a (name, lat, lon) to the cache, merging with existing entry by name.
 * Must be called with cache_lock held.
 */
static void cache_add_locked(const char *name, double lat, double lon)
{
    /* Find existing entry */
    for (int i = 0; i < cache_count; i++) {
        if (strcmp(cache[i].ident, name) == 0) {
            /* Already have this waypoint. If coords differ significantly,
             * this could be a parse error or renamed waypoint -- for Phase 1
             * just keep the first sighting and count. */
            cache[i].sightings++;
            stat_sightings++;
            return;
        }
    }

    /* New entry */
    if (cache_count >= LEARNED_WP_MAX)
        return;  /* cache full */

    strncpy(cache[cache_count].ident, name, 5);
    cache[cache_count].ident[5] = '\0';
    cache[cache_count].lat = lat;
    cache[cache_count].lon = lon;
    cache[cache_count].sightings = 1;
    cache_count++;
    stat_sightings++;
}

int learned_wp_parse_fpn(const char *text)
{
    if (!text || !text[0])
        return 0;

    int learned = 0;
    const char *p = text;

    /* Scan for patterns: WAYPOINT_NAME,N/S dddddE/W dddddd
     * Valid waypoint names: 2-5 uppercase letters immediately preceding comma,
     * followed by N or S plus 5 digits plus E or W plus 6 digits. */
    while (*p) {
        /* Find next comma */
        const char *comma = strchr(p, ',');
        if (!comma) break;

        /* Look backward from comma for waypoint name (2-5 uppercase letters) */
        const char *name_end = comma;
        const char *name_start = comma;
        while (name_start > p && name_start[-1] >= 'A' && name_start[-1] <= 'Z')
            name_start--;

        int name_len = name_end - name_start;
        if (name_len < 2 || name_len > 5) {
            p = comma + 1;
            continue;
        }

        /* Look forward after comma for coordinate pattern */
        const char *after = comma + 1;
        if (*after != 'N' && *after != 'S') {
            p = comma + 1;
            continue;
        }

        /* Need 6 chars for lat (NDDmmm) then 7 chars for lon (WDDDmmm) */
        double lat = 0, lon = 0;
        if (!parse_dm_coord(after, 0, &lat)) {
            p = comma + 1;
            continue;
        }

        const char *lon_start = after + 6;
        if (*lon_start != 'E' && *lon_start != 'W') {
            p = comma + 1;
            continue;
        }
        if (!parse_dm_coord(lon_start, 1, &lon)) {
            p = comma + 1;
            continue;
        }

        /* Valid pair found */
        char name[6];
        memcpy(name, name_start, name_len);
        name[name_len] = '\0';

        /* Sanity: not (0,0) */
        if (lat != 0.0 || lon != 0.0) {
            pthread_mutex_lock(&cache_lock);
            cache_add_locked(name, lat, lon);
            pthread_mutex_unlock(&cache_lock);
            learned++;
        }

        /* Advance past the coordinate */
        p = lon_start + 7;
    }

    if (learned > 0)
        stat_fpn_parsed++;

    return learned;
}

int learned_wp_lookup(const char *ident, double *lat, double *lon)
{
    if (!ident || !ident[0] || !lat || !lon)
        return 0;

    int found = 0;
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < cache_count; i++) {
        if (strcmp(cache[i].ident, ident) == 0) {
            *lat = cache[i].lat;
            *lon = cache[i].lon;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&cache_lock);

    return found;
}

int learned_wp_count(void)
{
    return cache_count;
}

void learned_wp_print_stats(void)
{
    if (stat_fpn_parsed == 0 && cache_count == 0)
        return;
    fprintf(stderr, "Learned waypoints: %d unique from %d FPN messages "
            "(%d total sightings)\n",
            cache_count, stat_fpn_parsed, stat_sightings);
}

void learned_wp_destroy(void)
{
    cache_count = 0;
    stat_fpn_parsed = 0;
    stat_sightings = 0;
}
