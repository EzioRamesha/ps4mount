/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07 */
#include "speed.h"

#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define SPEED_ALIGN 4096ULL /* O_DIRECT reads start on a sector boundary */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

unsigned long long speed_offset(unsigned long long i, unsigned long long size, unsigned long long n, unsigned seed) {
    /* each read at the start of its share of the file, moved by up to a share's slack */
    unsigned long long share = size / n;
    share -= share % SPEED_ALIGN;
    unsigned long long slack = share - SPEED_CHUNK;
    unsigned long long shift = slack ? (unsigned long long)seed * 7919ULL * SPEED_ALIGN % slack : 0;
    shift -= shift % SPEED_ALIGN;
    return i * share + shift;
}

int speed_measure(const char *path, unsigned long long size, unsigned long long want, int max_seconds,
                  unsigned seed, double *mbps, unsigned long long *done) {
    *mbps = 0;
    *done = 0;
    if (size < SPEED_CHUNK || want < SPEED_CHUNK)
        return -1;
    unsigned long long n = want / SPEED_CHUNK, room = size / SPEED_CHUNK;
    if (n > room)
        n = room;
    int fd = -1;
#ifdef O_DIRECT
    fd = open(path, O_RDONLY | O_DIRECT); /* past the system cache where the filesystem allows it */
#endif
    if (fd < 0)
        fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    void *buf = NULL;
    if (posix_memalign(&buf, SPEED_ALIGN, SPEED_CHUNK)) {
        close(fd);
        return -1;
    }
    int rc = 0;
    double start = now_seconds(), elapsed = 0;
    for (unsigned long long i = 0; i < n; i++) {
        ssize_t r = pread(fd, buf, SPEED_CHUNK, (off_t)speed_offset(i, size, n, seed));
        if (r != (ssize_t)SPEED_CHUNK) {
            rc = -1;
            break;
        }
        *done += SPEED_CHUNK;
        elapsed = now_seconds() - start;
        if (elapsed >= max_seconds)
            break;
    }
    free(buf);
    close(fd);
    if (rc)
        return -1;
    if (elapsed < 1e-3)
        elapsed = 1e-3;
    *mbps = *done / 1e6 / elapsed;
    return 0;
}
