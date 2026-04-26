/*
 * Aircraft position extraction from ACARS text payloads.
 *
 * Ported verbatim from iridium-sniffer sbd_acars.c (same author, same
 * license). The label whitelist is conservative — only labels known to
 * carry the aircraft's current position are accepted, to avoid treating
 * waypoints or flight-plan coordinates as the aircraft's own fix.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "acars_position.h"
#include "waypoint_db.h"
#include "learned_waypoints.h"

int acars_extract_text_position(const char *label, const char *text,
                                 double *lat, double *lon)
{
    if (!label || !text || !text[0])
        return 0;

    /* Labels known to carry current aircraft position in text:
     *   H1  - position/met report (POS preamble)
     *   20  - position report (POS preamble)
     *   44  - OOOI + position
     *   4J  - position and weather
     *   15  - general aviation position report
     *   SA  - in-range/fuel status (some carriers include position)
     *
     * NOT included (carry waypoints/destinations, not current position):
     *   H1/FPN - flight plan (future route)
     *   5Z     - too variable, airline-specific
     */
    int label_ok = 0;
    if (label[0] == 'H' && label[1] == '1') label_ok = 1;
    else if (label[0] == '2' && label[1] == '0') label_ok = 1;
    else if (label[0] == '4' && label[1] == '4') label_ok = 1;
    else if (label[0] == '4' && label[1] == 'J') label_ok = 1;
    else if (label[0] == '1' && label[1] == '5') label_ok = 1;
    else if (label[0] == 'S' && label[1] == 'A') label_ok = 1;

    if (!label_ok)
        return 0;

    /* Skip H1 flight plan messages (FPN preamble = future route, not
     * current position) */
    if (label[0] == 'H' && label[1] == '1') {
        if (strncmp(text, "FPN", 3) == 0)
            return 0;
    }

    /*
     * Format 1: POS[NS]ddddd[EW]dddddd  (degrees * 1000)
     *   Example: POSN43312W123174 = 43.312N, 123.174W
     *   Used in H1/POS and label 20/POS messages.
     */
    const char *p = strstr(text, "POS");
    if (p) {
        p += 3;
        char lat_dir = *p;
        if (lat_dir == 'N' || lat_dir == 'S') {
            p++;
            int lat_digits = 0;
            const char *lp = p;
            while (*p >= '0' && *p <= '9' && lat_digits < 5) {
                lat_digits++;
                p++;
            }
            if (lat_digits == 5) {
                char lon_dir = *p;
                if (lon_dir == 'E' || lon_dir == 'W') {
                    p++;
                    int lon_digits = 0;
                    const char *lop = p;
                    while (*p >= '0' && *p <= '9' && lon_digits < 6) {
                        lon_digits++;
                        p++;
                    }
                    if (lon_digits == 6) {
                        char lat_buf[6], lon_buf[7];
                        memcpy(lat_buf, lp, 5); lat_buf[5] = '\0';
                        memcpy(lon_buf, lop, 6); lon_buf[6] = '\0';
                        double la = atof(lat_buf) / 1000.0;
                        double lo = atof(lon_buf) / 1000.0;
                        if (la >= 0 && la <= 90 && lo >= 0 && lo <= 180) {
                            *lat = (lat_dir == 'S') ? -la : la;
                            *lon = (lon_dir == 'W') ? -lo : lo;
                            return 1;
                        }
                    }
                }
            }
        }
    }

    /*
     * Format 2: [NS]dddmm[EW]dddmm  (degrees + minutes, no decimal)
     *   Example: N33521W084123 = 33d52.1'N, 084d12.3'W = 33.868N, 84.205W
     *   Common across many ACARS labels and carriers.
     *   Latitude: 2 digits degrees + 3 digits (minutes * 10)
     *   Longitude: 3 digits degrees + 3 digits (minutes * 10)
     */
    for (const char *s = text; *s; s++) {
        if ((*s == 'N' || *s == 'S') && s[1] >= '0' && s[1] <= '9') {
            char lat_dir = *s;
            s++;
            int n = 0;
            const char *lat_start = s;
            while (s[n] >= '0' && s[n] <= '9') n++;
            if (n == 5 && (s[n] == 'E' || s[n] == 'W')) {
                char lon_dir = s[n];
                const char *lon_start = s + n + 1;
                int m = 0;
                while (lon_start[m] >= '0' && lon_start[m] <= '9') m++;
                if (m == 6) {
                    int lat_deg = (lat_start[0] - '0') * 10 +
                                  (lat_start[1] - '0');
                    int lat_min10 = (lat_start[2] - '0') * 100 +
                                    (lat_start[3] - '0') * 10 +
                                    (lat_start[4] - '0');
                    int lon_deg = (lon_start[0] - '0') * 100 +
                                  (lon_start[1] - '0') * 10 +
                                  (lon_start[2] - '0');
                    int lon_min10 = (lon_start[3] - '0') * 100 +
                                    (lon_start[4] - '0') * 10 +
                                    (lon_start[5] - '0');

                    if (lat_deg <= 90 && lat_min10 < 600 &&
                        lon_deg <= 180 && lon_min10 < 600) {
                        double la = lat_deg + lat_min10 / 600.0;
                        double lo = lon_deg + lon_min10 / 600.0;
                        *lat = (lat_dir == 'S') ? -la : la;
                        *lon = (lon_dir == 'W') ? -lo : lo;
                        return 1;
                    }
                }
            }
            s = lat_start - 1;
        }
    }

    /*
     * Format 3: LAT [NS] dd.ddd/LON [EW] ddd.ddd  (decimal degrees, tagged)
     *   Example: LAT N 33.917/LON W 77.988
     */
    p = strstr(text, "LAT");
    if (p) {
        p += 3;
        while (*p == ' ') p++;
        char lat_dir = *p;
        if (lat_dir == 'N' || lat_dir == 'S') {
            p++;
            while (*p == ' ') p++;
            char *end;
            double la = strtod(p, &end);
            if (end != p && la >= 0 && la <= 90) {
                const char *lp = strstr(end, "LON");
                if (lp) {
                    lp += 3;
                    while (*lp == ' ') lp++;
                    char lon_dir = *lp;
                    if (lon_dir == 'E' || lon_dir == 'W') {
                        lp++;
                        while (*lp == ' ') lp++;
                        double lo = strtod(lp, &end);
                        if (end != lp && lo >= 0 && lo <= 180) {
                            *lat = (lat_dir == 'S') ? -la : la;
                            *lon = (lon_dir == 'W') ? -lo : lo;
                            return 1;
                        }
                    }
                }
            }
        }
    }

    return 0;
}

/*
 * Altitude extraction. Tried formats, in order:
 *  1. FLnnn        e.g., "FL350" -> 35000 ft
 *  2. nnnF         e.g., "TUPAC289F" at end-of-token -> FL289 -> 28900 ft
 *                  (MDPOS trailing flight level — must be followed by a
 *                   delimiter or EOL and preceded by a non-digit to avoid
 *                   matching parts of lat/lon coordinates)
 *  3. ALT nnnnn    e.g., "ALT 35000"
 *  4. nnnnnFT      e.g., "35000FT"
 * Returns 1 on match, fills *alt_ft in feet.
 */
static int is_digit(char c) { return c >= '0' && c <= '9'; }

int acars_extract_text_altitude(const char *text, int *alt_ft)
{
    if (!text || !text[0] || !alt_ft) return 0;

    /* Format 1: FLnnn */
    for (const char *p = strstr(text, "FL"); p; p = strstr(p + 2, "FL")) {
        const char *q = p + 2;
        if (is_digit(q[0]) && is_digit(q[1]) && is_digit(q[2]) && !is_digit(q[3])) {
            int fl = (q[0]-'0')*100 + (q[1]-'0')*10 + (q[2]-'0');
            if (fl > 0 && fl < 600) { *alt_ft = fl * 100; return 1; }
        }
    }

    /* Format 2: nnnF (flight level + F suffix at word boundary) */
    for (const char *s = text; s[0] && s[1] && s[2] && s[3]; s++) {
        if (is_digit(s[0]) && is_digit(s[1]) && is_digit(s[2]) && s[3] == 'F') {
            /* Require non-digit before (avoid mid-coordinate matches like
             * "000W060" where the 000 lives inside a lat/lon) */
            if (s != text && is_digit(s[-1])) continue;
            /* Require delimiter or end-of-string after the F */
            char after = s[4];
            if (after != '\0' && after != '.' && after != ',' &&
                after != ' '  && after != '\r' && after != '\n' &&
                after != '/') continue;
            int fl = (s[0]-'0')*100 + (s[1]-'0')*10 + (s[2]-'0');
            if (fl > 0 && fl < 600) { *alt_ft = fl * 100; return 1; }
        }
    }

    /* Format 3: ALT <digits> */
    const char *p = strstr(text, "ALT");
    if (p) {
        p += 3;
        while (*p == ' ' || *p == '\t') p++;
        if (is_digit(*p)) {
            char *end;
            long v = strtol(p, &end, 10);
            if (end != p && v > 0 && v < 60000) { *alt_ft = (int)v; return 1; }
        }
    }

    /* Format 4: nnnnnFT (5-digit feet with FT suffix) */
    for (const char *s = text; s[0] && s[1] && s[2] && s[3] && s[4] && s[5]; s++) {
        if (is_digit(s[0]) && is_digit(s[1]) && is_digit(s[2]) &&
            is_digit(s[3]) && is_digit(s[4]) &&
            s[5] == 'F' && s[6] == 'T') {
            if (s != text && is_digit(s[-1])) continue;
            int v = (s[0]-'0')*10000 + (s[1]-'0')*1000 + (s[2]-'0')*100 +
                    (s[3]-'0')*10   + (s[4]-'0');
            if (v > 0 && v < 60000) { *alt_ft = v; return 1; }
        }
    }

    return 0;
}

int acars_extract_waypoint_position(const char *label, const char *text,
                                     double *lat, double *lon)
{
    if (!label || !text || !text[0]) return 0;

    /* Same label gate as coordinate extraction — waypoints scanned from
     * non-position labels would be destinations/routes, not a fix. */
    int label_ok = 0;
    if (label[0] == 'H' && label[1] == '1') label_ok = 1;
    else if (label[0] == '2' && label[1] == '0') label_ok = 1;
    else if (label[0] == '4' && label[1] == '4') label_ok = 1;
    else if (label[0] == '4' && label[1] == 'J') label_ok = 1;
    else if (label[0] == '1' && label[1] == '5') label_ok = 1;
    else if (label[0] == 'S' && label[1] == 'A') label_ok = 1;
    if (!label_ok) return 0;

    if (label[0] == 'H' && label[1] == '1' && strncmp(text, "FPN", 3) == 0)
        return 0;

    const char *scan = text;
    if (strncmp(scan, "POS", 3) == 0) scan += 3;

    while (*scan) {
        while (*scan && *scan != ',' && *scan != ' ' &&
               *scan != '/' && *scan != '.')
            scan++;
        while (*scan == ',' || *scan == ' ' ||
               *scan == '/' || *scan == '.')
            scan++;

        if (scan[0] >= 'A' && scan[0] <= 'Z' &&
            scan[1] >= 'A' && scan[1] <= 'Z' &&
            scan[2] >= 'A' && scan[2] <= 'Z' &&
            scan[3] >= 'A' && scan[3] <= 'Z' &&
            scan[4] >= 'A' && scan[4] <= 'Z' &&
            (scan[5] == ',' || scan[5] == ' ' || scan[5] == '/' ||
             scan[5] == '.' || scan[5] == '\0' || scan[5] == '\r' ||
             scan[5] == '\n')) {
            char ident[6];
            memcpy(ident, scan, 5);
            ident[5] = '\0';
            if (waypoint_db_lookup(ident, lat, lon))
                return 1;
            /* Fall back to runtime-learned cache (harvested from FPN). */
            if (learned_wp_lookup(ident, lat, lon))
                return 1;
        }

        /* Advance to the next delimiter — must include '.' to match the
         * top-of-loop skip set. Without '.' here, "240050.BNJEE" would
         * tokenize as one chunk and the BNJEE lookup never fires. */
        while (*scan && *scan != ',' && *scan != ' ' &&
               *scan != '/' && *scan != '.')
            scan++;
    }
    return 0;
}
