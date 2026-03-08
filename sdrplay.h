/*
 * SDRplay native API v3 backend for inmarsat-sniffer
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __SDRPLAY_H__
#define __SDRPLAY_H__

void sdrplay_list(void);
void *sdrplay_setup(const char *serial);
void *sdrplay_stream_thread(void *arg);
void sdrplay_close(void *ctx);

#endif
