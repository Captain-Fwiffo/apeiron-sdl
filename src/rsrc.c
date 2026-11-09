#include "rsrc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#endif

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return -1; }
    *buf = (uint8_t *)malloc((size_t)n + 1);
    if (!*buf) { fclose(f); return -1; }
    if (fread(*buf, 1, (size_t)n, f) != (size_t)n) { free(*buf); *buf = NULL; fclose(f); return -1; }
    fclose(f);
    *len = (size_t)n;
    return 0;
}

/* The resource fork inside an AppleDouble file (entry id 2). */
static int appledouble_fork(const uint8_t *b, size_t n, size_t *off, size_t *len)
{
    if (n < 26 || be32(b) != 0x00051607u) return 0;
    unsigned entries = be16(b + 24);
    if (26 + (size_t)entries * 12 > n) return 0;
    for (unsigned i = 0; i < entries; i++) {
        const uint8_t *e = b + 26 + i * 12;
        if (be32(e) == 2) {
            uint32_t o = be32(e + 4), l = be32(e + 8);
            if ((size_t)o + l > n) return 0;
            *off = o; *len = l;
            return 1;
        }
    }
    return 0;
}

/* The resource fork inside a MacBinary I/II/III file. */
static int macbinary_fork(const uint8_t *b, size_t n, size_t *off, size_t *len)
{
    if (n < 128 || b[0] != 0 || b[74] != 0 || b[82] != 0 || b[1] == 0 || b[1] > 63) return 0;
    uint32_t dl = be32(b + 83), rl = be32(b + 87);
    size_t dpad = ((size_t)dl + 127) & ~(size_t)127;
    if (128 + dpad + rl > n + 127 || rl == 0) return 0;
    *off = 128 + dpad;
    *len = rl;
    return (*off + *len <= n) ? 1 : 0;
}

/* A raw fork starts with: data offset, map offset, data length, map length. */
static int looks_like_raw_fork(const uint8_t *b, size_t n)
{
    if (n < 30) return 0;
    uint32_t d = be32(b), m = be32(b + 4), dl = be32(b + 8), ml = be32(b + 12);
    return d >= 16 && m >= d && (uint64_t)d + dl <= m + 1 && (uint64_t)m + ml <= n && ml >= 30;
}

static int parse_map(ResFile *rf)
{
    const uint8_t *f = rf->fork;
    size_t n = rf->fork_len;
    if (!looks_like_raw_fork(f, n)) return -1;
    uint32_t data_off = be32(f), map_off = be32(f + 4), map_len = be32(f + 12);
    const uint8_t *m = f + map_off;
    unsigned type_list = be16(m + 24), name_list = be16(m + 26);
    if (type_list + 2u > map_len) return -1;
    unsigned ntypes = (unsigned)(be16(m + type_list) + 1) & 0xFFFF;
    int total = 0;
    for (unsigned t = 0; t < ntypes; t++) {
        const uint8_t *te = m + type_list + 2 + t * 8;
        if ((size_t)(te - m) + 8 > map_len) return -1;
        total += be16(te + 4) + 1;
    }
    rf->res = (Resource *)calloc((size_t)total + 1, sizeof(Resource));
    if (!rf->res) return -1;
    int k = 0;
    for (unsigned t = 0; t < ntypes; t++) {
        const uint8_t *te = m + type_list + 2 + t * 8;
        unsigned cnt = be16(te + 4) + 1u, ref_off = be16(te + 6);
        for (unsigned j = 0; j < cnt; j++) {
            const uint8_t *re = m + type_list + ref_off + j * 12;
            if ((size_t)(re - m) + 12 > map_len) return -1;
            Resource *r = &rf->res[k];
            memcpy(r->type, te, 4);
            r->type[4] = 0;
            r->id = (int16_t)be16(re);
            unsigned name_off = be16(re + 2);
            uint32_t doff = ((uint32_t)re[5] << 16) | ((uint32_t)re[6] << 8) | re[7];
            size_t p = (size_t)data_off + doff;
            if (p + 4 > n) return -1;
            r->size = be32(f + p);
            if (p + 4 + r->size > n) return -1;
            r->data = f + p + 4;
            if (name_off != 0xFFFF) {
                const uint8_t *np = m + name_list + name_off;
                if ((size_t)(np - m) < map_len) {
                    unsigned l = np[0];
                    if (l > 255) l = 255;
                    memcpy(r->name, np + 1, l);
                    r->name[l] = 0;
                }
            }
            k++;
        }
    }
    rf->count = k;
    return 0;
}

/* On case-sensitive systems the files may be named "apeiron.rsrc" or "APEIRON" rather than "Apeiron": find the real spelling. */
static void fix_case(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
#ifndef _WIN32
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    char dir[1100];
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    DIR *d = opendir(dir[0] ? dir : "/");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcasecmp(e->d_name, base)) { snprintf(out, cap, "%.*s%s%s", slash ? (int)(slash - path) : 0, path, slash ? "/" : "", e->d_name); break; }
    }
    closedir(d);
#endif
}

static int try_path(ResFile *rf, const char *path_in)
{
    char path[1100];
    fix_case(path_in, path, sizeof path);
    uint8_t *buf = NULL;
    size_t n = 0, off = 0, len = 0;
    if (read_file(path, &buf, &n) != 0) return 0;
    int ok = 0;
    if (appledouble_fork(buf, n, &off, &len) || macbinary_fork(buf, n, &off, &len)) {
        rf->fork = (uint8_t *)malloc(len + 1);
        if (rf->fork) { memcpy(rf->fork, buf + off, len); rf->fork_len = len; ok = 1; }
        free(buf);
    } else if (looks_like_raw_fork(buf, n)) {
        rf->fork = buf; rf->fork_len = n; ok = 1;
    } else {
        free(buf);
    }
    if (ok && parse_map(rf) != 0) {
        free(rf->fork); free(rf->res);
        memset(rf, 0, sizeof *rf);
        return 0;
    }
    if (ok) snprintf(rf->source, sizeof rf->source, "%s", path);
    return ok;
}

int rsrc_open(ResFile *rf, const char *dir, const char *name, char *err, size_t errlen)
{
    char path[1100];
    const char *sep = (dir && dir[0]) ? "/" : "";
    const char *d = dir ? dir : "";
    memset(rf, 0, sizeof *rf);
    const char *fmts[] = { "%s%s%s.rsrc", "%s%s._%s", "%s%s%s.bin", "%s%s%s", "%s%s%s.rsrc.bin" };
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        snprintf(path, sizeof path, fmts[i], d, sep, name);
        if (try_path(rf, path)) return 0;
    }
    snprintf(err, errlen, "cannot find a resource fork for '%s' in '%s' (tried NAME.rsrc, ._NAME, NAME.bin, NAME)", name, d);
    return 1;
}

int rsrc_patch(const ResFile *rf, const Resource *r, const uint8_t *data)
{
    uint8_t *buf = NULL;
    size_t n = 0, off = 0, len = 0;
    if (!rf->fork || !r || r->data < rf->fork || r->data + r->size > rf->fork + rf->fork_len) return 1;
    if (read_file(rf->source, &buf, &n) != 0) return 1;
    if (!(appledouble_fork(buf, n, &off, &len) || macbinary_fork(buf, n, &off, &len))) { off = 0; if (!looks_like_raw_fork(buf, n)) { free(buf); return 1; } }
    size_t at = off + (size_t)(r->data - rf->fork);
    if (at + r->size > n) { free(buf); return 1; }
    memcpy(buf + at, data, r->size);
    FILE *f = fopen(rf->source, "wb");
    int rc = (f && fwrite(buf, 1, n, f) == n) ? 0 : 1;
    if (f) fclose(f);
    free(buf);
    return rc;
}

void rsrc_close(ResFile *rf)
{
    free(rf->fork);
    free(rf->res);
    memset(rf, 0, sizeof *rf);
}

const Resource *rsrc_find(const ResFile *rf, const char *type, int id)
{
    for (int i = 0; i < rf->count; i++)
        if (rf->res[i].id == id && memcmp(rf->res[i].type, type, 4) == 0) return &rf->res[i];
    return NULL;
}
