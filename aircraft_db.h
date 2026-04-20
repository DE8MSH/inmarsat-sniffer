/*
 * Aircraft registration to ICAO hex lookup database
 *
 * Loads the tar1090-db aircraft.csv format (semicolon-separated,
 * fields: icao_hex;registration;type;flags;description;...)
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __AIRCRAFT_DB_H__
#define __AIRCRAFT_DB_H__

#include <stdint.h>

/*
 * Load aircraft database from CSV file.
 * Expected format: icao_hex;registration;... (semicolon-separated)
 * Returns number of entries loaded, or -1 on error.
 */
int aircraft_db_load(const char *path);

/*
 * Look up ICAO hex address by aircraft registration.
 * Returns pointer to 6-char hex string (static, valid until next load),
 * or NULL if not found.
 */
const char *aircraft_db_lookup(const char *registration);

/*
 * Look up aircraft info by ICAO hex / AES ID (24-bit as hex string or uint).
 * Returns 1 on found, 0 on not found. All output pointers may be NULL.
 * Strings are owned by the DB (valid until next load/destroy).
 */
typedef struct {
    const char *registration;
    const char *type;        /* ICAO type code, e.g., "B738" */
    const char *description; /* Aircraft full description, e.g., "Boeing 737-800" */
    const char *operator_;   /* Airline/operator */
} aircraft_info_t;

int aircraft_db_lookup_by_hex(const char *hex6, aircraft_info_t *out);
int aircraft_db_lookup_by_aes(uint32_t aes_id, aircraft_info_t *out);

/*
 * Free all database resources.
 */
void aircraft_db_destroy(void);

/*
 * Download/update the aircraft database from the tar1090-db project.
 * Stores at ~/.iridium-sniffer/aircraft.csv
 * Returns 0 on success, -1 on error.
 */
int aircraft_db_update(void);

/*
 * Get default database path (~/.iridium-sniffer/aircraft.csv).
 * Returns static buffer.
 */
const char *aircraft_db_default_path(void);

#endif
