/*
 * BaseStation (SBS) format output for aircraft position feeds
 *
 * Outputs MSG,3 position messages compatible with VRS, tar1090,
 * PlanePlotter, and other ADS-B tracking tools.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BASESTATION_H
#define BASESTATION_H

#include <stdint.h>

/*
 * Initialize basestation output.
 * endpoint: "PORT" for TCP server, "HOST:PORT" for TCP client
 * Returns 0 on success, -1 on error.
 */
int basestation_init(const char *endpoint);

/*
 * Send an aircraft position in MSG,3 format to all connected clients
 * (server mode) or to the remote host (client mode).
 *
 * reg:       aircraft registration (looked up in aircraft_db for ICAO hex)
 * flight:    flight ID / callsign (may be empty)
 * lat, lon:  position in decimal degrees
 * alt_ft:    altitude in feet (-99999 if unknown)
 * gs_knots:  groundspeed in knots (-1 if unknown)
 * track_deg: track angle in degrees (-1 if unknown)
 * timestamp: detection time in nanoseconds
 *
 * If gs_knots/track_deg are provided (>= 0), they are used directly
 * in the MSG,3 line. Otherwise, the values are derived from consecutive
 * position fixes when possible.
 */
void basestation_send_position(const char *reg, const char *flight,
                                double lat, double lon,
                                int alt_ft, double gs_knots,
                                double track_deg, uint64_t timestamp_ns);

/*
 * Shut down basestation output (close sockets, stop thread).
 */
void basestation_destroy(void);

#endif
