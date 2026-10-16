/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Background copy/move engine for the panel's file manager.
 *
 * Locking discipline: g_task (an fmgr_status_t) is the only state shared between the main
 * loop and the worker thread, and every access to it, from either side, holds g_lock. The
 * worker's own bookkeeping (g_job, g_ok/g_skip/g_fail/g_last_note, g_tick_time/g_tick_bytes)
 * is written by fmgr_start before the thread is created and from then on touched only by the
 * worker thread itself, so it needs no lock, exactly like g_install_tids/g_install_n in
 * main.c. g_cancel is a plain volatile int: fmgr_cancel() is its only writer, the worker its
 * only reader.
 *
 * Cancel/cleanup semantics: the worker checks g_cancel at the top of every file and every
 * 4 MB block inside a big file. A cancelled or failed single-file copy removes its ".part"
 * temp file in the destination folder; a partly-filled destination FOLDER is left alone
 * (it may have existed, with its own files, before this task started). A cross-drive move
 * never deletes a source file until that file's copy has been fsynced and renamed into place
 * at the destination, so a cancelled cross-drive move leaves every not-yet-copied source
 * untouched. A move never removes a source folder except with rmdir(), which only succeeds
 * once every child has actually been moved out of it -- that single property is what makes
 * "leave a partly-moved folder alone" correct without any extra bookkeeping. */
#include "fmgr.h"

#include "fm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FMGR_JOIN_MAX 1536 /* room for a 1023-byte input path plus nested child names */
#define FMGR_MAX_DEPTH 32  /* guards against pathological nesting (and runaway recursion):
                            * each recursion frame holds two FMGR_JOIN_MAX (1536 B) buffers,
                            * so 32 deep is already a worst case of ~100 KB of thread stack,
                            * which no real game-drive tree ever gets close to nesting */
#define FMGR_CHUNK (4u << 20) /* 4 MB: the brief's cancel-check and progress granularity */

enum { PROC_OK = 0, PROC_SKIP = 1, PROC_FAIL = 2, PROC_CANCEL = 3 };
enum { RESOLVE_PROCEED = 0, RESOLVE_SKIP = 1, RESOLVE_MERGE = 2, RESOLVE_ERROR = -1 };
enum { COPY_OK = 0, COPY_FAIL = -1, COPY_CANCEL = -2 };

/* The one task the daemon runs at a time, and the lock that guards every access to it. */
static fmgr_status_t g_task;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* Written only by fmgr_cancel(), read only by the worker thread. */
static volatile int g_cancel;

/* The destination drive of the task that most recently reached a terminal state, not yet
 * taken by fmgr_take_finished_drive(); -1 when there is nothing to report. Guarded by g_lock,
 * same as g_task: the worker sets it right after setting g_task.state to a terminal value (see
 * fmgr_worker below), and fmgr_take_finished_drive() clears it when read, so each finished task
 * is reported at most once. */
static int g_finished_drive = -1;

/* The running task's inputs: written by fmgr_start() before the worker thread is created,
 * read-only for the worker from then on (fmgr_start refuses a new task while one is already
 * running, so there is never a second writer). No lock needed, same as g_install_tids. */
typedef struct {
    char srcs[FMGR_MAX_SRCS][1024];
    int nsrcs;
    char dst[1024];
    int copy;
    int conflict;
    int dst_drive;
} fmgr_job_t;
static fmgr_job_t g_job;

/* Worker-only bookkeeping: touched only by the worker thread, never by the main loop. */
static int g_ok, g_skip, g_fail;
static char g_last_note[256];
static double g_tick_time;
static unsigned long long g_tick_bytes;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Joins dir and name into out. Returns -1 (and leaves out unusable) when it would not fit,
 * so callers fail closed instead of operating on a silently truncated path. */
static int join_path(const char *dir, const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Recursively counts regular files and their total bytes under path (lstat-based: never
 * follows a symlink, and skips anything that is neither a file nor a folder, matching what
 * the worker will do when it actually gets there). A path that cannot be read contributes
 * nothing -- the worker reports the same problem, and skips it, when it processes it for
 * real. depth guards against pathological nesting. */
static void walk_count(const char *path, int depth, unsigned long long *items, unsigned long long *bytes) {
    if (depth > FMGR_MAX_DEPTH)
        return;
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISREG(st.st_mode)) {
        (*items)++;
        *bytes += (unsigned long long)st.st_size;
        return;
    }
    if (!S_ISDIR(st.st_mode))
        return;
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char child[FMGR_JOIN_MAX];
        if (join_path(path, de->d_name, child, sizeof(child)) == 0)
            walk_count(child, depth + 1, items, bytes);
    }
    closedir(d);
}

/* Tries "name (2)", "name (3)", ... under dst_dir until one names neither a file nor a
 * folder there, and writes the resulting full path to out. */
static int resolve_keep_both(const char *dst_dir, const char *name, char *out, size_t cap, char *note,
                             size_t ncap) {
    for (int attempt = 2; attempt <= 1000; attempt++) {
        char newname[FM_NAME_MAX];
        if (fm_keep_both_name(name, attempt, newname, sizeof(newname)) != 0) {
            snprintf(note, ncap, "'%s': no free name fits, skipped", name);
            return RESOLVE_SKIP;
        }
        char cand[FMGR_JOIN_MAX];
        if (join_path(dst_dir, newname, cand, sizeof(cand)) != 0) {
            snprintf(note, ncap, "'%s': path too long, skipped", name);
            return RESOLVE_ERROR;
        }
        struct stat st;
        if (lstat(cand, &st) != 0) {
            snprintf(out, cap, "%s", cand);
            return RESOLVE_PROCEED;
        }
    }
    snprintf(note, ncap, "'%s': too many name collisions, skipped", name);
    return RESOLVE_SKIP;
}

/* Decides what to do about placing an item named "name" directly under dst_dir, given
 * whether it is a folder (placing_dir) and the task's conflict policy. Writes the resolved
 * destination path to out on RESOLVE_PROCEED/RESOLVE_MERGE, or a short reason to note on
 * RESOLVE_SKIP/RESOLVE_ERROR. FMGR_ASK should never reach here (fmgr_start refuses it), but
 * is treated as SKIP, noted, if it somehow does. */
static int resolve_clash(const char *dst_dir, const char *name, int placing_dir, int conflict, char *out,
                         size_t cap, char *note, size_t ncap) {
    char cand[FMGR_JOIN_MAX];
    if (join_path(dst_dir, name, cand, sizeof(cand)) != 0) {
        snprintf(note, ncap, "'%s': path too long, skipped", name);
        return RESOLVE_ERROR;
    }
    struct stat st;
    int exists = (lstat(cand, &st) == 0);
    if (!exists) {
        snprintf(out, cap, "%s", cand);
        return RESOLVE_PROCEED;
    }

    int effective = conflict;
    int asked_but_unresolved = (conflict == FMGR_ASK);
    if (asked_but_unresolved)
        effective = FMGR_SKIP;

    int same_type = (placing_dir && S_ISDIR(st.st_mode)) || (!placing_dir && S_ISREG(st.st_mode));
    if (!same_type) {
        /* a file where a folder is, or the reverse: there is no safe "overwrite" across
         * types (it would mean deleting a whole folder, or a file, sight unseen), so only
         * SKIP and KEEP_BOTH are honoured; OVERWRITE also falls back to SKIP here. */
        if (effective == FMGR_KEEP_BOTH)
            return resolve_keep_both(dst_dir, name, out, cap, note, ncap);
        snprintf(note, ncap, asked_but_unresolved ? "'%s' needed a conflict choice, skipped"
                                                   : "'%s' exists as a different kind of item, skipped",
                 name);
        return RESOLVE_SKIP;
    }

    switch (effective) {
    case FMGR_OVERWRITE:
        snprintf(out, cap, "%s", cand);
        /* a folder onto a folder: merge into the existing one (its own contents keep their
         * own conflict resolution as we recurse); a file onto a file: rename() over an
         * existing file replaces it atomically, so RESOLVE_PROCEED already does the job. */
        return placing_dir ? RESOLVE_MERGE : RESOLVE_PROCEED;
    case FMGR_KEEP_BOTH:
        return resolve_keep_both(dst_dir, name, out, cap, note, ncap);
    case FMGR_SKIP:
    default:
        snprintf(note, ncap, asked_but_unresolved ? "'%s' needed a conflict choice, skipped"
                                                   : "'%s' already exists, skipped",
                 name);
        return RESOLVE_SKIP;
    }
}

/* Records the outcome of one item (a file, or a whole folder moved in one rename()) and
 * updates the shared task's progress. class is PROC_OK/PROC_SKIP/PROC_FAIL; note may be NULL
 * or empty. g_ok/g_skip/g_fail/g_last_note are worker-only: no lock needed for them. */
static void finish_item(const char *name, int class, const char *note) {
    if (class == PROC_SKIP) {
        g_skip++;
        if (note && *note)
            snprintf(g_last_note, sizeof(g_last_note), "%s", note);
    } else if (class == PROC_FAIL) {
        g_fail++;
        if (note && *note)
            snprintf(g_last_note, sizeof(g_last_note), "%s", note);
    } else {
        g_ok++;
    }
    pthread_mutex_lock(&g_lock);
    g_task.done_items++;
    snprintf(g_task.what, sizeof(g_task.what), "%s", name);
    pthread_mutex_unlock(&g_lock);
}

/* Adds n just-written bytes to the task's progress. done_bytes is updated under the lock
 * every call (cheap at 4 MB granularity); bytes_per_sec/eta_seconds are only recomputed when
 * at least a second has passed, per the brief ("taking the mutex per 4 MB block is fine,
 * recomputing ETA per block is wasteful"). g_tick_time/g_tick_bytes are worker-only. */
static void progress_add_bytes(unsigned long long n) {
    pthread_mutex_lock(&g_lock);
    g_task.done_bytes += n;
    double now = now_seconds();
    double elapsed = now - g_tick_time;
    if (elapsed >= 1.0) {
        unsigned long long delta = g_task.done_bytes - g_tick_bytes;
        g_task.bytes_per_sec = (unsigned long long)((double)delta / elapsed);
        unsigned long long remaining =
            g_task.total_bytes > g_task.done_bytes ? g_task.total_bytes - g_task.done_bytes : 0;
        g_task.eta_seconds = g_task.bytes_per_sec ? (long)(remaining / g_task.bytes_per_sec) : 0;
        g_tick_time = now;
        g_tick_bytes = g_task.done_bytes;
    }
    pthread_mutex_unlock(&g_lock);
}

/* Builds "<dir>/.<base>.part" for dst_path "<dir>/<base>": the temp name a copy is written
 * to before it is fsynced and renamed into place. */
static int build_temp_path(const char *dst_path, char *out, size_t cap) {
    const char *slash = strrchr(dst_path, '/');
    if (!slash)
        return -1;
    size_t dirlen = (size_t)(slash - dst_path);
    char dir[FMGR_JOIN_MAX];
    if (dirlen >= sizeof(dir))
        return -1;
    memcpy(dir, dst_path, dirlen);
    dir[dirlen] = 0;
    int n = snprintf(out, cap, "%s/.%s.part", dir, slash + 1);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Copies src to dst_path through a ".part" temp file in dst_path's folder, fsyncs it, then
 * renames it into place. Checks g_cancel at the top and after every up-to-4MB block. On
 * cancel or any failure the temp file is removed and dst_path is left untouched (it either
 * never existed or still holds whatever was there before). */
static int copy_file(const char *src, const char *dst_path, const char *name, char *msg, size_t mcap) {
    char tmp[FMGR_JOIN_MAX];
    if (build_temp_path(dst_path, tmp, sizeof(tmp)) != 0) {
        snprintf(msg, mcap, "'%s': destination path too long", name);
        return COPY_FAIL;
    }
    if (g_cancel)
        return COPY_CANCEL;
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        snprintf(msg, mcap, "'%s': could not open the source: %s", name, strerror(errno));
        return COPY_FAIL;
    }
    int dfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) {
        snprintf(msg, mcap, "'%s': could not create the destination: %s", name, strerror(errno));
        close(sfd);
        return COPY_FAIL;
    }

    /* Static, not a stack buffer: one fmgr task runs at a time daemon-wide (fmgr_busy()
     * refuses a second), so only one thread is ever inside copy_file, and keeping 4 MB off
     * the worker thread's stack avoids relying on a large thread stack being available. */
    static unsigned char buf[FMGR_CHUNK];
    int result = COPY_OK;
    for (;;) {
        if (g_cancel) {
            result = COPY_CANCEL;
            break;
        }
        ssize_t r = read(sfd, buf, sizeof(buf));
        if (r < 0) {
            snprintf(msg, mcap, "'%s': read error: %s", name, strerror(errno));
            result = COPY_FAIL;
            break;
        }
        if (r == 0)
            break; /* end of file */
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(dfd, buf + off, (size_t)(r - off));
            if (w < 0) {
                snprintf(msg, mcap, "'%s': write error: %s", name, strerror(errno));
                result = COPY_FAIL;
                break;
            }
            off += w;
        }
        if (result != COPY_OK)
            break;
        progress_add_bytes((unsigned long long)r);
    }
    close(sfd);
    if (result != COPY_OK) {
        close(dfd);
        unlink(tmp);
        return result;
    }
    if (fsync(dfd) != 0) {
        snprintf(msg, mcap, "'%s': could not flush the copy: %s", name, strerror(errno));
        close(dfd);
        unlink(tmp);
        return COPY_FAIL;
    }
    close(dfd);
    if (rename(tmp, dst_path) != 0) {
        snprintf(msg, mcap, "'%s': could not finish the copy: %s", name, strerror(errno));
        unlink(tmp);
        return COPY_FAIL;
    }
    return COPY_OK;
}

typedef struct {
    int copy;     /* 1 copy, 0 move */
    int conflict; /* FMGR_* policy */
} plan_ctx_t;

/* Recursively places src (a file or a folder) as dst_dir/name. same_drive_move is 1 only for
 * a MOVE whose whole top-level source lives on the destination's drive: in that case every
 * file placed is moved with rename() (instant); otherwise (a copy, or a cross-drive move)
 * files are copied through copy_file() and, for a move, the source file is only unlinked
 * after its copy has been renamed into place. depth guards pathological nesting. Returns
 * PROC_OK/PROC_SKIP/PROC_FAIL/PROC_CANCEL; note gets a short reason on SKIP/FAIL. Every item
 * it finishes (a file, or a whole folder moved in one rename()) is recorded with
 * finish_item(), which is what drives done_items/what for the panel. */
static int process_entry(const plan_ctx_t *ctx, int same_drive_move, const char *src, const char *dst_dir,
                         const char *name, int depth, char *note, size_t ncap) {
    if (g_cancel)
        return PROC_CANCEL;
    if (depth > FMGR_MAX_DEPTH) {
        snprintf(note, ncap, "'%s' is nested too deep, skipped", name);
        finish_item(name, PROC_SKIP, note);
        return PROC_SKIP;
    }

    struct stat sst;
    if (lstat(src, &sst) != 0) {
        snprintf(note, ncap, "'%s' is gone, skipped", name);
        finish_item(name, PROC_SKIP, note);
        return PROC_SKIP;
    }
    int is_dir = S_ISDIR(sst.st_mode);
    if (!is_dir && !S_ISREG(sst.st_mode)) {
        /* exFAT has no symlinks, and lstat (not stat) means we never followed one anyway;
         * this only catches something stranger, which we leave alone entirely. */
        snprintf(note, ncap, "'%s' is not a file or folder, skipped", name);
        finish_item(name, PROC_SKIP, note);
        return PROC_SKIP;
    }

    char dst_path[FMGR_JOIN_MAX];
    int rc = resolve_clash(dst_dir, name, is_dir, ctx->conflict, dst_path, sizeof(dst_path), note, ncap);
    if (rc == RESOLVE_SKIP) {
        finish_item(name, PROC_SKIP, note);
        return PROC_SKIP;
    }
    if (rc == RESOLVE_ERROR) {
        finish_item(name, PROC_FAIL, note);
        return PROC_FAIL;
    }
    int merge = (rc == RESOLVE_MERGE);

    if (is_dir) {
        if (same_drive_move && !merge) {
            /* The whole folder moves in one rename(): instant, and the only correct way to
             * move a folder on the same drive (copy-then-delete would needlessly double the
             * space used and the time taken). */
            if (rename(src, dst_path) != 0) {
                snprintf(note, ncap, "could not move '%s': %s", name, strerror(errno));
                finish_item(name, PROC_FAIL, note);
                return PROC_FAIL;
            }
            finish_item(name, PROC_OK, NULL);
            return PROC_OK;
        }

        if (!merge && mkdir(dst_path, 0755) != 0 && errno != EEXIST) {
            snprintf(note, ncap, "could not create '%s': %s", name, strerror(errno));
            finish_item(name, PROC_FAIL, note);
            return PROC_FAIL;
        }
        DIR *d = opendir(src);
        if (!d) {
            snprintf(note, ncap, "could not read '%s': %s", name, strerror(errno));
            finish_item(name, PROC_FAIL, note);
            return PROC_FAIL;
        }
        int cancelled = 0;
        struct dirent *de;
        while (!cancelled && (de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            char child_src[FMGR_JOIN_MAX];
            if (join_path(src, de->d_name, child_src, sizeof(child_src)) != 0) {
                /* Name too long to even address: leave it exactly where it is and note it,
                 * rather than guess at a truncated path. */
                char skip_note[256];
                snprintf(skip_note, sizeof(skip_note), "'%s/%s': path too long, skipped", name, de->d_name);
                finish_item(de->d_name, PROC_SKIP, skip_note);
                continue;
            }
            char child_note[256];
            child_note[0] = 0;
            int r = process_entry(ctx, same_drive_move, child_src, dst_path, de->d_name, depth + 1, child_note,
                                  sizeof(child_note));
            if (r == PROC_CANCEL)
                cancelled = 1;
            /* PROC_OK/PROC_SKIP/PROC_FAIL were already recorded by the recursive call's own
             * finish_item(); nothing left to do with r here besides noticing a cancel. */
        }
        closedir(d);
        if (cancelled)
            return PROC_CANCEL;
        if (!ctx->copy) {
            /* A move: try to remove the now-should-be-empty source folder. rmdir() only
             * succeeds once every child really is gone, so a folder that still holds a
             * skipped or failed child (or anything cancel left behind) is correctly left
             * alone -- no extra bookkeeping needed to get that right. */
            rmdir(src);
        }
        /* The folder itself is a container, not a counted item (planning does not count it
         * either): its children already called finish_item for themselves. */
        return PROC_OK;
    }

    /* A regular file. */
    if (same_drive_move) {
        if (rename(src, dst_path) != 0) {
            snprintf(note, ncap, "could not move '%s': %s", name, strerror(errno));
            finish_item(name, PROC_FAIL, note);
            return PROC_FAIL;
        }
        finish_item(name, PROC_OK, NULL);
        return PROC_OK;
    }

    char copy_note[256];
    copy_note[0] = 0;
    int cr = copy_file(src, dst_path, name, copy_note, sizeof(copy_note));
    if (cr == COPY_CANCEL)
        return PROC_CANCEL;
    if (cr != COPY_OK) {
        finish_item(name, PROC_FAIL, copy_note);
        return PROC_FAIL;
    }
    if (!ctx->copy) {
        /* Cross-drive move: the copy is fsynced and renamed into place, so it is now safe to
         * remove the original. If the unlink itself fails, the data is still safe (it now
         * exists at the destination) but the move did not fully complete, so this is a soft
         * failure, not a lost-data one. */
        if (unlink(src) != 0) {
            snprintf(note, ncap, "copied '%s' but could not remove the original: %s", name, strerror(errno));
            finish_item(name, PROC_FAIL, note);
            return PROC_FAIL;
        }
    }
    finish_item(name, PROC_OK, NULL);
    return PROC_OK;
}

static void *fmgr_worker(void *arg) {
    (void)arg;
    g_ok = g_skip = g_fail = 0;
    g_last_note[0] = 0;
    g_tick_time = now_seconds();
    g_tick_bytes = 0;

    plan_ctx_t ctx = {.copy = g_job.copy, .conflict = g_job.conflict};
    int cancelled = 0;

    for (int i = 0; i < g_job.nsrcs; i++) {
        if (g_cancel) {
            cancelled = 1;
            break;
        }
        const char *src = g_job.srcs[i];
        const char *name = base_name(src);
        int same_drive_move = !g_job.copy && fm_drive_of(src) == g_job.dst_drive;
        char note[256];
        note[0] = 0;
        int r = process_entry(&ctx, same_drive_move, src, g_job.dst, name, 0, note, sizeof(note));
        if (r == PROC_CANCEL) {
            cancelled = 1;
            break;
        }
        /* PROC_OK/PROC_SKIP/PROC_FAIL are already accounted for inside process_entry. */
    }

    pthread_mutex_lock(&g_lock);
    if (cancelled) {
        g_task.state = FMGR_CANCELLED;
        snprintf(g_task.msg, sizeof(g_task.msg), "Cancelled: %d done, %d skipped, %d failed", g_ok, g_skip,
                 g_fail);
    } else if (g_fail > 0) {
        g_task.state = FMGR_FAILED;
        snprintf(g_task.msg, sizeof(g_task.msg), "%d done, %d skipped, %d failed: %s", g_ok, g_skip, g_fail,
                 g_last_note[0] ? g_last_note : "see the log");
    } else {
        g_task.state = FMGR_DONE;
        const char *verb = g_job.copy ? "Copied" : "Moved";
        if (g_skip > 0)
            snprintf(g_task.msg, sizeof(g_task.msg), "%s %d item%s (%d skipped)", verb, g_ok, g_ok == 1 ? "" : "s",
                     g_skip);
        else
            snprintf(g_task.msg, sizeof(g_task.msg), "%s %d item%s", verb, g_ok, g_ok == 1 ? "" : "s");
    }
    /* Record that a task just reached a terminal state, for the main loop to pick up on its
     * own (see fmgr_take_finished_drive()) instead of relying on a client's HTTP poll to notice
     * the same transition in app_fm_task_json(). g_job.dst_drive is set by fmgr_start() before
     * this thread was created and never written again, so it is safe to read here. */
    g_finished_drive = g_job.dst_drive;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int fmgr_busy(void) {
    pthread_mutex_lock(&g_lock);
    int busy = (g_task.state == FMGR_RUNNING);
    pthread_mutex_unlock(&g_lock);
    return busy;
}

int fmgr_take_finished_drive(void) {
    pthread_mutex_lock(&g_lock);
    int drive = g_finished_drive;
    g_finished_drive = -1;
    pthread_mutex_unlock(&g_lock);
    return drive;
}

void fmgr_status(fmgr_status_t *out) {
    pthread_mutex_lock(&g_lock);
    *out = g_task;
    pthread_mutex_unlock(&g_lock);
}

void fmgr_cancel(void) {
    g_cancel = 1;
}

int fmgr_start(char (*srcs)[1024], int n, const char *dst, int copy, int conflict, unsigned long long dst_free,
               char *msg, size_t cap) {
    if (fmgr_busy()) {
        snprintf(msg, cap, "A copy or move is already running");
        return -1;
    }
    if (n <= 0) {
        snprintf(msg, cap, "Nothing selected");
        return -1;
    }
    if (n > FMGR_MAX_SRCS) {
        snprintf(msg, cap, "Too many items selected (limit %d)", FMGR_MAX_SRCS);
        return -1;
    }
    if (conflict == FMGR_ASK) {
        /* The panel is expected to resolve ASK before calling fmgr_start (see fm.h/Task 8);
         * refusing here keeps that contract explicit instead of silently guessing for the
         * whole task. resolve_clash() still defends against FMGR_ASK reaching the worker. */
        snprintf(msg, cap, "No conflict choice was made");
        return -1;
    }
    if (conflict != FMGR_OVERWRITE && conflict != FMGR_SKIP && conflict != FMGR_KEEP_BOTH) {
        snprintf(msg, cap, "Unknown conflict choice");
        return -1;
    }

    if (!fm_path_ok(dst)) {
        snprintf(msg, cap, "Invalid destination");
        return -1;
    }
    struct stat dst_st;
    if (stat(dst, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode)) {
        snprintf(msg, cap, "The destination is not a folder");
        return -1;
    }
    int dst_drive = fm_drive_of(dst);
    if (dst_drive < 0) {
        snprintf(msg, cap, "The destination is not on a drive");
        return -1;
    }

    unsigned long long total_items = 0, total_bytes = 0, needed_space = 0;
    for (int i = 0; i < n; i++) {
        if (!fm_path_ok(srcs[i])) {
            snprintf(msg, cap, "Invalid source path");
            return -1;
        }
        if (fm_is_drive_root(srcs[i])) {
            snprintf(msg, cap, "Cannot %s a drive itself", copy ? "copy" : "move");
            return -1;
        }
        if (fm_covers(srcs[i], dst)) {
            snprintf(msg, cap, "Cannot %s a folder into itself", copy ? "copy" : "move");
            return -1;
        }
        struct stat sst;
        if (lstat(srcs[i], &sst) != 0) {
            snprintf(msg, cap, "A selected item no longer exists");
            return -1;
        }
        if (!S_ISDIR(sst.st_mode) && !S_ISREG(sst.st_mode)) {
            snprintf(msg, cap, "A selected item is not a file or folder");
            return -1;
        }
        /* resolve_clash decides "free or occupied" with a plain lstat(), which cannot tell
         * a pre-existing destination entry from one this same task already placed there a
         * moment ago. Two sources sharing a basename would otherwise let the second one
         * silently overwrite (or rename over) what the first just placed -- losing the
         * first source's only copy on OVERWRITE, and even on a same-drive move's rename().
         * Refuse the whole request up front instead: O(n^2) with n <= FMGR_MAX_SRCS is free,
         * and this fails closed before a single byte moves. */
        for (int j = 0; j < i; j++) {
            if (!strcmp(base_name(srcs[i]), base_name(srcs[j]))) {
                snprintf(msg, cap, "Two selected items are both named '%s'", base_name(srcs[i]));
                return -1;
            }
        }
        if (S_ISDIR(sst.st_mode)) {
            /* A folder landing back in the folder that already contains it would have to
             * merge into itself: under FMGR_OVERWRITE that means creating ".part" files
             * inside the very directory being readdir'd, which POSIX leaves unspecified.
             * fm_covers(src, dst) above only catches dst equal to or inside src; this is the
             * other direction (dst is src's immediate parent). */
            const char *slash = strrchr(srcs[i], '/');
            if (slash) {
                size_t parent_len = (size_t)(slash - srcs[i]);
                size_t dst_len = strlen(dst);
                if (dst_len > 0 && dst[dst_len - 1] == '/')
                    dst_len--; /* tolerate a drive-root dst spelled with a trailing slash */
                if (parent_len > 0 && dst_len == parent_len && strncmp(dst, srcs[i], parent_len) == 0) {
                    snprintf(msg, cap, "Cannot %s a folder into the folder that already contains it",
                             copy ? "copy" : "move");
                    return -1;
                }
            }
        }

        int same_drive = !copy && fm_drive_of(srcs[i]) == dst_drive;
        const char *name = base_name(srcs[i]);
        if (same_drive) {
            unsigned long long items = 1, bytes = 0; /* the default: one instant rename() */
            if (S_ISDIR(sst.st_mode)) {
                char cand[FMGR_JOIN_MAX];
                struct stat clash_st;
                if (join_path(dst, name, cand, sizeof(cand)) == 0 && lstat(cand, &clash_st) == 0 &&
                    S_ISDIR(clash_st.st_mode) && conflict == FMGR_OVERWRITE) {
                    /* This one will have to merge into an existing folder instead of a
                     * single rename(): count its files individually so the progress total
                     * matches what will really happen. */
                    items = 0;
                    walk_count(srcs[i], 0, &items, &bytes);
                }
            }
            total_items += items; /* bytes stay out of total_bytes/needed_space: every file
                                    * here moves with rename(), never an actual copy */
        } else {
            unsigned long long items = 0, bytes = 0;
            walk_count(srcs[i], 0, &items, &bytes);
            total_items += items;
            total_bytes += bytes;
            needed_space += bytes;
        }
    }
    if (needed_space > dst_free) {
        snprintf(msg, cap, "Not enough free space on the destination (need %llu MB, have %llu MB)",
                 needed_space / (1024ULL * 1024ULL), dst_free / (1024ULL * 1024ULL));
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    memset(&g_task, 0, sizeof(g_task));
    g_task.state = FMGR_RUNNING;
    g_task.copy = copy;
    g_task.conflict = conflict;
    g_task.total_items = total_items > (unsigned long long)INT_MAX ? INT_MAX : (int)total_items;
    g_task.total_bytes = total_bytes;
    snprintf(g_task.what, sizeof(g_task.what), "Starting...");
    pthread_mutex_unlock(&g_lock);

    g_job.nsrcs = n;
    for (int i = 0; i < n; i++)
        snprintf(g_job.srcs[i], sizeof(g_job.srcs[i]), "%s", srcs[i]);
    snprintf(g_job.dst, sizeof(g_job.dst), "%s", dst);
    g_job.copy = copy;
    g_job.conflict = conflict;
    g_job.dst_drive = dst_drive;

    g_cancel = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, fmgr_worker, NULL)) {
        /* Unlike install_all_start's install (seconds), a copy or move can run for hours.
         * Running fmgr_worker inline here, on the only thread that also serves the panel and
         * delivers fmgr_cancel(), would make the task both unresponsive and uncancellable
         * for its whole duration -- degrading is worse than refusing. Roll back the RUNNING
         * state set above so fmgr_busy() does not get stuck forever with no worker to ever
         * finish and clear it. */
        pthread_mutex_lock(&g_lock);
        g_task.state = FMGR_IDLE;
        pthread_mutex_unlock(&g_lock);
        snprintf(msg, cap, "Could not start the background copy, try again");
        return -1;
    }
    pthread_detach(th);
    return 0;
}
