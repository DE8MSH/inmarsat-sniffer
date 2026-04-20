/*
 * Aircraft registration to ICAO hex lookup database
 *
 * Loads tar1090-db aircraft.csv and provides fast binary-search lookup
 * of registration -> ICAO hex address.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aircraft_db.h"

#define DB_URL "https://github.com/wiedehopf/tar1090-db/raw/refs/heads/csv/aircraft.csv.gz"

typedef struct {
    char reg[12];            /* registration (e.g., "N712TW") */
    char hex[7];             /* ICAO hex (e.g., "A98539") */
    /* Indices into strings buffer (-1 if field empty). Use indices
     * instead of char pointers so realloc doesn't invalidate them. */
    int  type_idx;           /* ICAO type code (e.g., "B738") */
    int  desc_idx;           /* long description (e.g., "BOEING 737-800") */
    int  op_idx;             /* operator / airline */
} db_entry_t;

static db_entry_t *db = NULL;
static int db_count = 0;
static int db_capacity = 0;

/* Single contiguous strings buffer for all extra fields.
 * Each null-terminated string; entries point via indices above. */
static char *strings = NULL;
static size_t strings_len = 0;
static size_t strings_cap = 0;

/* Separate array sorted by hex for AES→info lookup */
static int *by_hex = NULL;  /* indices into db[] sorted by hex */

static int entry_cmp(const void *a, const void *b)
{
    return strcmp(((const db_entry_t *)a)->reg,
                 ((const db_entry_t *)b)->reg);
}

static int by_hex_cmp(const void *a, const void *b)
{
    int ai = *(const int *)a;
    int bi = *(const int *)b;
    return strcmp(db[ai].hex, db[bi].hex);
}

/* Append a string to the strings buffer, return its index (or -1 if empty) */
static int strings_add(const char *s)
{
    if (!s || !*s) return -1;
    size_t slen = strlen(s);
    if (strings_len + slen + 1 > strings_cap) {
        size_t new_cap = strings_cap * 2;
        if (new_cap < strings_len + slen + 1) new_cap = strings_len + slen + 1024;
        char *new_buf = realloc(strings, new_cap);
        if (!new_buf) return -1;
        strings = new_buf;
        strings_cap = new_cap;
    }
    int idx = (int)strings_len;
    memcpy(strings + strings_len, s, slen + 1);
    strings_len += slen + 1;
    return idx;
}

static const char *strings_get(int idx)
{
    return (idx >= 0 && strings) ? (strings + idx) : NULL;
}

/*
 * Normalize a registration string: strip leading dots, uppercase,
 * remove dashes/spaces.
 */
static void normalize_reg(const char *src, char *dst, int dstlen)
{
    /* Skip leading dots */
    while (*src == '.') src++;

    int j = 0;
    for (int i = 0; src[i] && j < dstlen - 1; i++) {
        if (src[i] == '-' || src[i] == ' ')
            continue;
        dst[j++] = toupper((unsigned char)src[i]);
    }
    dst[j] = '\0';
}

int aircraft_db_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "aircraft_db: cannot open %s\n", path);
        return -1;
    }

    aircraft_db_destroy();

    db_capacity = 700000;
    db = malloc(sizeof(db_entry_t) * db_capacity);
    if (!db) {
        fclose(f);
        return -1;
    }

    /* Allocate strings buffer — assume ~30 chars avg per entry */
    strings_cap = 700000 * 64;
    strings = malloc(strings_cap);
    strings_len = 0;
    if (!strings) {
        fclose(f);
        aircraft_db_destroy();
        return -1;
    }

    char line[1024];
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Format: hex;reg;type;flags;desc;year;operator;... (tar1090-db) */
        char *fields[8] = {0};
        fields[0] = line;
        int nf = 1;
        for (char *p = line; *p && nf < 8; p++) {
            if (*p == ';') {
                *p = '\0';
                fields[nf++] = p + 1;
            }
        }
        /* Strip newline from last field */
        if (nf >= 1) {
            char *nl = strchr(fields[nf-1], '\n');
            if (nl) *nl = '\0';
            nl = strchr(fields[nf-1], '\r');
            if (nl) *nl = '\0';
        }

        char *hex_field = fields[0];
        char *reg_field = nf >= 2 ? fields[1] : NULL;
        char *type_field = nf >= 3 ? fields[2] : NULL;
        char *desc_field = nf >= 5 ? fields[4] : NULL;
        char *op_field = nf >= 7 ? fields[6] : NULL;

        /* Validate hex (6 chars, hex digits) */
        int hex_len = (int)strlen(hex_field);
        if (hex_len != 6) continue;
        int hex_ok = 1;
        for (int i = 0; i < 6; i++) {
            char c = hex_field[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                  (c >= 'a' && c <= 'f'))) {
                hex_ok = 0;
                break;
            }
        }
        if (!hex_ok) continue;
        if (!reg_field || !reg_field[0]) continue;

        if (db_count >= db_capacity) {
            db_capacity *= 2;
            db_entry_t *new_db = realloc(db, sizeof(db_entry_t) * db_capacity);
            if (!new_db) break;
            db = new_db;
        }

        normalize_reg(reg_field, db[db_count].reg, sizeof(db[db_count].reg));
        if (!db[db_count].reg[0]) continue;

        /* Store hex uppercase */
        for (int i = 0; i < 6; i++)
            db[db_count].hex[i] = toupper((unsigned char)hex_field[i]);
        db[db_count].hex[6] = '\0';

        db[db_count].type_idx = strings_add(type_field);
        db[db_count].desc_idx = strings_add(desc_field);
        db[db_count].op_idx   = strings_add(op_field);

        db_count++;
        loaded++;
    }

    fclose(f);

    /* Sort by registration for binary search */
    qsort(db, db_count, sizeof(db_entry_t), entry_cmp);

    /* Build hex-sorted index for AES → info lookup */
    by_hex = malloc(db_count * sizeof(int));
    if (by_hex) {
        for (int i = 0; i < db_count; i++) by_hex[i] = i;
        qsort(by_hex, db_count, sizeof(int), by_hex_cmp);
    }

    fprintf(stderr, "aircraft_db: loaded %d entries from %s (%.1f MB strings)\n",
            loaded, path, strings_len / 1048576.0);
    return loaded;
}

const char *aircraft_db_lookup(const char *registration)
{
    if (!db || db_count == 0 || !registration || !registration[0])
        return NULL;

    char norm[12];
    normalize_reg(registration, norm, sizeof(norm));
    if (!norm[0]) return NULL;

    db_entry_t key;
    strncpy(key.reg, norm, sizeof(key.reg));
    key.reg[sizeof(key.reg) - 1] = '\0';

    db_entry_t *found = bsearch(&key, db, db_count, sizeof(db_entry_t),
                                 entry_cmp);
    return found ? found->hex : NULL;
}

void aircraft_db_destroy(void)
{
    free(db);
    free(by_hex);
    free(strings);
    db = NULL;
    by_hex = NULL;
    strings = NULL;
    db_count = 0;
    db_capacity = 0;
    strings_len = 0;
    strings_cap = 0;
}

int aircraft_db_lookup_by_hex(const char *hex6, aircraft_info_t *out)
{
    if (!db || !by_hex || !hex6) return 0;

    /* Normalize to uppercase */
    char key_hex[7];
    for (int i = 0; i < 6 && hex6[i]; i++)
        key_hex[i] = toupper((unsigned char)hex6[i]);
    key_hex[6] = '\0';

    /* Binary search over by_hex[] */
    int lo = 0, hi = db_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = strcmp(db[by_hex[mid]].hex, key_hex);
        if (cmp == 0) {
            if (out) {
                db_entry_t *e = &db[by_hex[mid]];
                out->registration = e->reg;
                out->type         = strings_get(e->type_idx);
                out->description  = strings_get(e->desc_idx);
                out->operator_    = strings_get(e->op_idx);
            }
            return 1;
        }
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

int aircraft_db_lookup_by_aes(uint32_t aes_id, aircraft_info_t *out)
{
    char hex6[7];
    snprintf(hex6, sizeof(hex6), "%06X", aes_id & 0xFFFFFF);
    return aircraft_db_lookup_by_hex(hex6, out);
}

const char *aircraft_db_default_path(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(path, sizeof(path), "%s/.inmarsat-sniffer/aircraft.csv", home);
    return path;
}

int aircraft_db_update(void)
{
    const char *path = aircraft_db_default_path();
    if (!path) {
        warnx("aircraft_db: cannot determine HOME directory");
        return -1;
    }

    /* Create directory */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.inmarsat-sniffer", getenv("HOME"));
    mkdir(dir, 0755);

    /* Download and decompress */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "curl -sL '%s' | gunzip > '%s.tmp' && mv '%s.tmp' '%s'",
             DB_URL, path, path, path);

    fprintf(stderr, "aircraft_db: downloading from tar1090-db...\n");
    int ret = system(cmd);
    if (ret != 0) {
        warnx("aircraft_db: download failed (is curl installed?)");
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 1000) {
        warnx("aircraft_db: downloaded file is too small or missing");
        unlink(path);
        return -1;
    }

    fprintf(stderr, "aircraft_db: saved to %s (%.1f MB)\n",
            path, st.st_size / 1e6);
    return 0;
}
