/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Log file size limit. */
#include "logfile.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int log_trim(const char *path, off_t max, off_t keep) {
    struct stat st;
    if (stat(path, &st) || st.st_size <= max)
        return 0;
    size_t n = (size_t)(keep < st.st_size ? keep : st.st_size);
    char *buf = malloc(n);
    int in = open(path, O_RDONLY);
    ssize_t got = buf && in >= 0 ? pread(in, buf, n, st.st_size - (off_t)n) : -1;
    if (in >= 0)
        close(in);
    if (got != (ssize_t)n) {
        free(buf);
        return -1;
    }
    char *nl = memchr(buf, '\n', n);
    size_t skip = nl ? (size_t)(nl - buf) + 1 : 0;
    /* written aside and renamed over the log, so a failure never loses it */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666), rc = out < 0;
    if (!rc && write(out, buf + skip, n - skip) != (ssize_t)(n - skip))
        rc = 1;
    if (out >= 0)
        close(out);
    free(buf);
    if (!rc && rename(tmp, path))
        rc = 1;
    if (rc)
        unlink(tmp);
    return rc ? -1 : 1;
}

#define TAIL_READ (64 << 10) /* bytes read from the end of the file */

long log_tail(const char *path, int lines, char *out, size_t n) {
    if (n)
        out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    off_t end = lseek(fd, 0, SEEK_END), start = end > TAIL_READ ? end - TAIL_READ : 0;
    char *b = malloc(TAIL_READ + 1);
    ssize_t len = b && lseek(fd, start, SEEK_SET) == start ? read(fd, b, TAIL_READ) : -1;
    close(fd);
    if (len < 0) {
        free(b);
        return -1;
    }
    /* back from the end to the start of the last lines lines (a final line break ends the last one) */
    ssize_t s = len;
    if (s && b[s - 1] == '\n')
        s--;
    int found = 0;
    while (s > 0 && found < lines) {
        s--;
        if (b[s] == '\n' && ++found == lines)
            s++;
    }
    if (lines <= 0)
        s = len;
    while (n && (size_t)(len - s) > n - 1) { /* drop whole lines until it fits */
        char *nl = memchr(b + s, '\n', (size_t)(len - s));
        s = nl ? nl - b + 1 : len;
    }
    long copied = n ? (long)(len - s) : 0;
    if (n) {
        memcpy(out, b + s, (size_t)copied);
        out[copied] = 0;
    }
    free(b);
    return copied;
}
