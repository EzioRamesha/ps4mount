/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Web panel tests: a client that stalls must not hold the daemon, which serves the
 * panel from its main loop. Build and run with: bash test/run.sh */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "web.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

/* A games list far larger than the socket buffers. */
void app_games_json(buf_t *out) {
    static char chunk[65536];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 512; i++)
        buf_put(out, chunk, sizeof(chunk));
}
int app_add(const char *tid, char *msg, size_t cap) { return snprintf(msg, cap, "%s", tid) < 0; }
int app_remove(const char *tid, char *msg, size_t cap) { return snprintf(msg, cap, "%s", tid) < 0; }
int app_launch(const char *tid, int force, char *msg, size_t cap) {
    (void)force;
    return snprintf(msg, cap, "launch[%s]", tid) < 0;
}
int app_rescan(const char *drive, char *msg, size_t cap) {
    return snprintf(msg, cap, "rescan[%s]", drive ? drive : "all") < 0;
}
int app_speedtest(const char *drive, char *msg, size_t cap) {
    if (!strcmp(drive, "usb2")) {
        snprintf(msg, cap, "A speed test is already running");
        return -1;
    }
    return snprintf(msg, cap, "speed[%s]", drive) < 0;
}
int app_game_json(const char *tid, buf_t *out, char *msg, size_t cap) {
    (void)msg;
    (void)cap;
    buf_printf(out, "{\"title_id\":\"%s\",\"updates\":[],\"dlcs\":[]}", tid);
    return 0;
}
int app_choose_update(const char *tid, const char *rel, char *msg, size_t cap) {
    return snprintf(msg, cap, "update[%s %s]", tid, rel) < 0;
}
int app_choose_dlc(const char *tid, const char *on, char *msg, size_t cap) {
    return snprintf(msg, cap, "dlc[%s %s]", tid, on) < 0;
}
void app_diag(buf_t *out) { buf_printf(out, "diag \"line\"\n"); }
void app_problems_json(buf_t *out) { buf_printf(out, "{\"problems\":[]}"); }
int app_addon(const char *content_id, int on, char *msg, size_t cap) {
    return snprintf(msg, cap, "%s=%d", content_id, on) < 0;
}
static const char *test_pin = "";
const char *app_pin(void) { return test_pin; }
int app_set_update(const char *tid, const char *ver, char *msg, size_t cap) {
    return snprintf(msg, cap, "%s=[%s]", tid, ver) < 0;
}
int app_exclude(const char *tid, int on, char *msg, size_t cap) {
    return snprintf(msg, cap, "%s %s", tid, on ? "excluded" : "included") < 0;
}
int app_resume(char *msg, size_t cap) { return snprintf(msg, cap, "resumed") < 0; }
void app_stop(void) {}
int app_restore(const char *tid, char *msg, size_t cap) { return snprintf(msg, cap, "%s", tid ? tid : "all") < 0; }
int app_restored_forget(char *msg, size_t cap) { return snprintf(msg, cap, "forgotten") < 0; }
void app_config_json(buf_t *out) { buf_printf(out, "{}"); }
int app_config_set(const char *body, char *msg, size_t cap) { return snprintf(msg, cap, "%s", body) < 0; }
int app_icon(const char *tid, buf_t *out) { return tid && out ? -1 : -1; }
int app_fm_list(const char *path, buf_t *out, char *msg, size_t cap) {
    (void)msg; (void)cap;
    buf_printf(out, "{\"path\":\"%s\",\"entries\":[]}", path);
    return 0;
}
int app_fm_mkdir(const char *dir, const char *name, char *msg, size_t cap) {
    return snprintf(msg, cap, "mkdir[%s + %s]", dir, name) < 0;
}
int app_fm_rename(const char *path, const char *name, char *msg, size_t cap) {
    return snprintf(msg, cap, "rename[%s -> %s]", path, name) < 0;
}
int app_fm_delete(const char *path, char *msg, size_t cap) {
    return snprintf(msg, cap, "delete[%s]", path) < 0;
}
int app_fm_move(const char *src, const char *dst, char *msg, size_t cap) {
    return snprintf(msg, cap, "move[%s -> %s]", src, dst) < 0;
}
void app_fm_task_json(buf_t *out) { buf_printf(out, "{\"state\":\"idle\"}"); }
int app_fm_copy(const char *srcs, const char *dst, int copy, int conflict, char *msg, size_t cap) {
    return snprintf(msg, cap, "copy[%s -> %s copy=%d conflict=%d]", srcs, dst, copy, conflict) < 0;
}
int app_fm_cancel(char *msg, size_t cap) { return snprintf(msg, cap, "cancelled") < 0; }

static int connect_panel(void) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (c >= 0 && connect(c, (struct sockaddr *)&a, sizeof(a))) {
        close(c);
        return -1;
    }
    return c;
}

/* Seconds web_serve took for the connection, or -1 when it was still busy after limit. */
static int serve_seconds(int listen_fd, int limit) {
    time_t start = time(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        web_serve(listen_fd);
        _exit(0);
    }
    int status;
    while (time(NULL) - start <= limit) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            return (int)(time(NULL) - start);
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

static void test_stalled_reader(int listen_fd) {
    printf("client that stops reading\n");
    int c = connect_panel();
    CHECK(c >= 0);
    const char req[] = "GET /api/games HTTP/1.1\r\n\r\n";
    CHECK(write(c, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1));
    int s = serve_seconds(listen_fd, 15);
    printf("  served in %d s\n", s);
    CHECK(s >= 0 && s <= 12);
    close(c);
}

static void test_silent_client(int listen_fd) {
    printf("client that sends nothing\n");
    int c = connect_panel();
    CHECK(c >= 0);
    int s = serve_seconds(listen_fd, 10);
    printf("  served in %d s\n", s);
    CHECK(s >= 0 && s <= 3);
    close(c);
}

/* A client that sends a few bytes of a request line and then nothing -- no newline ever
 * arrives -- must not stall the daemon's single-threaded main loop: web_serve's own
 * READ_LIMIT (3s) cumulative deadline on top of the per-read SO_RCVTIMEO (2s) must still
 * close it out quickly, well under the 15 s used for test_stalled_reader above. */
static void test_drip_client(int listen_fd) {
    printf("client that drips the request line\n");
    int c = connect_panel();
    CHECK(c >= 0);
    const char partial[] = "GET /x";
    CHECK(write(c, partial, sizeof(partial) - 1) == (ssize_t)(sizeof(partial) - 1));
    int s = serve_seconds(listen_fd, 10);
    printf("  served in %d s\n", s);
    CHECK(s >= 0 && s <= 5);
    char resp[256] = {0};
    size_t len = 0;
    ssize_t r;
    while (len + 1 < sizeof(resp) && (r = read(c, resp + len, sizeof(resp) - 1 - len)) > 0)
        len += (size_t)r;
    resp[len] = 0;
    CHECK(strstr(resp, "414") != NULL); /* never silently dispatched with a partial path */
    close(c);
}

/* A POST whose body arrives one byte a second keeps every read() inside SO_RCVTIMEO, so only
 * the READ_LIMIT deadline stops it from holding the main loop until the body is complete. */
static void test_drip_body(int listen_fd) {
    printf("client that drips the request body\n");
    int c = connect_panel();
    CHECK(c >= 0);
    const char head[] = "POST /api/config HTTP/1.1\r\nContent-Length: 60\r\n\r\nx";
    CHECK(write(c, head, sizeof(head) - 1) == (ssize_t)(sizeof(head) - 1));
    pid_t dripper = fork();
    if (dripper == 0) {
        for (int i = 0; i < 20; i++) {
            sleep(1);
            if (write(c, "x", 1) != 1)
                break;
        }
        _exit(0);
    }
    int s = serve_seconds(listen_fd, 15);
    printf("  served in %d s\n", s);
    CHECK(s >= 0 && s <= 6);
    kill(dripper, SIGKILL);
    waitpid(dripper, NULL, 0);
    close(c);
}

/* Sends req and returns the whole response in out (served by a child process). */
static void request(int listen_fd, const char *req, char *out, size_t n) {
    out[0] = 0;
    int c = connect_panel();
    if (c < 0)
        return;
    if (write(c, req, strlen(req)) != (ssize_t)strlen(req)) {
        close(c);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        web_serve(listen_fd);
        _exit(0);
    }
    size_t len = 0;
    ssize_t r;
    while (len + 1 < n && (r = read(c, out + len, n - 1 - len)) > 0)
        len += (size_t)r;
    out[len] = 0;
    close(c);
    waitpid(pid, NULL, 0);
}

/* Like request(), but writes req1 then forks web_serve (so its first read only sees req1),
 * then waits before writing req2 -- simulating a request line split across TCP segments. */
static void request_split(int listen_fd, const char *req1, const char *req2, char *out, size_t n) {
    out[0] = 0;
    int c = connect_panel();
    if (c < 0)
        return;
    if (write(c, req1, strlen(req1)) != (ssize_t)strlen(req1)) {
        close(c);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        web_serve(listen_fd);
        _exit(0);
    }
    usleep(100000);
    if (write(c, req2, strlen(req2)) != (ssize_t)strlen(req2)) {
        close(c);
        waitpid(pid, NULL, 0);
        return;
    }
    size_t len = 0;
    ssize_t r;
    while (len + 1 < n && (r = read(c, out + len, n - 1 - len)) > 0)
        len += (size_t)r;
    out[len] = 0;
    close(c);
    waitpid(pid, NULL, 0);
}

static void test_diag(int listen_fd) {
    printf("diagnostics as JSON\n");
    char resp[4096];
    request(listen_fd, "GET /api/diag HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "200 OK") != NULL);
    CHECK(strstr(resp, "{\"text\":\"diag \\\"line\\\"\\n\"}") != NULL);
}

static void test_update_choice(int listen_fd) {
    printf("update choice parameters\n");
    char resp[2048];
    request(listen_fd, "POST /api/update?title_id=CUSA00001&version=01.05 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "CUSA00001=[01.05]") != NULL);
    request(listen_fd, "POST /api/update?title_id=CUSA00001&version=none HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "CUSA00001=[none]") != NULL);
    request(listen_fd, "POST /api/update?title_id=CUSA00001&version= HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "CUSA00001=[]") != NULL);
    request(listen_fd, "POST /api/update?title_id=CUSA00001&version=../x HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid") != NULL);
    request(listen_fd, "POST /api/update?title_id=CUSA00001&version=1.5 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid") != NULL);
    request(listen_fd, "GET /api/problems HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "{\"problems\":[]}") != NULL);
}

static void test_log_download(int listen_fd) {
    printf("log download\n");
    char resp[4096];
    request(listen_fd, "GET /api/log.txt HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "200 OK") != NULL && strstr(resp, "text/plain") != NULL);
    CHECK(strstr(resp, "Content-Disposition: attachment; filename=\"ps4mount-log.txt\"\r\n") != NULL);
    char *end = strstr(resp, "\r\n\r\n");
    CHECK(end != NULL && strstr(resp, "Connection: close\r\n\r\n") == end - 17);
}

static void test_launch(int listen_fd) {
    printf("start a game\n");
    char resp[2048];
    request(listen_fd, "POST /api/launch?title_id=CUSA00001 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "launch[CUSA00001]") != NULL);
    request(listen_fd, "POST /api/launch?title_id=../etc HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid title_id") != NULL);
    request(listen_fd, "POST /api/launch HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid title_id") != NULL);
    request(listen_fd, "POST /api/launch?title_id=CUSA0000X HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid title_id") != NULL);
    request(listen_fd, "POST /api/launch?title_id=CUSA000012 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid title_id") != NULL);
    request(listen_fd, "POST /api/launch?title_id=PLAS00001 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid title_id") != NULL);
    request(listen_fd, "GET /api/launch?title_id=CUSA00001 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "404") != NULL);
}

static void test_rescan(int listen_fd) {
    printf("scan all drives or one\n");
    char resp[2048];
    request(listen_fd, "POST /api/rescan HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "rescan[all]") != NULL);
    request(listen_fd, "POST /api/rescan?drive=usb3 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "rescan[usb3]") != NULL);
    request(listen_fd, "POST /api/rescan?drive=ext1 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "rescan[ext1]") != NULL);
    static const char *const bad[] = {"usb8", "ext2", "usb", "usb10", "../x", "USB0"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char req[128];
        snprintf(req, sizeof(req), "POST /api/rescan?drive=%s HTTP/1.1\r\n\r\n", bad[i]);
        request(listen_fd, req, resp, sizeof(resp));
        CHECK(strstr(resp, "invalid drive") != NULL);
    }
    request(listen_fd, "POST /api/rescanx HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "404") != NULL);
}

static void test_speedtest(int listen_fd) {
    printf("speed test of one drive\n");
    char resp[2048];
    request(listen_fd, "POST /api/speedtest?drive=usb1 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "\"ok\":true") != NULL && strstr(resp, "speed[usb1]") != NULL);
    request(listen_fd, "POST /api/speedtest?drive=usb2 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "\"ok\":false") != NULL && strstr(resp, "already running") != NULL);
    static const char *const bad[] = {"POST /api/speedtest HTTP/1.1\r\n\r\n",
                                      "POST /api/speedtest?drive=usb9 HTTP/1.1\r\n\r\n",
                                      "POST /api/speedtest?drive=../x HTTP/1.1\r\n\r\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        request(listen_fd, bad[i], resp, sizeof(resp));
        CHECK(strstr(resp, "invalid drive") != NULL);
    }
    request(listen_fd, "GET /api/speedtest?drive=usb1 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "speed[usb1]") == NULL);
}

static void test_addon(int listen_fd) {
    printf("add-on on/off parameters\n");
    char resp[2048];
    request(listen_fd, "POST /api/addon?content_id=UP0001-CUSA00001_00-ADDON00000000001&on=0 HTTP/1.1\r\n\r\n", resp,
            sizeof(resp));
    CHECK(strstr(resp, "UP0001-CUSA00001_00-ADDON00000000001=0") != NULL);
    request(listen_fd, "POST /api/addon?on=1&content_id=UP0001-CUSA00001_00-ADDON00000000001 HTTP/1.1\r\n\r\n", resp,
            sizeof(resp));
    CHECK(strstr(resp, "UP0001-CUSA00001_00-ADDON00000000001=1") != NULL);
    static const char *const bad[] = {
        "POST /api/addon?content_id=UP0001-CUSA00001_00-ADDON0000000000/&on=0 HTTP/1.1\r\n\r\n",
        "POST /api/addon?content_id=UP0001-CUSA00001_00-ADDON00000000001X&on=0 HTTP/1.1\r\n\r\n",
        "POST /api/addon?content_id=UP0001-CUSA00001_00-ADDON0000000000&on=0 HTTP/1.1\r\n\r\n",
        "POST /api/addon?content_id=UP0001-PPSA00001_00-ADDON00000000001&on=0 HTTP/1.1\r\n\r\n",
        "POST /api/addon?content_id=UP0001-CUSA00001_00-ADDON00000000001&on=2 HTTP/1.1\r\n\r\n",
        "POST /api/addon?content_id=UP0001-CUSA00001_00-ADDON00000000001 HTTP/1.1\r\n\r\n",
        "POST /api/addon?content_id=../../../../etc/passwd000000000000&on=0 HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        request(listen_fd, bad[i], resp, sizeof(resp));
        CHECK(strstr(resp, "invalid") != NULL);
    }
}

static void test_fm_api(int listen_fd) {
    printf("file manager parameters\n");
    char resp[4096];
    request(listen_fd, "GET /api/fm/list?path=/mnt/usb0/my%20games HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "\"path\":\"/mnt/usb0/my games\"") != NULL); /* %20 decoded */
    request(listen_fd, "GET /api/fm/list?path=/data/ps4mount HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid path") != NULL);
    request(listen_fd, "GET /api/fm/list?path=/mnt/usb0/../usb1 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid path") != NULL);
    request(listen_fd, "POST /api/fm/mkdir?path=/mnt/usb0&name=new%20folder HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "mkdir[/mnt/usb0 + new folder]") != NULL);
    request(listen_fd, "POST /api/fm/mkdir?path=/mnt/usb0&name=a/b HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid name") != NULL);
    request(listen_fd, "POST /api/fm/rename?path=/mnt/usb0/a.pkg&name=b.pkg HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "rename[/mnt/usb0/a.pkg -> b.pkg]") != NULL);
    request(listen_fd, "POST /api/fm/delete?path=/mnt/usb0/a.pkg HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "delete[/mnt/usb0/a.pkg]") != NULL);
    request(listen_fd, "POST /api/fm/delete?path=/mnt/usb0 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid path") != NULL); /* a drive root is never deleted */
    request(listen_fd, "POST /api/fm/move?src=/mnt/usb0/a.pkg&dst=/mnt/usb0/games HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "move[/mnt/usb0/a.pkg -> /mnt/usb0/games]") != NULL);
    request(listen_fd, "GET /api/fm/delete?path=/mnt/usb0/a.pkg HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "404") != NULL); /* mutations are POST only */
    request(listen_fd, "POST /api/fm/mkdir?path=/mnt/usb0/name=x&name=real HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "mkdir[/mnt/usb0/name=x + real]") != NULL); /* name param not spoofed by path value */
    request(listen_fd, "POST /api/fm/move?src=/mnt/usb0/dst=a&dst=/mnt/usb0/games HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "move[/mnt/usb0/dst=a -> /mnt/usb0/games]") != NULL); /* dst param not spoofed by src value */
}

static void test_fm_copy_api(int listen_fd) {
    printf("file manager copy/move task endpoints\n");
    char resp[4096];
    request(listen_fd, "GET /api/fm/task HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "{\"state\":\"idle\"}") != NULL);

    const char *body = "/mnt/usb0/a.pkg\n/mnt/usb0/b.pkg";
    char req[512];
    snprintf(req, sizeof(req),
             "POST /api/fm/copy?dst=/mnt/usb0/games&copy=1&conflict=0 HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(body), body);
    request(listen_fd, req, resp, sizeof(resp));
    /* json_msg strips control characters (including the '\n' between sources), so check the
     * pieces either side of it rather than the literal joined string. */
    CHECK(strstr(resp, "copy[/mnt/usb0/a.pkg") != NULL);
    CHECK(strstr(resp, "/mnt/usb0/b.pkg -> /mnt/usb0/games copy=1 conflict=0]") != NULL);

    request(listen_fd,
            "POST /api/fm/copy?dst=/data/ps4mount&copy=1&conflict=0 HTTP/1.1\r\nContent-Length: 4\r\n\r\nabcd", resp,
            sizeof(resp));
    CHECK(strstr(resp, "invalid path") != NULL); /* /data/ps4mount is never a valid fm path */

    request(listen_fd, "POST /api/fm/cancel HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "cancelled") != NULL);

    request(listen_fd, "GET /api/fm/copy?dst=/mnt/usb0&copy=1&conflict=0 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "404") != NULL); /* mutations are POST only */
    request(listen_fd, "GET /api/fm/cancel HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "404") != NULL);
}

/* Regression for a copy/move request whose body is cut short of its own declared
 * Content-Length (the client stalled, or many long selected paths did not fit the request
 * buffer): the srcs list must never be silently processed short -- that would start a real
 * copy/move on a different, smaller set of files than the user selected. */
static void test_fm_copy_truncated(int listen_fd) {
    printf("file manager copy request body shorter than Content-Length\n");
    char resp[4096];
    const char *sent = "/mnt/usb0/a.pkg"; /* 15 bytes actually written */
    char req[512];
    snprintf(req, sizeof(req),
             "POST /api/fm/copy?dst=/mnt/usb0/games&copy=1&conflict=0 HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(sent) + 50, sent); /* claims 50 bytes more than actually sent */
    request(listen_fd, req, resp, sizeof(resp));
    CHECK(strstr(resp, "Too many files selected at once") != NULL);
    CHECK(strstr(resp, "copy[") == NULL); /* never dispatched with the partial list */
}

/* Regression for the request-target truncation bug: the method/path line used to be
 * sscanf'd into a 255-byte buffer, so a real (and valid) fm path longer than that got cut
 * mid-path into something that could still be a syntactically valid path -- e.g. the parent
 * of the intended target -- and got dispatched anyway. web_serve now sizes that buffer for
 * the fm API's worst case and refuses to dispatch when the target did not fit. */
static void test_fm_long_target(int listen_fd) {
    printf("file manager long request target\n");
    /* 240 bytes: long enough that "/api/fm/delete?path=" (20 bytes) + path exceeds the old
     * 255-byte request-target buffer, short enough that app_fm_delete's "delete[<path>]"
     * reply still fits the unrelated, unchanged msg[256] reply buffer in web_serve. */
    char path[300] = "/mnt/usb0/";
    size_t base = strlen(path);
    for (size_t i = base; i < 240; i++)
        path[i] = 'a';
    path[240] = 0;
    CHECK(strlen(path) == 240);

    char req[768], resp[4096];
    int rl = snprintf(req, sizeof(req), "POST /api/fm/delete?path=%s HTTP/1.1\r\n\r\n", path);
    CHECK(rl > 255 && (size_t)rl < sizeof(req)); /* the whole request target, not just the path value */
    request(listen_fd, req, resp, sizeof(resp));
    char want[300];
    snprintf(want, sizeof(want), "delete[%s]", path);
    CHECK(strstr(resp, want) != NULL); /* the full, un-truncated decoded path reached app_fm_delete */

    /* An absurdly long request target (longer than the new 8192-byte path buffer) must be
     * rejected outright with 414, never dispatched with a silently truncated path. */
    char hugereq[20000];
    char *p = hugereq;
    p += snprintf(p, sizeof(hugereq) - (size_t)(p - hugereq), "POST /api/fm/delete?path=/mnt/usb0/");
    while ((size_t)(p - hugereq) < sizeof(hugereq) - 32)
        *p++ = 'a';
    *p = 0;
    strcat(hugereq, " HTTP/1.1\r\n\r\n");
    request(listen_fd, hugereq, resp, sizeof(resp));
    CHECK(strstr(resp, "414") != NULL);
    CHECK(strstr(resp, "delete[") == NULL); /* never dispatched to app_fm_delete */
}

/* Regression for the 414 guard itself misfiring: a long fm request legitimately spans more
 * than one TCP segment, and web_serve's single-shot initial read() could return before the
 * request line was fully buffered, making the 414 guard misread "not here yet" as "too long".
 * Splits a valid long request across two writes 100ms apart and expects it to still dispatch
 * with the full path -- never a false 414. */
static void test_fm_split_request(int listen_fd) {
    printf("file manager request split across reads\n");
    char path[300] = "/mnt/usb0/";
    size_t base = strlen(path);
    for (size_t i = base; i < 240; i++)
        path[i] = 'a';
    path[240] = 0;

    char req[768];
    int rl = snprintf(req, sizeof(req), "POST /api/fm/delete?path=%s HTTP/1.1\r\n\r\n", path);
    CHECK(rl > 255 && (size_t)rl < sizeof(req)); /* long enough to matter; split happens well before the end */

    char req1[128] = {0}, resp[4096];
    memcpy(req1, req, 100);
    request_split(listen_fd, req1, req + 100, resp, sizeof(resp));
    char want[300];
    snprintf(want, sizeof(want), "delete[%s]", path);
    CHECK(strstr(resp, want) != NULL); /* dispatched with the full path */
    CHECK(strstr(resp, "414") == NULL); /* not a false 414 from reading only the first chunk */
}

static void test_pin_guard(int listen_fd) {
    printf("panel PIN\n");
    char resp[2048];
    test_pin = "4821";
    request(listen_fd, "GET /api/problems HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "401") != NULL && strstr(resp, "\"pin\"") != NULL);
    request(listen_fd, "GET / HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "200 OK") != NULL);
    request(listen_fd, "GET /api/problems HTTP/1.1\r\nHost: x\r\nx-pin:  4821 \r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "{\"problems\":[]}") != NULL);
    request(listen_fd, "GET /api/problems HTTP/1.1\r\nX-PIN: 48210\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "401") != NULL);
    request(listen_fd, "GET /api/problems HTTP/1.1\r\nX-PIN: 482\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "401") != NULL);
    /* a PIN in the query or body is not a header */
    request(listen_fd, "POST /api/update?title_id=CUSA00001&version=&x-pin:4821 HTTP/1.1\r\n\r\nX-PIN: 4821", resp,
            sizeof(resp));
    CHECK(strstr(resp, "401") != NULL);
    /* the lock lives in the server process: serve the wrong PINs and the right one in one child */
    int fds[PIN_MAX_FAILS + 1];
    for (int i = 0; i <= PIN_MAX_FAILS; i++) {
        const char *r = i < PIN_MAX_FAILS ? "GET /api/problems HTTP/1.1\r\nX-PIN: 0000\r\n\r\n"
                                          : "GET /api/problems HTTP/1.1\r\nX-PIN: 4821\r\n\r\n";
        fds[i] = connect_panel();
        CHECK(fds[i] >= 0 && write(fds[i], r, strlen(r)) == (ssize_t)strlen(r));
    }
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i <= PIN_MAX_FAILS; i++)
            web_serve(listen_fd);
        _exit(0);
    }
    for (int i = 0; i <= PIN_MAX_FAILS; i++) {
        size_t len = 0;
        ssize_t r;
        while (len + 1 < sizeof(resp) && (r = read(fds[i], resp + len, sizeof(resp) - 1 - len)) > 0)
            len += (size_t)r;
        resp[len] = 0;
        close(fds[i]);
        if (i == PIN_MAX_FAILS - 1)
            CHECK(strstr(resp, "401") != NULL);
    }
    waitpid(pid, NULL, 0);
    CHECK(strstr(resp, "429") != NULL && strstr(resp, "pin-locked") != NULL);
    test_pin = "";
    request(listen_fd, "GET /api/problems HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "{\"problems\":[]}") != NULL);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = web_listen();
    CHECK(listen_fd >= 0);
    if (listen_fd >= 0) {
        test_diag(listen_fd);
        test_update_choice(listen_fd);
        test_launch(listen_fd);
        test_fm_api(listen_fd);
        test_fm_copy_api(listen_fd);
        test_fm_copy_truncated(listen_fd);
        test_fm_long_target(listen_fd);
        test_fm_split_request(listen_fd);
        test_rescan(listen_fd);
        test_speedtest(listen_fd);
        test_log_download(listen_fd);
        test_addon(listen_fd);
        test_pin_guard(listen_fd);
        test_stalled_reader(listen_fd);
        test_silent_client(listen_fd);
        test_drip_client(listen_fd);
        test_drip_body(listen_fd);
        close(listen_fd);
    }
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
