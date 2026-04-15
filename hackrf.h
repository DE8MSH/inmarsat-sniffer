/*
 * HackRF native backend for inmarsat-sniffer
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __INMARSAT_HACKRF_H__
#define __INMARSAT_HACKRF_H__

void hackrf_backend_list(void);
void *hackrf_backend_setup(const char *serial);
void *hackrf_stream_thread(void *arg);

#endif
