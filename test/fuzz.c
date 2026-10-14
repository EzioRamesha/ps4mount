/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * libFuzzer harness for the pkg and param.sfo parsers, which read files straight
 * off the user's USB drive. Built and run by CI only (clang -fsanitize=fuzzer):
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -Isrc test/fuzz.c src/pkg.c src/crypto.c -o build/fuzz
 * test/fuzz_seed.c writes a valid pkg and a valid sfo to start the corpus from. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pkg.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* param.sfo: parsed from a buffer as is. */
    static sfo_t sfo;
    if (sfo_parse(data, size, &sfo) == 0)
        sfo_get(&sfo, "TITLE");

    /* pkg: pkg_open reads from a file, so the input goes through one. */
#ifdef __linux__
    int fd = memfd_create("fuzz-pkg", 0);
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
#else
    char path[] = "/tmp/ps4mount-fuzz-XXXXXX";
    int fd = mkstemp(path);
#endif
    if (fd < 0)
        return 0;
    if (write(fd, data, size) == (ssize_t)size) {
        static pkg_t pkg;
        if (pkg_open(&pkg, path) == 0) {
            pkg_fake_key(&pkg);
            for (int i = 0; i < pkg.nentries; i++) {
                size_t len = 0;
                free(pkg_entry(&pkg, pkg.entries[i].id, &len));
            }
            pkg_close(&pkg);
        }
    }
    close(fd);
#ifndef __linux__
    unlink(path);
#endif
    return 0;
}
