/* Classic Mac resource fork reader.
 *
 * Opens the resource fork of one of Apeiron's files from whatever form the user has it in:
 *   - an AppleDouble file:  <dir>/NAME.rsrc  or  <dir>/._NAME   (what unar and macOS zips produce)
 *   - a MacBinary file:     <dir>/NAME.bin
 *   - a raw resource fork:  <dir>/NAME.rsrc or <dir>/NAME       (detected by its header)
 * and gives access to the resources by type and id. All numbers in resources are big-endian; this
 * module returns raw bytes and the decoders convert.
 */
#ifndef APEIRON_RSRC_H
#define APEIRON_RSRC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char type[5];          /* four characters, NUL terminated ("PICT") */
    int16_t id;
    uint32_t size;
    const uint8_t *data;   /* points into the fork buffer */
    char name[256];        /* resource name, may be empty */
} Resource;

typedef struct {
    uint8_t *fork;         /* the whole resource fork */
    size_t fork_len;
    Resource *res;
    int count;
    char source[1024];     /* the path it was read from */
} ResFile;

/* Opens NAME inside dir (dir may be "." or ""). Returns 0 on success, nonzero with a message in err. */
int rsrc_open(ResFile *rf, const char *dir, const char *name, char *err, size_t errlen);
void rsrc_close(ResFile *rf);

/* Overwrites the data of a resource in the file it was read from (same size only). Returns 0 on success. */
int rsrc_patch(const ResFile *rf, const Resource *r, const uint8_t *data);

/* Finds a resource; returns NULL if absent. */
const Resource *rsrc_find(const ResFile *rf, const char *type, int id);

/* Big-endian readers. */
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

#endif
