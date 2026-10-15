/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What keeps PS4Mount from harming the console: database corruption reports in the
 * system log stop new registrations, and titles that keep failing are given up. */
#pragma once

#include <stddef.h>
#include <time.h>

/* 1 when a system log line reports that a system database was flagged as corrupted
 * ("DbCorruptionOccured : 0x80f50080 : /appinfo.db : ..."). db gets the database
 * file when the line names one, "" otherwise. */
int safety_db_corrupt(const char *line, char *db, size_t n);

#define RETRY_SECONDS 300 /* wait after a failed registration */
#define RETRY_MAX_TRIES 3 /* then the title is given up until asked again */

typedef struct {
    char tid[10];
    time_t until;
    int tries;
    char reason[96];
} retry_t;

/* A table of n entries, zeroed at the start. */
typedef struct {
    retry_t *items;
    int n;
} retry_table_t;

/* 1 while tid waits after a failure, or was given up. */
int retry_waiting(const retry_table_t *t, const char *tid, time_t now);
/* 1 when tid failed RETRY_MAX_TRIES times in a row. */
int retry_given_up(const retry_table_t *t, const char *tid);
/* Seconds until tid is tried again after a failure, 0 when none is pending or it was given up. */
long retry_remaining(const retry_table_t *t, const char *tid, time_t now);
/* Records a failure with why it happened; returns how many in a row (0 when the table
 * is full). reason is copied, so the caller's buffer does not need to outlive the call. */
int retry_failed(retry_table_t *t, const char *tid, time_t now, const char *reason);
/* The reason given to the most recent retry_failed() for tid, "" when none is on record
 * (never failed, or retry_clear() was called since). */
const char *retry_reason(const retry_table_t *t, const char *tid);
void retry_clear(retry_table_t *t, const char *tid);
/* Forgets the titles given up, so they are tried again. */
void retry_clear_given_up(retry_table_t *t);
