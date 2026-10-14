/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Unit tests for the parts that run without a console: crypto, the install file
 * builders and the param.sfo parser. Build and run with: bash test/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build.h"
#include "catalog.h"
#include "crypto.h"
#include "logfile.h"
#include "origin.h"
#include "pkg.h"
#include "pdb_template.h"
#include "policy.h"
#include "safety.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static void hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++)
        sscanf(s + 2 * i, "%2hhx", &out[i]);
}

/* A pkg with only the fields the builders use. */
static void fake_pkg(pkg_t *p, const char *content_id, uint64_t size) {
    memset(p, 0, sizeof(*p));
    snprintf(p->content_id, sizeof(p->content_id), "%s", content_id);
    memcpy(p->title_id, content_id + 7, 9);
    p->pkg_size = size;
    for (int i = 0; i < 32; i++)
        p->digest[i] = (uint8_t)(0xa0 + i);
}

static void test_crypto(void) {
    printf("crypto\n");
    uint8_t h[32], want[32];
    sha256("abc", 3, h);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    CHECK(!memcmp(h, want, 32));
    sha256("", 0, h);
    hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want, 32);
    CHECK(!memcmp(h, want, 32));
    /* the same digest fed in pieces */
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, "a", 1);
    sha256_update(&c, "bc", 2);
    sha256_final(&c, h);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    CHECK(!memcmp(h, want, 32));
    CHECK(crc32c("123456789", 9) == 0xe3069283u);
}

static void test_app_json(void) {
    printf("app.json / ac.json\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTGAME00000000", 1234567);
    buf_t b = {0};
    build_app_json(&b, &p, "/mnt/usb0/ps4/CUSA00001/app.pkg");
    const char *want = "{\"numberOfSplitFiles\":1,\"originalFileSize\":1234567,\"packageDigest\":"
                       "\"A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF\",\"pieces\":[{\"fileOffset\":0,"
                       "\"fileSize\":1234567,\"url\":\"\\/mnt\\/usb0\\/ps4\\/CUSA00001\\/app.pkg\"}]}";
    CHECK(b.len == strlen(want) && !memcmp(b.data, want, b.len));
    buf_free(&b);
}

static void check_pbm(const pkg_t *p, int type) {
    buf_t b = {0};
    build_app_pbm(&b, p, "01.02", type);
    uint64_t blocks = (p->pkg_size + 0xffff) / 0x10000;
    size_t bitmap = (size_t)((blocks + 7) / 8);
    CHECK(b.len == 0x100 + bitmap + 32);
    CHECK(!memcmp(b.data, "pdbm", 4));
    CHECK(!memcmp(b.data + 4, p->title_id, 9));
    CHECK(!memcmp(b.data + 0x0e, "01.02", 5));
    CHECK(le32(b.data + 0x1e) == (uint32_t)type);
    CHECK(le64(b.data + 0x22) == blocks);
    CHECK(!memcmp(b.data + 0x60, p->digest, 32));
    CHECK(le32(b.data + 0xac) == bitmap);
    /* all blocks present; bits past the last block stay clear */
    CHECK(b.data[0x100] == 0xff);
    if (blocks % 8)
        CHECK(b.data[0x100 + bitmap - 1] == (uint8_t)(0xff << (8 - blocks % 8)));
    uint8_t h[32];
    sha256(b.data, b.len - 32, h);
    CHECK(!memcmp(b.data + b.len - 32, h, 32));
    buf_free(&b);
}

static void test_pbm(void) {
    printf("app.pbm / patch.pbm / ac.pbm\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTGAME00000000", 82706432); /* whole blocks */
    check_pbm(&p, PBM_APP);
    check_pbm(&p, PBM_PATCH);
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTADDON0000001", 524288 + 100); /* partial last block */
    check_pbm(&p, PBM_ADDCONT);
    check_pbm(&p, PBM_LICENSE);
}

/* Records of a pdb: u32 version, then (u32 key, u32 len, u32 len, data), CRC32C trailer. */
typedef struct {
    uint32_t key, len;
    const uint8_t *data;
} rec_t;

static int parse_pdb(const uint8_t *d, size_t n, rec_t *out, int max) {
    int c = 0;
    size_t off = 4;
    while (off + 12 <= n - 4 && c < max) {
        out[c].key = le32(d + off);
        out[c].len = le32(d + off + 4);
        out[c].data = d + off + 12;
        off += 12 + out[c].len;
        c++;
    }
    return off == n - 4 ? c : -1;
}

static const rec_t *find(const rec_t *r, int n, uint32_t key) {
    for (int i = 0; i < n; i++)
        if (r[i].key == key)
            return &r[i];
    return NULL;
}

static void test_ac_pdb(void) {
    printf("add-on PlayGo record\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTADDON0000001", 31129600);
    const char *url = "/mnt/usb0/ps4/CUSA00001/addcont/CUSA00001/TESTADDON0000001/ac.pkg";
    buf_t json = {0}, pdb = {0};
    build_app_json(&json, &p, url);
    build_ac_pdb(&pdb, &p, url, &json);
    static rec_t got[256], tmpl[256];
    int n = parse_pdb(pdb.data, pdb.len, got, 256);
    int nt = parse_pdb(k_pdb_addcont_template, sizeof(k_pdb_addcont_template), tmpl, 256);
    CHECK(n > 0 && n == nt);
    for (int i = 0; n > 0 && i < n && i < nt; i++)
        CHECK(got[i].key == tmpl[i].key); /* same records in the same order as the installer */
    CHECK(le32(pdb.data) == le32(k_pdb_addcont_template));
    CHECK(le32(pdb.data + pdb.len - 4) == crc32c(pdb.data, pdb.len - 4));
    const rec_t *r;
    CHECK((r = find(got, n, 0xf4241)) && r->len == 36 && !memcmp(r->data, p.content_id, 36));
    CHECK((r = find(got, n, 0xf42a6)) && r->len == strlen(url) && !memcmp(r->data, url, r->len));
    CHECK((r = find(got, n, 0xf42a7)) && r->len == json.len && !memcmp(r->data, json.data, r->len));
    for (uint32_t k = 0xf4246; k <= 0xf4249; k++)
        CHECK((r = find(got, n, k)) && r->len == 8 && le64(r->data) == p.pkg_size);
    CHECK((r = find(got, n, 0xf431b)) && r->len == 53 && !memcmp(r->data, "0000000000000000-", 17) &&
          !memcmp(r->data + 17, p.content_id, 36));
    buf_free(&json);
    buf_free(&pdb);
}

static void slot(uint8_t *lic, uint8_t v) { memset(lic, v, 0x400); }

static int idx_count(const buf_t *idx) { return (int)le32(idx->data + 4); }

static const uint8_t *idx_label(const buf_t *idx, int i) { return idx->data + 0x20 + i * 0x30; }

static void test_license(void) {
    printf("license files (rif/idx)\n");
    static uint8_t a[0x400], b[0x400], c[0x400];
    slot(a, 0x11);
    slot(b, 0x22);
    slot(c, 0x33);
    const char *game = "UP0000-CUSA00001_00-TESTGAME00000000", *dlc = "UP0000-CUSA00001_00-TESTADDON0000001";

    buf_t r1 = {0}, i1 = {0};
    build_license(&r1, &i1, NULL, 0, NULL, 0, game, a);
    CHECK(r1.len == 0x800 && !memcmp(r1.data, "rifa", 4) && !memcmp(r1.data + 4, game, 19));
    CHECK(!memcmp(r1.data + 0x400, a, 0x400));
    CHECK(i1.len == 0x50 && !memcmp(i1.data, "ridx", 4) && idx_count(&i1) == 1);
    CHECK(!memcmp(i1.data + 9, game, 19) && !memcmp(idx_label(&i1, 0), game + 20, 16));
    CHECK(le64(idx_label(&i1, 0) + 0x10) == 0x400 && le64(idx_label(&i1, 0) + 0x18) == 0x400);

    /* an add-on gets a second slot, the game keeps its own */
    buf_t r2 = {0}, i2 = {0};
    build_license(&r2, &i2, r1.data, r1.len, i1.data, i1.len, dlc, b);
    CHECK(r2.len == 0xc00 && idx_count(&i2) == 2 && i2.len == 0x80);
    CHECK(!memcmp(r2.data + 0x400, a, 0x400) && !memcmp(r2.data + 0x800, b, 0x400));
    CHECK(!memcmp(idx_label(&i2, 1), dlc + 20, 16) && le64(idx_label(&i2, 1) + 0x10) == 0x800);

    /* writing the game again replaces its slot and keeps the add-on */
    buf_t r3 = {0}, i3 = {0};
    build_license(&r3, &i3, r2.data, r2.len, i2.data, i2.len, game, c);
    CHECK(r3.len == 0xc00 && idx_count(&i3) == 2);
    CHECK(!memcmp(r3.data + 0x400, c, 0x400) && !memcmp(r3.data + 0x800, b, 0x400));

    /* files of another content ID prefix, or damaged ones, are replaced */
    buf_t r4 = {0}, i4 = {0};
    build_license(&r4, &i4, r2.data, r2.len, i2.data, i2.len, "UP0000-CUSA00002_00-OTHERGAME0000000", a);
    CHECK(r4.len == 0x800 && idx_count(&i4) == 1);
    buf_t r5 = {0}, i5 = {0};
    build_license(&r5, &i5, r2.data, 0x500, i2.data, i2.len, dlc, b); /* rif shorter than its slots */
    CHECK(r5.len == 0x800 && idx_count(&i5) == 1);

    buf_t *all[] = {&r1, &i1, &r2, &i2, &r3, &i3, &r4, &i4, &r5, &i5};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        buf_free(all[i]);
}

/* A param.sfo with one string and one integer. */
static size_t make_sfo(uint8_t *d) {
    memset(d, 0, 256);
    const char keys[] = "TITLE\0ATTRIBUTE\0";
    uint32_t ko = 20 + 2 * 16, dof = ko + (uint32_t)sizeof(keys);
    memcpy(d, "\0PSF", 4);
    d[4] = 1, d[5] = 1;
    d[8] = (uint8_t)ko, d[12] = (uint8_t)dof, d[16] = 2;
    uint8_t *e = d + 20;
    e[0] = 0, e[2] = 0x04, e[3] = 0x02, e[4] = 6, e[8] = 8, e[12] = 0; /* TITLE: utf8, "Title\0" */
    e += 16;
    e[0] = 6, e[2] = 0x04, e[3] = 0x04, e[4] = 4, e[8] = 4, e[12] = 8; /* ATTRIBUTE: int32 at 8 */
    memcpy(d + ko, keys, sizeof(keys));
    memcpy(d + dof, "Title", 6);
    d[dof + 8] = 0x34, d[dof + 9] = 0x12;
    return dof + 12;
}

static void test_sfo(void) {
    printf("param.sfo\n");
    uint8_t d[256];
    size_t n = make_sfo(d);
    sfo_t s;
    CHECK(sfo_parse(d, n, &s) == 0 && s.count == 2);
    const sfo_item_t *t = sfo_get(&s, "TITLE"), *a = sfo_get(&s, "ATTRIBUTE");
    CHECK(t && !t->is_int && !strcmp(t->sval, "Title"));
    CHECK(a && a->is_int && a->ival == 0x1234);
    CHECK(!sfo_get(&s, "CATEGORY"));
    d[1] = 'X'; /* not an SFO */
    CHECK(sfo_parse(d, n, &s) != 0);

    /* offsets and lengths of a damaged param.sfo must stay inside the buffer */
    n = make_sfo(d);
    d[20 + 12] = 0xf0, d[20 + 13] = 0x0f; /* TITLE value at +0xff0 */
    CHECK(sfo_parse(d, n, &s) != 0);
    n = make_sfo(d);
    d[20 + 4] = 0xff, d[20 + 5] = 0x01; /* TITLE length 511 */
    CHECK(sfo_parse(d, n, &s) != 0);
    n = make_sfo(d);
    d[20 + 16] = 0xf0, d[20 + 17] = 0x0f; /* ATTRIBUTE key at +0xff0 */
    CHECK(sfo_parse(d, n, &s) != 0);
    n = make_sfo(d);
    CHECK(sfo_parse(d, n - 2, &s) != 0); /* ATTRIBUTE int32 cut short */
}

/* A pkg file with one plain entry (id 0x1000) of the given size at 0x1020. */
static int make_pkg(char *path, uint32_t entry_size, const char *data) {
    static uint8_t h[0x1020 + 16];
    memset(h, 0, sizeof(h));
    h[0] = 0x7f, h[1] = 'C', h[2] = 'N', h[3] = 'T';
    h[0x13] = 1;                      /* entry count */
    h[0x1a] = 0x10;                   /* entry table at 0x1000 */
    uint8_t *e = h + 0x1000;
    e[2] = 0x10;                      /* id 0x1000 */
    e[18] = 0x10, e[19] = 0x20;       /* offset 0x1020 */
    e[20] = (uint8_t)(entry_size >> 24), e[21] = (uint8_t)(entry_size >> 16);
    e[22] = (uint8_t)(entry_size >> 8), e[23] = (uint8_t)entry_size;
    memcpy(h + 0x1020, data, strlen(data));
    strcpy(path, "/tmp/ps4mount-unit-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    int rc = write(fd, h, sizeof(h)) == (ssize_t)sizeof(h) ? 0 : -1;
    close(fd);
    return rc;
}

static void test_pkg_entry(void) {
    printf("pkg entries\n");
    char path[64];
    pkg_t *p = malloc(sizeof(*p));
    size_t len = 0;
    CHECK(p && !make_pkg(path, 5, "hello") && !pkg_open(p, path));
    uint8_t *d = pkg_entry(p, 0x1000, &len);
    CHECK(d && len == 5 && !memcmp(d, "hello", 5));
    free(d);
    pkg_close(p);
    unlink(path);

    /* a size near 4 GB used to wrap the padded size to 0 and write far out of bounds */
    CHECK(!make_pkg(path, 0xfffffff8u, "hello") && !pkg_open(p, path));
    CHECK(!pkg_entry(p, 0x1000, &len));
    pkg_close(p);
    unlink(path);
    free(p);
}

static size_t file_text(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(out, 1, cap - 1, f) : 0;
    out[n] = 0;
    if (f)
        fclose(f);
    return n;
}

static void test_log_tail(void) {
    printf("log tail\n");
    char path[] = "/tmp/ps4mount-tail-XXXXXX", out[64];
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const char *text = "one\ntwo\nthree\nfour\n";
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    CHECK(log_tail(path, 2, out, sizeof(out)) == 11 && !strcmp(out, "three\nfour\n"));
    CHECK(log_tail(path, 10, out, sizeof(out)) == (long)strlen(text) && !strcmp(out, text));
    CHECK(log_tail(path, 3, out, 12) == 11 && !strcmp(out, "three\nfour\n")); /* only whole lines fit */
    CHECK(log_tail(path, 0, out, sizeof(out)) == 0 && !out[0]);
    FILE *f = fopen(path, "a");
    if (f) {
        fputs("five, no line break", f);
        fclose(f);
    }
    CHECK(log_tail(path, 1, out, sizeof(out)) == 19 && !strcmp(out, "five, no line break"));
    CHECK(log_tail("/tmp/ps4mount-no-such-file", 5, out, sizeof(out)) == -1 && !out[0]);
    unlink(path);
}

static void test_log_trim(void) {
    printf("log trim\n");
    char path[] = "/tmp/ps4mount-log-XXXXXX", text[256];
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const char log[] = "line one\nline two\nline three\nline four\n";
    CHECK(write(fd, log, sizeof(log) - 1) == (ssize_t)(sizeof(log) - 1));
    close(fd);

    CHECK(log_trim(path, 1000, 20) == 0); /* under the limit: untouched */
    CHECK(file_text(path, text, sizeof(text)) == sizeof(log) - 1);

    /* over the limit: the end is kept, starting at a whole line */
    CHECK(log_trim(path, 30, 20) == 1);
    file_text(path, text, sizeof(text));
    CHECK(!strcmp(text, "line four\n"));
    unlink(path);

    CHECK(log_trim("/tmp/ps4mount-no-such-log", 30, 20) == 0);
}

static void touch(const char *path) {
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

static void test_name_file(void) {
    printf("origin: game name file\n");
    char out[128];
    origin_name_file(out, sizeof(out), "Persona 5");
    CHECK(!strcmp(out, "Persona 5.txt"));
    origin_name_file(out, sizeof(out), "Ratchet: Into the Nexus? <\"Pro\"> a/b\\c|d*");
    CHECK(!strcmp(out, "Ratchet_ Into the Nexus_ __Pro__ a_b_c_d_.txt"));
    origin_name_file(out, sizeof(out), "Ends with dots... ");
    CHECK(!strcmp(out, "Ends with dots.txt"));
    origin_name_file(out, sizeof(out), "");
    CHECK(!strcmp(out, "game.txt"));
    origin_name_file(out, sizeof(out), "...");
    CHECK(!strcmp(out, "game.txt"));
    origin_name_file(out, sizeof(out), "origin");
    CHECK(!strcmp(out, "origin (name).txt")); /* never the origin record */
    char lng[300];
    memset(lng, 'x', sizeof(lng) - 1);
    lng[sizeof(lng) - 1] = 0;
    origin_name_file(out, sizeof(out), lng);
    CHECK(strlen(out) <= 104 && !strcmp(out + strlen(out) - 4, ".txt"));
    memcpy(lng + 99, "\xe3\x83\x9a", 3); /* a 3-byte character across the 100-byte limit */
    origin_name_file(out, sizeof(out), lng);
    CHECK(strlen(out) == 99 + 4 && !strcmp(out + 99, ".txt"));
    origin_name_file(out, sizeof(out), "\xe3\x83\x9a\xe3\x83\xab\xe3\x82\xbd\xe3\x83\x8a"); /* UTF-8 kept */
    CHECK(!strcmp(out, "\xe3\x83\x9a\xe3\x83\xab\xe3\x82\xbd\xe3\x83\x8a.txt"));
}

static void test_origin(void) {
    printf("origin records and restored list\n");
    char root[] = "/tmp/ps4mount-test-XXXXXX", game[128], list[128], out[256], p[256];
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    snprintf(game, sizeof(game), "%s/ps4", root);
    mkdir(game, 0777);
    snprintf(game, sizeof(game), "%s/ps4/CUSA00001", root);
    mkdir(game, 0777);

    CHECK(origin_find(game, "app.pkg", out, sizeof(out)) != 0); /* nothing recorded yet */
    CHECK(origin_add(game, "app.pkg", "games/My Game [v1.00].pkg") == 0);
    CHECK(origin_add(game, "patch.pkg", "Update 1.01.pkg") == 0);
    CHECK(origin_add(game, "addcont/CUSA00001/TESTADDON0000001/ac.pkg", "dlc/Pack.pkg") == 0);
    CHECK(origin_find(game, "app.pkg", out, sizeof(out)) == 0 && !strcmp(out, "games/My Game [v1.00].pkg"));
    CHECK(origin_find(game, "addcont/CUSA00001/TESTADDON0000001/ac.pkg", out, sizeof(out)) == 0 &&
          !strcmp(out, "dlc/Pack.pkg"));
    /* a newer update in the same place: the last record wins, the old one is kept aside */
    CHECK(origin_add(game, "replaced/patch-1.pkg", "Update 1.01.pkg") == 0);
    CHECK(origin_add(game, "patch.pkg", "Update 1.02.pkg") == 0);
    CHECK(origin_find(game, "patch.pkg", out, sizeof(out)) == 0 && !strcmp(out, "Update 1.02.pkg"));
    CHECK(origin_find(game, "replaced/patch-1.pkg", out, sizeof(out)) == 0 && !strcmp(out, "Update 1.01.pkg"));
    CHECK(origin_find(game, "patch", out, sizeof(out)) != 0); /* whole names only */
    CHECK(origin_add(game, "bad\tname", "x.pkg") != 0);

    /* free names in the original folder */
    snprintf(p, sizeof(p), "%s/Game.pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    touch(p);
    snprintf(p, sizeof(p), "%s/Game (1).pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    touch(p);
    snprintf(p, sizeof(p), "%s/Game (2).pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    snprintf(p, sizeof(p), "%s/v1.0/noext", root);
    CHECK(origin_free_path(root, "v1.0/noext", out, sizeof(out)) == 0 && !strcmp(out, p));

    /* restored pkgs are matched by path and size */
    snprintf(list, sizeof(list), "%s/restored.txt", root);
    CHECK(!restored_has(list, "Game.pkg", 100));
    CHECK(restored_add(list, "Game.pkg", 100) == 0);
    CHECK(restored_add(list, "games/Other.pkg", 5000000000ull) == 0);
    CHECK(restored_has(list, "Game.pkg", 100));
    CHECK(restored_has(list, "games/Other.pkg", 5000000000ull));
    CHECK(!restored_has(list, "Game.pkg", 101));    /* changed */
    CHECK(!restored_has(list, "Game2.pkg", 100));   /* renamed */
    CHECK(!restored_has(list, "x/Game.pkg", 100));  /* moved */

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
}

static cat_pkg_t *cat_add(catalog_t *c, const char *path, const char *cat, const char *cid, const char *ver) {
    cat_pkg_t *p = &c->pkgs[c->npkgs++];
    memset(p, 0, sizeof(*p));
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->state = CAT_OK;
    snprintf(p->category, sizeof(p->category), "%s", cat);
    snprintf(p->content_id, sizeof(p->content_id), "%s", cid);
    snprintf(p->title_id, sizeof(p->title_id), "%.9s", cid + 7);
    snprintf(p->app_ver, sizeof(p->app_ver), "%s", ver);
    return p;
}

static const char *choose_105(const char *tid) { return !strcmp(tid, "CUSA00001") ? "01.05" : NULL; }
static const char *choose_none(const char *tid) { return !strcmp(tid, "CUSA00001") ? CAT_NO_UPDATE : NULL; }
static const char *choose_missing(const char *tid) { return !strcmp(tid, "CUSA00001") ? "09.99" : NULL; }

static void test_catalog_group(void) {
    printf("catalog: games, updates and add-ons in any folder\n");
    catalog_t c = {0};
    c.cap = 16;
    c.pkgs = calloc((size_t)c.cap, sizeof(*c.pkgs));
    /* sorted by path, as the walk leaves them */
    cat_add(&c, "/u/Persona/DLC/costume.pkg", "ac", "EP0001-CUSA00001_00-COSTUME000000000", "01.00");
    cat_add(&c, "/u/Persona/DLC/eu-pack.pkg", "ac", "EP0001-CUSA00009_00-EUPACK0000000000", "01.00");
    cat_add(&c, "/u/Persona/Update/v105.pkg", "gp", "EP0001-CUSA00001_00-PERSONA000000000", "01.05");
    cat_add(&c, "/u/Persona/Update/v110.pkg", "gp", "EP0001-CUSA00001_00-PERSONA000000000", "01.10");
    cat_add(&c, "/u/Persona/Update/v108.pkg", "gp", "EP0001-CUSA00001_00-PERSONA000000000", "01.08");
    cat_pkg_t *g = cat_add(&c, "/u/Persona/game.pkg", "gd", "EP0001-CUSA00001_00-PERSONA000000000", "01.00");
    snprintf(g->services[g->nservices++], 20, "EP0001-CUSA00009_00"); /* add-ons of another region */
    cat_add(&c, "/u/Racing/base.pkg", "gd", "EP0001-CUSA00002_00-RACING0000000000", "01.00");
    cat_add(&c, "/u/backup/Persona.pkg", "gd", "EP0001-CUSA00001_00-PERSONA000000000", "01.00");
    cat_add(&c, "/u/loose/other-dlc.pkg", "ac", "EP0001-CUSA00777_00-NOGAME0000000000", "01.00");
    cat_add(&c, "/u/loose/broken.pkg", "gd", "EP0001-CUSA00003_00-BROKEN0000000000", "01.00")->state = CAT_BAD;

    catalog_group(&c, NULL);
    CHECK(c.ngames == 2);
    const cat_game_t *p = catalog_game(&c, "CUSA00001"), *r = catalog_game(&c, "CUSA00002");
    CHECK(p && !strcmp(c.pkgs[p->app].path, "/u/Persona/game.pkg")); /* the backup copy comes later */
    CHECK(p && p->patch >= 0 && !strcmp(c.pkgs[p->patch].app_ver, "01.10"));
    CHECK(p && p->ndlc == 2);
    CHECK(r && r->patch == -1 && r->ndlc == 0);
    CHECK(!catalog_game(&c, "CUSA00003"));
    CHECK(c.dupes == 1 && c.orphans == 1);

    catalog_group(&c, NULL); /* grouping again gives the same result */
    CHECK(c.ngames == 2 && c.dupes == 1 && c.orphans == 1);

    printf("catalog: what each pkg is used for\n");
    CHECK(p && p->nupdates == 3);
    int roles[CAT_ROLE_NOGAME + 1] = {0};
    for (int i = 0; i < c.npkgs; i++)
        roles[c.pkgs[i].role]++;
    CHECK(roles[CAT_ROLE_APP] == 2 && roles[CAT_ROLE_PATCH] == 1 && roles[CAT_ROLE_SPARE] == 2);
    CHECK(roles[CAT_ROLE_DLC] == 2 && roles[CAT_ROLE_DUPE] == 1 && roles[CAT_ROLE_ORPHAN] == 1);
    CHECK(roles[CAT_ROLE_NONE] == 1); /* the unreadable pkg */
    cat_add(&c, "/u/zz/lost-update.pkg", "gp", "EP0001-CUSA00888_00-LOST000000000000", "01.02");
    catalog_group(&c, NULL);
    CHECK(c.pkgs[c.npkgs - 1].role == CAT_ROLE_NOGAME);

    printf("catalog: update chosen by the user\n");
    catalog_group(&c, choose_105);
    p = catalog_game(&c, "CUSA00001");
    CHECK(p && p->patch >= 0 && !strcmp(c.pkgs[p->patch].app_ver, "01.05"));
    CHECK(!strcmp(c.pkgs[p->patch].path, "/u/Persona/Update/v105.pkg"));
    catalog_group(&c, choose_none);
    p = catalog_game(&c, "CUSA00001");
    CHECK(p && p->patch == -1 && p->nupdates == 3);
    catalog_group(&c, choose_missing); /* a version that is not on the drive: the highest */
    p = catalog_game(&c, "CUSA00001");
    CHECK(p && p->patch >= 0 && !strcmp(c.pkgs[p->patch].app_ver, "01.10"));
    catalog_free(&c);
}

static void put_file(const char *root, const char *rel, const char *data) {
    char p[PATH_MAX], d[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    snprintf(d, sizeof(d), "%s", p);
    for (char *s = d + strlen(root) + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(d, 0777);
            *s = '/';
        }
    FILE *f = fopen(p, "wb");
    if (f) {
        fputs(data, f);
        fclose(f);
    }
}

static int skip_named(const char *path, uint64_t size) {
    (void)size;
    return strstr(path, "skip-me") != NULL;
}

static int cat_has(const catalog_t *c, const char *root, const char *rel) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    for (int i = 0; i < c->npkgs; i++)
        if (!strcmp(c->pkgs[i].path, p))
            return 1;
    return 0;
}

static void test_catalog_walk(void) {
    printf("catalog: walk of the drive\n");
    char root[] = "/tmp/ps4mount-drive-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    put_file(root, "root.pkg", "x");
    put_file(root, "Games/RPG/Persona/Update/u.PKG", "x");
    put_file(root, "ps4/CUSA00001/app.pkg", "x");
    put_file(root, "ps4/CUSA00001/replaced/patch-1.pkg", "x");
    put_file(root, ".Trashes/old.pkg", "x");
    put_file(root, "System Volume Information/s.pkg", "x");
    put_file(root, "Games/skip-me.pkg", "x");
    put_file(root, "Games/readme.txt", "x");

    catalog_t c = {0};
    CHECK(catalog_walk(&c, root, skip_named, NULL, 0) == 3);
    CHECK(c.npkgs == 3 && cat_has(&c, root, "root.pkg") && cat_has(&c, root, "Games/RPG/Persona/Update/u.PKG") &&
          cat_has(&c, root, "ps4/CUSA00001/app.pkg"));
    for (int i = 1; i < c.npkgs; i++)
        CHECK(strcmp(c.pkgs[i - 1].path, c.pkgs[i].path) < 0);

    CHECK(catalog_read(&c) == 3); /* not real pkgs */
    CHECK(c.pkgs[0].state == CAT_BAD);
    CHECK(catalog_walk(&c, root, skip_named, NULL, 0) == 0); /* nothing new: nothing to read again */

    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/root.pkg", root);
    unlink(p);
    put_file(root, "Games/RPG/Persona/Update/u.PKG", "changed");
    CHECK(catalog_walk(&c, root, skip_named, NULL, 0) == 1);
    CHECK(c.npkgs == 2 && !cat_has(&c, root, "root.pkg"));
    catalog_free(&c);

    printf("catalog: folders to ignore\n");
    put_file(root, "Backup/old/game.pkg", "x");
    put_file(root, "Games/Backup/kept.pkg", "x"); /* only Backup at the root is ignored */
    static const char *const ignore[] = {"backup", "Games/RPG"};
    catalog_t g = {0};
    CHECK(catalog_walk(&g, root, NULL, ignore, 2) == 3); /* with Games/skip-me.pkg: no skip function */
    CHECK(cat_has(&g, root, "ps4/CUSA00001/app.pkg") && cat_has(&g, root, "Games/Backup/kept.pkg"));
    CHECK(!cat_has(&g, root, "Backup/old/game.pkg") && !cat_has(&g, root, "Games/RPG/Persona/Update/u.PKG"));
    catalog_free(&g);

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
}

/* A pkg header saying the pkg is full bytes long, cut after have bytes. */
static void put_short_pkg(const char *root, const char *rel, uint64_t full, size_t have) {
    static uint8_t h[0x2000];
    memset(h, 0, sizeof(h));
    h[0] = 0x7f, h[1] = 'C', h[2] = 'N', h[3] = 'T';
    for (int i = 0; i < 8; i++)
        h[0x430 + i] = (uint8_t)(full >> (56 - 8 * i));
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(h, 1, have < sizeof(h) ? have : sizeof(h), f);
        fclose(f);
    }
}

static const cat_pkg_t *cat_get(const catalog_t *c, const char *root, const char *rel) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    for (int i = 0; i < c->npkgs; i++)
        if (!strcmp(c->pkgs[i].path, p))
            return &c->pkgs[i];
    return NULL;
}

static void test_catalog_short(void) {
    printf("catalog: pkgs still copying or cut off\n");
    char root[] = "/tmp/ps4mount-short-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    put_short_pkg(root, "cut.pkg", 0x10000, 0x1000);
    put_short_pkg(root, "header-cut.pkg", 0x10000, 0x100); /* before the size field */
    put_short_pkg(root, "magic-cut.pkg", 0x10000, 2);
    put_short_pkg(root, "empty.pkg", 0x10000, 0);
    put_short_pkg(root, "whole.pkg", 0x1000, 0x1000); /* right size, but not a real pkg */
    put_file(root, "tiny.pkg", "x");

    catalog_t c = {0};
    CHECK(catalog_walk(&c, root, NULL, NULL, 0) == 6);
    CHECK(catalog_read(&c) == 6);
    const cat_pkg_t *cut = cat_get(&c, root, "cut.pkg");
    CHECK(cut && cut->state == CAT_SHORT && cut->full_size == 0x10000);
    CHECK(cat_get(&c, root, "header-cut.pkg")->state == CAT_SHORT);
    CHECK(cat_get(&c, root, "magic-cut.pkg")->state == CAT_SHORT);
    CHECK(cat_get(&c, root, "empty.pkg")->state == CAT_SHORT);
    CHECK(cat_get(&c, root, "whole.pkg")->state == CAT_BAD);
    CHECK(cat_get(&c, root, "tiny.pkg")->state == CAT_BAD);
    catalog_group(&c, NULL);
    CHECK(c.ngames == 0 && cat_get(&c, root, "cut.pkg")->role == CAT_ROLE_NONE);

    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s.cache", root);
    CHECK(catalog_save(&c, file) == 0);
    catalog_t l = {0};
    CHECK(catalog_load(&l, file) == 2); /* only whole.pkg and tiny.pkg: incomplete ones are read again */
    catalog_free(&l);
    unlink(file);

    CHECK(catalog_walk(&c, root, NULL, NULL, 0) == 0); /* unchanged: not read on every scan */
    put_short_pkg(root, "cut.pkg", 0x10000, 0x2000);  /* the copy goes on */
    CHECK(catalog_walk(&c, root, NULL, NULL, 0) == 1);
    CHECK(catalog_read(&c) == 1 && cat_get(&c, root, "cut.pkg")->state == CAT_SHORT);
    put_short_pkg(root, "cut.pkg", 0x1000, 0x1000);   /* complete now (and not a real pkg) */
    CHECK(catalog_walk(&c, root, NULL, NULL, 0) == 1);
    CHECK(catalog_read(&c) == 1 && cat_get(&c, root, "cut.pkg")->state == CAT_BAD);
    catalog_free(&c);

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
}

static void test_catalog_cache(void) {
    printf("catalog: saved and loaded again\n");
    catalog_t c = {0};
    c.cap = 8;
    c.pkgs = calloc((size_t)c.cap, sizeof(*c.pkgs));
    cat_pkg_t *g = cat_add(&c, "/u/A Game/game.pkg", "gd", "EP0001-CUSA00001_00-PERSONA000000000", "01.00");
    snprintf(g->services[g->nservices++], 20, "EP0001-CUSA00009_00");
    snprintf(g->services[g->nservices++], 20, "UP0001-CUSA00010_00");
    g->size = 123456789012ULL;
    g->mtime = 1700000000;
    cat_add(&c, "/u/A Game/upd.pkg", "gp", "EP0001-CUSA00001_00-PERSONA000000000", "01.10")->size = 5;
    cat_add(&c, "/u/bad.pkg", "", "EP0001-CUSA00003_00-BROKEN0000000000", "")->state = CAT_BAD;
    cat_add(&c, "/u/new.pkg", "gd", "EP0001-CUSA00004_00-NEW0000000000000", "01.00")->state = CAT_UNREAD;
    cat_add(&c, "/u/tab\there.pkg", "gd", "EP0001-CUSA00005_00-TAB0000000000000", "01.00");

    char path[] = "/tmp/ps4mount-cat-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    CHECK(catalog_save(&c, path) == 0);

    FILE *f = fopen(path, "a");
    CHECK(f != NULL);
    if (f) {
        fputs("garbage line\n2\tx\n", f);
        fclose(f);
    }
    catalog_t d = {0};
    CHECK(catalog_load(&d, path) == 3); /* the unread one and the one with a tab are not kept */
    CHECK(d.npkgs == 3);
    for (int i = 1; i < d.npkgs; i++)
        CHECK(strcmp(d.pkgs[i - 1].path, d.pkgs[i].path) < 0);
    const cat_pkg_t *a = NULL, *bad = NULL;
    for (int i = 0; i < d.npkgs; i++) {
        if (!strcmp(d.pkgs[i].path, "/u/A Game/game.pkg"))
            a = &d.pkgs[i];
        if (!strcmp(d.pkgs[i].path, "/u/bad.pkg"))
            bad = &d.pkgs[i];
    }
    CHECK(a && a->state == CAT_OK && a->size == 123456789012ULL && a->mtime == 1700000000);
    CHECK(a && !strcmp(a->category, "gd") && !strcmp(a->title_id, "CUSA00001") && !strcmp(a->app_ver, "01.00"));
    CHECK(a && !strcmp(a->content_id, "EP0001-CUSA00001_00-PERSONA000000000"));
    CHECK(a && a->nservices == 2 && !strcmp(a->services[1], "UP0001-CUSA00010_00"));
    CHECK(bad && bad->state == CAT_BAD);
    catalog_group(&d, NULL);
    CHECK(d.ngames == 1 && d.games[0].patch >= 0);

    CHECK(catalog_load(&d, "/tmp/ps4mount-no-such-file") == -1);
    catalog_free(&c);
    catalog_free(&d);

    printf("catalog: add-on names in the cache\n");
    catalog_t e = {0};
    e.cap = 4;
    e.pkgs = calloc((size_t)e.cap, sizeof(*e.pkgs));
    snprintf(cat_add(&e, "/u/ac.pkg", "ac", "EP0001-CUSA00001_00-COSTUME000000001", "")->name, 64, "Costume Pack - Été");
    cat_add(&e, "/u/noname.pkg", "ac", "EP0001-CUSA00001_00-COSTUME000000002", "");
    CHECK(catalog_save(&e, path) == 0);
    catalog_t r = {0};
    CHECK(catalog_load(&r, path) == 2);
    CHECK(r.npkgs == 2 && !strcmp(r.pkgs[0].name, "Costume Pack - Été") && r.pkgs[0].state == CAT_OK);
    CHECK(r.npkgs == 2 && r.pkgs[1].name[0] == 0 && r.pkgs[1].state == CAT_OK);
    catalog_free(&e);
    catalog_free(&r);
    /* a cache from before add-on names: add-ons are read again, other pkgs are kept */
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("2\t10\t1\tac\tCUSA00001\tEP0001-CUSA00001_00-COSTUME000000001\t-\t0\t/u/ac.pkg\n", f);
        fputs("2\t10\t1\tgd\tCUSA00001\tEP0001-CUSA00001_00-PERSONA000000000\t01.00\t0\t/u/game.pkg\n", f);
        fclose(f);
    }
    CHECK(catalog_load(&r, path) == 2);
    CHECK(r.npkgs == 2 && !strcmp(r.pkgs[0].path, "/u/ac.pkg") && r.pkgs[0].state == CAT_UNREAD);
    CHECK(r.npkgs == 2 && r.pkgs[1].state == CAT_OK);
    catalog_free(&r);
    unlink(path);
}

static int kv_count(const char *key, const char *value, void *arg) {
    (void)key;
    (void)value;
    (*(int *)arg)++;
    return 0;
}

static void test_policy(void) {
    printf("policy: what to do with a game\n");
    const char *why;
    game_facts_t f = {0};
    CHECK(policy_decide(&f, &why) == POLICY_ADD && !why[0]); /* new game */
    f.registered = f.installed = 1;
    CHECK(policy_decide(&f, &why) == POLICY_KEEP);
    f.installed = 0; /* update or add-ons changed */
    CHECK(policy_decide(&f, &why) == POLICY_ADD);
    f.retry_wait = 1;
    CHECK(policy_decide(&f, &why) == POLICY_SKIP && why[0]);
    f.retry_wait = 0;
    f.halted = 1;
    CHECK(policy_decide(&f, &why) == POLICY_SKIP);
    f.registered = f.installed = 1; /* paused: games already added stay mounted */
    CHECK(policy_decide(&f, &why) == POLICY_KEEP);
    f.halted = 0;
    game_facts_t base = f;
    int *flags[] = {&f.internal, &f.removed, &f.excluded, &f.elsewhere};
    for (int i = 0; i < 4; i++) {
        f = base;
        *flags[i] = 1;
        CHECK(policy_decide(&f, &why) == POLICY_SKIP && why[0]);
    }

    panel_facts_t p = {0};
    CHECK(!strcmp(policy_panel_state(&p), "pending"));
    p.mounted = p.registered = 1;
    CHECK(!strcmp(policy_panel_state(&p), "ready"));
    p.running = 1;
    CHECK(!strcmp(policy_panel_state(&p), "running"));
    p.removed = 1;
    CHECK(!strcmp(policy_panel_state(&p), "removed"));
    p.excluded = 1;
    CHECK(!strcmp(policy_panel_state(&p), "excluded"));
    p.elsewhere = 1;
    CHECK(!strcmp(policy_panel_state(&p), "duplicate"));
    p.internal = 1;
    CHECK(!strcmp(policy_panel_state(&p), "internal"));
    panel_facts_t q = {0};
    q.given_up = 1;
    CHECK(!strcmp(policy_panel_state(&q), "failed"));
    q.mounted = q.registered = 1; /* added after all */
    CHECK(!strcmp(policy_panel_state(&q), "ready"));

    printf("policy: state files\n");
    char path[] = "/tmp/ps4mount-kv-XXXXXX", v[32];
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    CHECK(kv_get(path, "CUSA00001", v, sizeof(v)) == -1);
    CHECK(kv_set(path, "CUSA00001", "1") == 0 && kv_set(path, "CUSA00002", "01.05") == 0);
    CHECK(kv_get(path, "CUSA00001", v, sizeof(v)) == 0 && !strcmp(v, "1"));
    CHECK(kv_set(path, "CUSA00002", "01.10") == 0); /* replaced, not added twice */
    CHECK(kv_get(path, "CUSA00002", v, sizeof(v)) == 0 && !strcmp(v, "01.10"));
    CHECK(kv_set(path, "CUSA00001", NULL) == 0);
    CHECK(kv_get(path, "CUSA00001", v, sizeof(v)) == -1);
    CHECK(kv_get(path, "CUSA0000", v, sizeof(v)) == -1); /* whole keys only */
    CHECK(kv_set(path, "bad\tkey", "1") == -1 && kv_set(path, "k", "bad\nvalue") == -1);
    FILE *f2 = fopen(path, "a");
    if (f2) {
        fputs("CUSA00003\n", f2); /* a key alone */
        fclose(f2);
    }
    CHECK(kv_get(path, "CUSA00003", v, sizeof(v)) == 0 && !strcmp(v, "1"));
    int count = 0;
    CHECK(kv_each(path, kv_count, &count) == 0 && count == 2);
    CHECK(kv_each("/tmp/ps4mount-no-such-file", kv_count, &count) == -1);
    CHECK(kv_set("/tmp/ps4mount-no-dir/x/kv.txt", "a", "1") == -1);
    unlink(path);
}

static void test_safety(void) {
    printf("safety: database corruption in the system log\n");
    char db[64];
    CHECK(safety_db_corrupt("<118>[DBR Message][dbRecoverySetCorruptionFlag:140] DbCorruptionOccured : 0x80f50080 : "
                            "/appinfo.db : detail - 0x80f50005, message - Missing mandatory title : NPXS40074",
                            db, sizeof(db)));
    CHECK(!strcmp(db, "/appinfo.db"));
    CHECK(safety_db_corrupt("[DBR Message][dbRecoveryOpenErrorDialog:48] DbCorruptionOccured : moduleId - 11", db,
                            sizeof(db)));
    CHECK(!db[0]);
    CHECK(safety_db_corrupt("DbCorruptionOccurred : /system_data/priv/mms/app.db:", db, sizeof(db)));
    CHECK(!strcmp(db, "/system_data/priv/mms/app.db"));
    CHECK(!safety_db_corrupt("[DBR Warning][update:63] progress has decreased", db, sizeof(db)));
    CHECK(!safety_db_corrupt("[PlayGoCore] /user/app/CUSA00001 mounted", db, sizeof(db)));

    printf("safety: titles that keep failing\n");
    retry_t items[2] = {0};
    retry_table_t t = {items, 2};
    time_t now = 1000;
    CHECK(!retry_waiting(&t, "CUSA00001", now));
    CHECK(retry_failed(&t, "CUSA00001", now) == 1);
    CHECK(retry_waiting(&t, "CUSA00001", now + RETRY_SECONDS - 1));
    CHECK(!retry_waiting(&t, "CUSA00001", now + RETRY_SECONDS));
    CHECK(retry_failed(&t, "CUSA00001", now) == 2 && !retry_given_up(&t, "CUSA00001"));
    CHECK(retry_failed(&t, "CUSA00001", now) == RETRY_MAX_TRIES && retry_given_up(&t, "CUSA00001"));
    CHECK(retry_waiting(&t, "CUSA00001", now + 100000)); /* given up: not tried again by itself */
    CHECK(retry_failed(&t, "CUSA00002", now) == 1);
    CHECK(retry_failed(&t, "CUSA00003", now) == 0); /* table full */
    retry_clear_given_up(&t);
    CHECK(!retry_given_up(&t, "CUSA00001") && !retry_waiting(&t, "CUSA00001", now + 100000));
    CHECK(retry_waiting(&t, "CUSA00002", now)); /* still waiting: not given up */
    retry_clear(&t, "CUSA00002");
    CHECK(!retry_waiting(&t, "CUSA00002", now));
}

int main(void) {
    test_crypto();
    test_app_json();
    test_pbm();
    test_ac_pdb();
    test_license();
    test_sfo();
    test_pkg_entry();
    test_log_trim();
    test_log_tail();
    test_origin();
    test_name_file();
    test_catalog_group();
    test_catalog_walk();
    test_catalog_short();
    test_catalog_cache();
    test_safety();
    test_policy();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
