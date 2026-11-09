/* apeiron_export: converts the resources of Apeiron's original files to PNG and WAV so they can be looked at and checked.
 *
 *   apeiron_export <data_dir> <out_dir>
 *
 * data_dir holds the original files (each as NAME.rsrc, ._NAME, NAME.bin or a raw resource fork): Apeiron, "Apeiron Sprites",
 * "Apeiron Titles", "Apeiron Sounds", "Apeiron Music", "Apeiron Scores", "Register Apeiron". Missing files are skipped.
 * Writes <out_dir>/<file>_PICT_<id>.png, <file>_snd_<id>_<name>.wav and <file>_clut_<id>.png (a colour swatch), and prints a report.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "../src/pict.h"
#include "../src/ppat.h"
#include "../src/rsrc.h"
#include "../src/snd.h"
#include "../src/writers.h"

static const char *kFiles[] = { "Apeiron", "Apeiron Sprites", "Apeiron Titles", "Apeiron Sounds", "Apeiron Music", "Apeiron Scores", "Register Apeiron" };

/* mkdir -p */
static void make_dirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            MKDIR(tmp);
            *p = c;
        }
    }
    MKDIR(tmp);
}

static void safe_name(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; in[i] && k + 1 < n; i++) out[k++] = (isalnum((unsigned char)in[i]) || in[i] == '-') ? in[i] : '_';
    out[k] = 0;
}

static void export_clut(const Resource *r, const char *path)
{
    uint8_t rgb[256 * 3];
    int n = clut_decode(r->data, r->size, rgb);
    if (n <= 0) return;
    int cells = 16, cs = 12;
    int w = cells * cs, h = ((n + cells - 1) / cells) * cs;
    uint8_t *px = (uint8_t *)calloc((size_t)w * h, 3);
    if (!px) return;
    for (int i = 0; i < n; i++)
        for (int y = 0; y < cs; y++)
            for (int x = 0; x < cs; x++) {
                uint8_t *p = &px[((size_t)((i / cells) * cs + y) * w + (i % cells) * cs + x) * 3];
                p[0] = rgb[i * 3]; p[1] = rgb[i * 3 + 1]; p[2] = rgb[i * 3 + 2];
            }
    write_png_rgb(path, w, h, px);
    free(px);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <data_dir> <out_dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1], *out = argv[2];
    make_dirs(out);
    int files_found = 0, total_fail = 0;
    for (size_t fi = 0; fi < sizeof kFiles / sizeof kFiles[0]; fi++) {
        ResFile rf;
        char err[512];
        if (rsrc_open(&rf, dir, kFiles[fi], err, sizeof err) != 0) { printf("%-18s not found\n", kFiles[fi]); continue; }
        files_found++;
        char prefix[128];
        safe_name(kFiles[fi], prefix, sizeof prefix);
        int pict_ok = 0, pict_vec = 0, pict_bad = 0, snd_ok = 0, snd_bad = 0, clut_n = 0, other = 0;
        for (int i = 0; i < rf.count; i++) {
            const Resource *r = &rf.res[i];
            char path[1400], msg[256];
            if (strcmp(r->type, "PICT") == 0) {
                PictImage img;
                PictStatus s = pict_decode(r->data, r->size, &img, msg, sizeof msg);
                if (s == PICT_OK) {
                    snprintf(path, sizeof path, "%s/%s_PICT_%d.png", out, prefix, r->id);
                    int rc = img.indexed ? write_png_indexed(path, img.width, img.height, img.pixels, img.palette, img.palette_size)
                                         : write_png_rgb(path, img.width, img.height, img.pixels);
                    if (rc == 0) pict_ok++; else { pict_bad++; printf("  PICT %d: cannot write %s\n", r->id, path); }
                    pict_free(&img);
                } else if (s == PICT_NO_BITMAP) {
                    pict_vec++;
                } else {
                    pict_bad++;
                    printf("  PICT %d (%u bytes): %s\n", r->id, r->size, msg);
                }
            } else if (strcmp(r->type, "snd ") == 0) {
                SndData sd;
                if (snd_decode(r->data, r->size, &sd, msg, sizeof msg) == 0) {
                    char nm[160];
                    safe_name(r->name, nm, sizeof nm);
                    snprintf(path, sizeof path, "%s/%s_snd_%d_%s.wav", out, prefix, r->id, nm);
                    if (write_wav8(path, sd.samples, sd.frames, (unsigned)(sd.rate_hz + 0.5)) == 0) snd_ok++; else snd_bad++;
                } else {
                    snd_bad++;
                    printf("  snd %d: %s\n", r->id, msg);
                }
            } else if (strcmp(r->type, "ppat") == 0) {
                PictImage img;
                if (ppat_decode(r->data, r->size, &img, msg, sizeof msg) == 0) {
                    snprintf(path, sizeof path, "%s/%s_ppat_%d.png", out, prefix, r->id);
                    write_png_indexed(path, img.width, img.height, img.pixels, img.palette, img.palette_size);
                    printf("  ppat %d: %dx%d, %d-bit\n", r->id, img.width, img.height, img.depth);
                    pict_free(&img);
                } else { printf("  ppat %d: %s\n", r->id, msg); }
            } else if (strcmp(r->type, "clut") == 0) {
                snprintf(path, sizeof path, "%s/%s_clut_%d.png", out, prefix, r->id);
                export_clut(r, path);
                clut_n++;
            } else {
                other++;
            }
        }
        printf("%-18s %3d resources: PICT %d ok, %d vector-only, %d failed; snd %d ok, %d failed; clut %d; other %d\n",
               kFiles[fi], rf.count, pict_ok, pict_vec, pict_bad, snd_ok, snd_bad, clut_n, other);
        total_fail += pict_bad + snd_bad;
        rsrc_close(&rf);
    }
    if (files_found == 0) { fprintf(stderr, "no Apeiron files found in '%s'\n", dir); return 1; }
    printf("%d file(s) read, %d resource(s) failed\n", files_found, total_fail);
    return total_fail ? 3 : 0;
}
