/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Writes a valid pkg and a valid param.sfo (the layouts of test/unit.c's
 * make_pkg and make_sfo) into the corpus folder given as the only argument,
 * so the fuzzer (test/fuzz.c) does not have to find the magic bytes itself. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int save(const char *dir, const char *name, const uint8_t *d, size_t n) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(d, 1, n, f) != n) {
        fprintf(stderr, "cannot write %s\n", path);
        return -1;
    }
    return fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fuzz_seed <corpus dir>\n");
        return 1;
    }

    /* A pkg with one plain entry (id 0x1000) of 5 bytes at 0x1020. */
    static uint8_t p[0x1020 + 16];
    p[0] = 0x7f, p[1] = 'C', p[2] = 'N', p[3] = 'T';
    p[0x13] = 1;                /* entry count */
    p[0x1a] = 0x10;             /* entry table at 0x1000 */
    uint8_t *e = p + 0x1000;
    e[2] = 0x10;                /* id 0x1000 */
    e[18] = 0x10, e[19] = 0x20; /* offset 0x1020 */
    e[23] = 5;                  /* size 5 */
    memcpy(p + 0x1020, "hello", 5);

    /* A param.sfo with TITLE ("Title") and ATTRIBUTE (int32 0x1234). */
    static uint8_t s[256];
    const char keys[] = "TITLE\0ATTRIBUTE\0";
    uint32_t ko = 20 + 2 * 16, dof = ko + (uint32_t)sizeof(keys);
    memcpy(s, "\0PSF", 4);
    s[4] = 1, s[5] = 1;
    s[8] = (uint8_t)ko, s[12] = (uint8_t)dof, s[16] = 2;
    uint8_t *i = s + 20;
    i[0] = 0, i[2] = 0x04, i[3] = 0x02, i[4] = 6, i[8] = 8, i[12] = 0;  /* TITLE: utf8, "Title\0" */
    i += 16;
    i[0] = 6, i[2] = 0x04, i[3] = 0x04, i[4] = 4, i[8] = 4, i[12] = 8; /* ATTRIBUTE: int32 at 8 */
    memcpy(s + ko, keys, sizeof(keys));
    memcpy(s + dof, "Title", 6);
    s[dof + 8] = 0x34, s[dof + 9] = 0x12;

    return save(argv[1], "pkg", p, sizeof(p)) || save(argv[1], "sfo", s, dof + 12) ? 1 : 0;
}
