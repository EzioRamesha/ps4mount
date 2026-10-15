/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Read speed of a drive, measured on one of its pkgs. Only reads: tested on the host. */
#pragma once

#define SPEED_CHUNK (4U << 20) /* bytes per read */

/* Reads about want bytes of path (size bytes long) in SPEED_CHUNK reads spread evenly over
 * the file, for at most max_seconds, and gives the speed in MB/s (10^6 bytes) and the bytes
 * read. seed moves the reads a little so a repeated test does not read what the system just
 * cached. 0 on success; -1 when the file is smaller than one chunk, cannot be opened or a
 * read fails. */
/* Where read i of n starts in a file of size bytes: a multiple of 4096 (O_DIRECT needs it),
 * and the read ends inside the file when n <= size / SPEED_CHUNK. */
unsigned long long speed_offset(unsigned long long i, unsigned long long size, unsigned long long n, unsigned seed);

int speed_measure(const char *path, unsigned long long size, unsigned long long want, int max_seconds,
                  unsigned seed, double *mbps, unsigned long long *done);
