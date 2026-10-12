/* The disc's own menu text.
 *
 * The decompilation targets MGS Integral and carries its Japanese item and
 * weapon descriptions as byte arrays; through the US disc's font resource
 * those codes draw as garbage. port/extract_disc.py pulls the English
 * descriptions out of the disc's executable into MGS/TEXT.DAT (24 items,
 * 2 frozen items, the mine-detector note, 10 weapons, in the order of the
 * game's tables); this replaces the table entries with them at font load.
 *
 * Each string gets its own 256-byte buffer: the game patches the Socom
 * description in place at offset 0x70 to toggle the suppressor note, which
 * for the shorter English text lands harmlessly past its terminator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char* itm_descriptions_8009E3E4[];
extern char* frozenItemsDescriptions_8009E444[];
extern char* mineDetectorUnusable_ptr;
extern char* wpn_descriptions_8009E5CC[];

void Mgs_DataPath(const char* devname, char* out, size_t n);
void Mgs_SpiBusTake(void);
void Mgs_SpiBusGive(void);

#define TEXT_SLOT 256

extern int mgs_font_us; /* source/font/font.c: the US glyph table layout */

static unsigned rd32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

void Mgs_TextOverride(void) {
    static char* slots; /* allocated once per launch: the tables are reset on relaunch */
    char path[160];
    FILE* f;
    long size;
    unsigned char* buf;
    const unsigned char* p;
    const unsigned char* end;
    unsigned ntables, t, used = 0, patched = 0;

    Mgs_DataPath("cdrom:\\MGS\\TEXT.DAT;1", path, sizeof path);
    Mgs_SpiBusTake();
    f = fopen(path, "rb");
    if (!f) {
        Mgs_SpiBusGive();
        printf("[text] no %s: the menus keep the decompilation's Integral text\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = size > 0 && size < 65536 ? malloc((size_t)size) : NULL;
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        Mgs_SpiBusGive();
        free(buf);
        printf("[text] cannot read %s\n", path);
        return;
    }
    fclose(f);
    Mgs_SpiBusGive();
    if (size < 12 || memcmp(buf, "MGST", 4) != 0 || rd32(buf + 4) != 1) {
        free(buf);
        printf("[text] %s: not a TEXT.DAT\n", path);
        return;
    }
    if (!slots) {
        slots = malloc(38 * TEXT_SLOT);
        if (!slots) {
            free(buf);
            return;
        }
    }
    ntables = rd32(buf + 8);
    p = buf + 12;
    end = buf + size;
    for (t = 0; t < ntables && p + 8 <= end; t++) {
        const unsigned ident = rd32(p), count = rd32(p + 4);
        unsigned i;
        p += 8;
        for (i = 0; i < count && p + 2 <= end; i++) {
            const unsigned len = p[0] | (p[1] << 8);
            char* dst;
            p += 2;
            if (p + len > end || len >= TEXT_SLOT - 1 || used >= 38) {
                p = end;
                break;
            }
            dst = slots + used * TEXT_SLOT;
            memset(dst, 0, TEXT_SLOT);
            memcpy(dst, p, len);
            p += len;
            used++;
            if (ident == 0 && i == 0) {
                /* the executable's name: SLUS / SLES carry the English font */
                mgs_font_us = (strncmp(dst, "SLUS", 4) == 0 || strncmp(dst, "SLES", 4) == 0);
                printf("[text] executable %s: %s glyph layout\n", dst,
                       mgs_font_us ? "US" : "Integral");
            } else if (ident == 1 && i < 24) {
                itm_descriptions_8009E3E4[i] = dst;
                patched++;
            } else if (ident == 2 && i < 2) {
                frozenItemsDescriptions_8009E444[i] = dst;
                patched++;
            } else if (ident == 3 && i == 0) {
                mineDetectorUnusable_ptr = dst;
                patched++;
            } else if (ident == 4 && i < 10) {
                wpn_descriptions_8009E5CC[i] = dst;
                patched++;
            }
        }
    }
    free(buf);
    printf("[text] %u menu descriptions from %s\n", patched, path);
}
