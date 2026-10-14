/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Minimal HTTP server for the PS4Mount panel (one request per connection). */
#include "web.h"

#include <ctype.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fm.h"
#include "page_data.h"

#define LOG_PATH "/data/ps4mount/log.txt"

int web_listen(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 8)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    return s;
}

/* The panel is served from the daemon's main loop: a client that stops reading
 * (each write may block SO_SNDTIMEO) is dropped after SEND_LIMIT seconds. */
#define SEND_LIMIT 10

static void send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    time_t start = time(NULL);
    while (n && time(NULL) - start < SEND_LIMIT) {
        ssize_t w = write(fd, c, n);
        if (w <= 0)
            return;
        c += w;
        n -= (size_t)w;
    }
}

/* extra: more header lines, each ending with \r\n ("" for none). */
static void reply_with(int fd, const char *status, const char *type, const char *extra, const void *body, size_t len) {
    char head[384];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "%sConnection: close\r\n\r\n",
                     status, type, len, extra);
    if (n < 0 || (size_t)n >= sizeof(head))
        return;
    send_all(fd, head, (size_t)n);
    send_all(fd, body, len);
}

static void reply(int fd, const char *status, const char *type, const void *body, size_t len) {
    reply_with(fd, status, type, "", body, len);
}

static void json_msg(int fd, int ok, const char *msg) {
    buf_t b = {0};
    buf_printf(&b, "{\"ok\":%s,\"%s\":\"", ok ? "true" : "false", ok ? "message" : "error");
    for (const char *p = msg; *p; p++) {
        if (*p == '"' || *p == '\\')
            buf_put(&b, "\\", 1);
        if ((unsigned char)*p >= 0x20)
            buf_put(&b, p, 1);
    }
    buf_printf(&b, "\"}");
    reply(fd, ok ? "200 OK" : "400 Bad Request", "application/json; charset=utf-8", b.data, b.len);
    buf_free(&b);
}

static int title_param(const char *path, char *tid) {
    const char *p = strstr(path, "title_id=");
    if (!p)
        return -1;
    p += 9;
    for (int i = 0; i < 9; i++) {
        char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return -1;
        tid[i] = c;
    }
    tid[9] = 0;
    return strncmp(tid, "CUSA", 4) ? -1 : 0;
}

/* Copies the percent-decoded value of key= from the query into out. -1 when missing/too long. */
static int fm_param(const char *path, const char *key, char *out, size_t cap) {
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    size_t plen = strlen(pat);

    const char *p = path;
    while ((p = strstr(p, pat)) != NULL) {
        /* Check if this match is properly anchored (after ? or &). */
        if (p > path && p[-1] != '?' && p[-1] != '&') {
            p++; /* Not anchored, try next occurrence. */
            continue;
        }

        /* Found a properly anchored match. */
        p += plen;
        size_t o = 0;
        while (*p && *p != '&' && *p != ' ') {
            char c = *p++;
            if (c == '%' && isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
                char h[3] = {p[0], p[1], 0};
                c = (char)strtol(h, NULL, 16);
                p += 2;
            } else if (c == '+') {
                c = ' ';
            }
            if (o + 1 >= cap)
                return -1;
            out[o++] = c;
        }
        out[o] = 0;
        return o ? 0 : -1;
    }
    return -1;
}

/* content_id= of the query, an add-on ID like UP0001-CUSA00001_00-ADDON00000000001. 0 when valid. */
static int content_param(const char *path, char *cid) {
    const char *p = strstr(path, "content_id=");
    if (!p)
        return -1;
    p += 11;
    for (int i = 0; i < 36; i++) {
        char c = p[i];
        int alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (i == 6 || i == 19 ? c != '-' : i == 16 ? c != '_' : !alnum)
            return -1;
        cid[i] = c;
    }
    cid[36] = 0;
    return (p[36] && p[36] != '&') || strncmp(cid + 7, "CUSA", 4) ? -1 : 0;
}

/* drive= of the query: usb0 to usb7 or ext0 to ext1. 1 when given and valid, 0 when absent,
 * -1 when invalid. */
static int drive_param(const char *path, char *drive, size_t n) {
    const char *p = strstr(path, "drive=");
    if (!p)
        return 0;
    p += 6;
    size_t l = strcspn(p, "&");
    if (n < 5 || l != 4 || !((!strncmp(p, "usb", 3) && p[3] >= '0' && p[3] <= '7') ||
                             (!strncmp(p, "ext", 3) && p[3] >= '0' && p[3] <= '1')))
        return -1;
    snprintf(drive, n, "%.4s", p);
    return 1;
}

/* version= of the query: "" (the highest), "none" or an APP_VER like 01.05. 0 when valid. */
static int version_param(const char *path, char *ver, size_t n) {
    const char *p = strstr(path, "version=");
    if (!p || n < 8)
        return -1;
    p += 8;
    size_t l = strcspn(p, "&");
    if (l == 4 && !strncmp(p, "none", 4)) {
        snprintf(ver, n, "none");
        return 0;
    }
    if (l && (l != 5 || p[2] != '.'))
        return -1;
    for (size_t i = 0; i < l; i++)
        if (i != 2 && (p[i] < '0' || p[i] > '9'))
            return -1;
    snprintf(ver, n, "%.*s", (int)l, p);
    return 0;
}

/* Replies {"<key>":"<text>"} with the text escaped for JSON. */
static void reply_text_json(int fd, const char *key, const buf_t *b) {
    buf_t j = {0};
    buf_printf(&j, "{\"%s\":\"", key);
    for (size_t i = 0; i < b->len; i++) {
        char c = (char)b->data[i];
        if (c == '\n')
            buf_put(&j, "\\n", 2);
        else if (c == '"' || c == '\\') {
            buf_put(&j, "\\", 1);
            buf_put(&j, &c, 1);
        } else if ((unsigned char)c >= 0x20)
            buf_put(&j, &c, 1);
    }
    buf_printf(&j, "\"}");
    reply(fd, "200 OK", "application/json; charset=utf-8", j.data, j.len);
    buf_free(&j);
}

/* Value of header name (lower case, with the colon) in the head of the request, trimmed. */
static void header_value(const char *req, const char *end, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *p = strstr(req, "\r\n"); p && p + 2 < end; p = strstr(p + 2, "\r\n")) {
        if (strncasecmp(p + 2, name, nl))
            continue;
        const char *v = p + 2 + nl;
        while (*v == ' ')
            v++;
        size_t l = strcspn(v, "\r\n");
        while (l && v[l - 1] == ' ')
            l--;
        if (l < n)
            snprintf(out, n, "%.*s", (int)l, v);
        return;
    }
}

static int pin_fails;
static time_t pin_locked_until;

/* 0 when the request may use /api/: no PIN is set, or X-PIN matches. Else replies 401/429. */
static int pin_guard(int fd, const char *req, const char *end) {
    static const char locked[] = "{\"ok\":false,\"error\":\"pin-locked\"}", wrong[] = "{\"ok\":false,\"error\":\"pin\"}";
    const char *pin = app_pin();
    if (!pin[0])
        return 0;
    time_t now = time(NULL);
    if (now < pin_locked_until) {
        reply(fd, "429 Too Many Requests", "application/json; charset=utf-8", locked, sizeof(locked) - 1);
        return -1;
    }
    char got[16];
    header_value(req, end, "x-pin:", got, sizeof(got));
    size_t pl = strlen(pin), gl = strlen(got);
    unsigned diff = (unsigned)(pl ^ gl);
    for (size_t i = 0; i < pl; i++)
        diff |= (unsigned char)pin[i] ^ (unsigned char)(i < gl ? got[i] : 0);
    if (!diff) {
        pin_fails = 0;
        return 0;
    }
    if (got[0] && ++pin_fails >= PIN_MAX_FAILS) {
        pin_fails = 0;
        pin_locked_until = now + PIN_LOCK_SECONDS;
        printf("[web] %d wrong PINs: panel locked for %d s\n", PIN_MAX_FAILS, PIN_LOCK_SECONDS);
    }
    reply(fd, "401 Unauthorized", "application/json; charset=utf-8", wrong, sizeof(wrong) - 1);
    return -1;
}

static void send_log(int fd) {
    buf_t b = {0};
    int f = open(LOG_PATH, O_RDONLY);
    if (f >= 0) {
        off_t end = lseek(f, 0, SEEK_END), start = end > 16384 ? end - 16384 : 0;
        lseek(f, start, SEEK_SET);
        char tmp[4096];
        ssize_t n;
        while ((n = read(f, tmp, sizeof(tmp))) > 0)
            buf_put(&b, tmp, (size_t)n);
        close(f);
    }
    reply_text_json(fd, "log", &b);
    buf_free(&b);
}

/* The whole log as a file to save (its last LOG_DOWNLOAD_MAX bytes if it ever grew past that). */
#define LOG_DOWNLOAD_MAX (4 << 20)

static void send_log_file(int fd) {
    buf_t b = {0};
    int f = open(LOG_PATH, O_RDONLY);
    if (f >= 0) {
        off_t end = lseek(f, 0, SEEK_END), start = end > LOG_DOWNLOAD_MAX ? end - LOG_DOWNLOAD_MAX : 0;
        lseek(f, start, SEEK_SET);
        char tmp[8192];
        ssize_t n;
        while (b.len < LOG_DOWNLOAD_MAX && (n = read(f, tmp, sizeof(tmp))) > 0)
            buf_put(&b, tmp, (size_t)n);
        close(f);
    }
    reply_with(fd, "200 OK", "text/plain; charset=utf-8",
               "Content-Disposition: attachment; filename=\"ps4mount-log.txt\"\r\n", b.data, b.len);
    buf_free(&b);
}

void web_serve(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    struct timeval rtv = {2, 0}, stv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
    char req[8192];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) {
        close(fd);
        return;
    }
    req[n] = 0;
    /* A long fm path (the move API can be several KB percent-encoded) may legitimately span
     * more than one TCP segment, so the first read above can return before the request line
     * is fully buffered. Parsing method/path against a half-arrived line would make the 414
     * guard below misread a merely-not-yet-received target as too long. Keep reading (bounded
     * by the same SO_RCVTIMEO as a stalled client) until a newline shows the line is complete,
     * or the buffer is full (a genuinely over-long target, which the 414 guard then catches). */
    while (!memchr(req, '\n', (size_t)n) && (size_t)n < sizeof(req) - 1) {
        ssize_t m = read(fd, req + n, sizeof(req) - 1 - (size_t)n);
        if (m <= 0)
            break;
        n += m;
        req[n] = 0;
    }
    /* the body of a POST may arrive after the headers */
    char *body = strstr(req, "\r\n\r\n");
    const char *cl = strstr(req, "Content-Length:");
    if (!cl)
        cl = strstr(req, "content-length:");
    long want = cl ? strtol(cl + 15, NULL, 10) : 0;
    while (want > 0 && (size_t)n < sizeof(req) - 1 && (!body || (long)(n - (body + 4 - req)) < want)) {
        ssize_t m = read(fd, req + n, sizeof(req) - 1 - (size_t)n);
        if (m <= 0)
            break;
        n += m;
        req[n] = 0;
        body = strstr(req, "\r\n\r\n");
    }
    const char *head_end = body ? body : req + n;
    body = body ? body + 4 : req + n;
    /* path is sized for the fm API's worst case: /api/fm/move?src=<enc>&dst=<enc> with each
     * path up to 1023 bytes percent-encoded (up to x3) plus the literal parts. req itself is
     * already an 8192-byte stack buffer above, so this one keeps the same style. */
    char method[8] = "", path[8192] = "", tid[10], msg[256];
    int consumed = 0;
    sscanf(req, "%7s %8191s%n", method, path, &consumed);
    /* %8191s stops at the field width even when the real request target is longer, which
     * would otherwise let a truncated-but-syntactically-valid path (e.g. naming the parent
     * of the intended file) slip through and be dispatched. Refuse instead of guessing: the
     * character right after the token we scanned must be request-line whitespace, or the
     * target did not fit and we must not proceed. */
    if (consumed > 0 && path[0] && req[consumed] != ' ' && req[consumed] != '\r' && req[consumed] != '\n') {
        reply(fd, "414 URI Too Long", "text/plain", "uri too long\n", 13);
        close(fd);
        return;
    }
    int post = !strcmp(method, "POST");

    if (!strncmp(path, "/api/", 5) && pin_guard(fd, req, head_end)) {
        close(fd);
        return;
    }
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        reply(fd, "200 OK", "text/html; charset=utf-8", index_html, sizeof(index_html));
    } else if (!strcmp(path, "/api/games")) {
        buf_t b = {0};
        app_games_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (!strncmp(path, "/icon/", 6)) {
        buf_t b = {0};
        snprintf(tid, sizeof(tid), "%.9s", path + 6);
        if (!app_icon(tid, &b))
            reply(fd, "200 OK", "image/png", b.data, b.len);
        else
            reply(fd, "404 Not Found", "text/plain", "no icon\n", 8);
        buf_free(&b);
    } else if (post && !strncmp(path, "/api/mount", 10)) {
        if (title_param(path, tid))
            json_msg(fd, 0, "invalid title_id");
        else {
            int rc = app_add(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/unmount", 12)) {
        if (title_param(path, tid))
            json_msg(fd, 0, "invalid title_id");
        else {
            int rc = app_remove(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/launch", 11)) {
        if (title_param(path, tid))
            json_msg(fd, 0, "invalid title_id");
        else {
            int rc = app_launch(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (!post && !strncmp(path, "/api/fm/list", 12)) {
        char p[1024];
        if (fm_param(path, "path", p, sizeof(p)) || !fm_path_ok(p))
            json_msg(fd, 0, "invalid path");
        else {
            buf_t b = {0};
            if (!app_fm_list(p, &b, msg, sizeof(msg)))
                reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
            else
                json_msg(fd, 0, msg);
            buf_free(&b);
        }
    } else if (post && !strncmp(path, "/api/fm/mkdir", 13)) {
        char p[1024], name[256];
        if (fm_param(path, "path", p, sizeof(p)) || !fm_path_ok(p))
            json_msg(fd, 0, "invalid path");
        else if (fm_param(path, "name", name, sizeof(name)) || !fm_name_ok(name))
            json_msg(fd, 0, "invalid name");
        else {
            int rc = app_fm_mkdir(p, name, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/fm/rename", 14)) {
        char p[1024], name[256];
        if (fm_param(path, "path", p, sizeof(p)) || !fm_path_ok(p) || fm_is_drive_root(p))
            json_msg(fd, 0, "invalid path");
        else if (fm_param(path, "name", name, sizeof(name)) || !fm_name_ok(name))
            json_msg(fd, 0, "invalid name");
        else {
            int rc = app_fm_rename(p, name, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/fm/delete", 14)) {
        char p[1024];
        if (fm_param(path, "path", p, sizeof(p)) || !fm_path_ok(p) || fm_is_drive_root(p))
            json_msg(fd, 0, "invalid path");
        else {
            int rc = app_fm_delete(p, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/fm/move", 12)) {
        char s[1024], d[1024];
        if (fm_param(path, "src", s, sizeof(s)) || !fm_path_ok(s) || fm_is_drive_root(s) ||
            fm_param(path, "dst", d, sizeof(d)) || !fm_path_ok(d))
            json_msg(fd, 0, "invalid path");
        else {
            int rc = app_fm_move(s, d, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && (!strncmp(path, "/api/exclude", 12) || !strncmp(path, "/api/include", 12))) {
        if (title_param(path, tid))
            json_msg(fd, 0, "invalid title_id");
        else {
            int rc = app_exclude(tid, path[5] == 'e', msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strcmp(path, "/api/resume")) {
        int rc = app_resume(msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (post && !strncmp(path, "/api/rescan", 11) && (path[11] == 0 || path[11] == '?')) {
        char drive[8];
        int rc = drive_param(path, drive, sizeof(drive));
        if (rc < 0)
            json_msg(fd, 0, "invalid drive");
        else {
            rc = app_rescan(rc ? drive : NULL, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/speedtest", 14) && (path[14] == 0 || path[14] == '?')) {
        char drive[8];
        if (drive_param(path, drive, sizeof(drive)) != 1)
            json_msg(fd, 0, "invalid drive");
        else {
            int rc = app_speedtest(drive, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/restore", 12) && (path[12] == 0 || path[12] == '?')) {
        int rc;
        if (strstr(path, "title_id="))
            rc = title_param(path, tid) ? (snprintf(msg, sizeof(msg), "invalid title_id"), -1)
                                        : app_restore(tid, msg, sizeof(msg));
        else
            rc = app_restore(NULL, msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (post && !strcmp(path, "/api/restored/forget")) {
        int rc = app_restored_forget(msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (!strcmp(path, "/api/config") && !post) {
        buf_t b = {0};
        app_config_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (post && !strcmp(path, "/api/config")) {
        int rc = app_config_set(body, msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (!strcmp(path, "/api/log")) {
        send_log(fd);
    } else if (!strcmp(path, "/api/log.txt")) {
        send_log_file(fd);
    } else if (!strcmp(path, "/api/problems")) {
        buf_t b = {0};
        app_problems_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (post && !strncmp(path, "/api/update?", 12)) {
        char ver[8];
        if (title_param(path, tid) || version_param(path, ver, sizeof(ver)))
            json_msg(fd, 0, "invalid title_id or version");
        else {
            int rc = app_set_update(tid, ver, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/addon?", 11)) {
        char cid[37];
        const char *on = strstr(path, "on=");
        if (content_param(path, cid) || !on || (on[3] != '0' && on[3] != '1') || (on[4] && on[4] != '&'))
            json_msg(fd, 0, "invalid content_id or on");
        else {
            int rc = app_addon(cid, on[3] == '1', msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (!strcmp(path, "/api/diag")) {
        buf_t b = {0};
        app_diag(&b);
        reply_text_json(fd, "text", &b);
        buf_free(&b);
    } else if (post && !strcmp(path, "/api/stop")) {
        json_msg(fd, 1, "PS4Mount is stopping");
        app_stop();
    } else {
        reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
    }
    close(fd);
}
