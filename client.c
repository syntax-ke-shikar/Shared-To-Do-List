// client.c (binary protocol, no JSON)
// UI & behavior preserved from your previous client version.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <time.h>

#define SERVER_HOST "127.0.0.1"
#define SERVER_PORT 8765

// Message types (same numeric values)
#define MSG_SYNC_FULL   0x01
#define MSG_ADD_TASK    0x02
#define MSG_UPDATE_TASK 0x03
#define MSG_DELETE_TASK 0x04
#define MSG_ERROR       0xFF
#define MSG_AUTH        0x10
#define MSG_AUTH_RESP   0x11
#define MSG_TOAST       0x12
#define MSG_REMINDER    0x13   // reminder message

typedef struct task {
    char id[64];
    char title[256];
    int done;
    int version;
    long updated_at;
    long due_at;

    char updated_by[128];   // who updated/added last

    struct task *next;
} task_t;

static task_t *tasks_head = NULL;
static pthread_mutex_t tasks_mutex = PTHREAD_MUTEX_INITIALIZER;

static int sockfd = -1;
static WINDOW *mainwin = NULL, *statuswin = NULL;
static char my_username[128] = "";

static int selected_index = 0;
static int show_info = 0;


/* ---------- toast & reminder state ---------- */
static char toast_msg[256] = "";
static pthread_mutex_t toast_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t toast_until = 0;

/* NEW: persistent reminder state */
static int  reminder_pending = 0;
static char reminder_task_title[256] = "";

/* ---------- framing send/recv ---------- */
static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf; size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += n;
    }
    return (int)sent;
}

static int recv_all(int fd, void *buf, size_t len) {
    char *p = buf; size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n <= 0) {
            if (n == 0) return 0;
            if (errno == EINTR) continue;
            return -1;
        }
        got += n;
    }
    return (int)got;
}

static int send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len) {
    uint32_t n = htonl(payload_len + 1);
    if (send_all(fd, &n, 4) != 4) return -1;
    if (send_all(fd, &type, 1) != 1) return -1;
    if (payload_len > 0 && payload) {
        if (send_all(fd, payload, payload_len) != (ssize_t)payload_len) return -1;
    }
    return 0;
}

static int recv_msg(int fd, uint8_t *type_out, char **payload_out, uint32_t *payload_len_out) {
    uint32_t n_net;
    if (recv_all(fd, &n_net, 4) != 4) return -1;
    uint32_t n = ntohl(n_net);
    if (n == 0) return -1;
    uint8_t type;
    if (recv_all(fd, &type, 1) != 1) return -1;
    uint32_t payload_len = n - 1;
    char *payload = NULL;
    if (payload_len) {
        payload = malloc(payload_len + 1);
        if (!payload) return -1;
        if (recv_all(fd, payload, payload_len) != (ssize_t)payload_len) { free(payload); return -1; }
        payload[payload_len] = '\0';
    }
    *type_out = type;
    *payload_out = payload;
    *payload_len_out = payload_len;
    return 0;
}

/* ---------- helpers to parse/build binary fields ---------- */
static uint32_t read_u32_be(const char *buf) {
    uint32_t v;
    memcpy(&v, buf, 4);
    return ntohl(v);
}

/* ---------- local tasks handling ---------- */
static void clear_tasks_locked() {
    task_t *cur = tasks_head;
    while (cur) { task_t *tmp = cur; cur = cur->next; free(tmp); }
    tasks_head = NULL;
}

static void add_task_local(task_t *t) {
    pthread_mutex_lock(&tasks_mutex);
    t->next = tasks_head;
    tasks_head = t;
    pthread_mutex_unlock(&tasks_mutex);
}

static void upsert_task_from_payload(const char *p, uint32_t len) {
    // parse single task: [u32 id_len][id][u32 title_len][title][u8 done][u32 version][u32 updated_by_len][updated_by][u32 updated_at][u32 due_at]
    const char *cur = p;
    if (len < 4) return;
    if ((size_t)(cur - p) > len) return;

    uint32_t id_len;
    memcpy(&id_len, cur, 4); id_len = ntohl(id_len); cur += 4; len -= 4;
    if (id_len > 1024 || id_len > len) return;
    char idbuf[64] = {0};
    memcpy(idbuf, cur, id_len < sizeof(idbuf)-1 ? id_len : sizeof(idbuf)-1); cur += id_len; len -= id_len;

    if (len < 4) return;
    uint32_t title_len;
    memcpy(&title_len, cur, 4); title_len = ntohl(title_len); cur += 4; len -= 4;
    if (title_len > 4096 || title_len > len) return;
    char titlebuf[256] = {0};
    memcpy(titlebuf, cur, title_len < sizeof(titlebuf)-1 ? title_len : sizeof(titlebuf)-1); cur += title_len; len -= title_len;

    if (len < 1) return;
    uint8_t done = *(uint8_t*)cur; cur += 1; len -= 1;

    if (len < 4) return;
    uint32_t version;
    memcpy(&version, cur, 4); version = ntohl(version); cur += 4; len -= 4;

    // NEW: read updated_by
    if (len < 4) return;
    uint32_t updby_len;
    memcpy(&updby_len, cur, 4); updby_len = ntohl(updby_len); cur += 4; len -= 4;
    if (updby_len > 1024 || updby_len > len) return;
    char updby[128] = {0};
    if (updby_len > 0) {
        memcpy(updby, cur, updby_len < sizeof(updby)-1 ? updby_len : sizeof(updby)-1);
        cur += updby_len; len -= updby_len;
    }

    if (len < 4) return;
    uint32_t updated_at;
    memcpy(&updated_at, cur, 4); updated_at = ntohl(updated_at); cur += 4; len -= 4;

    if (len < 4) return;
    uint32_t due_at;
    memcpy(&due_at, cur, 4); due_at = ntohl(due_at); cur += 4; len -= 4;

    // upsert into linked list
    pthread_mutex_lock(&tasks_mutex);
    task_t *curt = tasks_head, *found = NULL;
    while (curt) {
        if (strcmp(curt->id, idbuf) == 0) { found = curt; break; }
        curt = curt->next;
    }
    if (found) {
        strncpy(found->title, titlebuf, sizeof(found->title)-1);
        found->done = done ? 1 : 0;
        found->version = (int)version;
        found->updated_at = (long)updated_at;
        found->due_at = (long)due_at;
        strncpy(found->updated_by, updby, sizeof(found->updated_by)-1);
    } else {
        task_t *n = calloc(1, sizeof(task_t));
        strncpy(n->id, idbuf, sizeof(n->id)-1);
        strncpy(n->title, titlebuf, sizeof(n->title)-1);
        n->done = done ? 1 : 0;
        n->version = (int)version;
        n->updated_at = (long)updated_at;
        n->due_at = (long)due_at;
        strncpy(n->updated_by, updby, sizeof(n->updated_by)-1);
        n->next = tasks_head;
        tasks_head = n;
    }
    pthread_mutex_unlock(&tasks_mutex);
}

static void load_tasks_from_sync_payload(const char *p, uint32_t len) {
    // payload: [u32 count] then count * task
    const char *cur = p;
    if (len < 4) return;
    uint32_t count;
    memcpy(&count, cur, 4); count = ntohl(count); cur += 4; len -= 4;
    pthread_mutex_lock(&tasks_mutex);
    clear_tasks_locked();
    for (uint32_t i = 0; i < count; ++i) {
        if (len < 4) break;
        uint32_t id_len;
        memcpy(&id_len, cur, 4); id_len = ntohl(id_len); cur += 4; len -= 4;
        if (id_len > len) break;
        char idbuf[64] = {0}; memcpy(idbuf, cur, id_len < sizeof(idbuf)-1 ? id_len : sizeof(idbuf)-1); cur += id_len; len -= id_len;

        if (len < 4) break;
        uint32_t title_len;
        memcpy(&title_len, cur, 4); title_len = ntohl(title_len); cur += 4; len -= 4;
        if (title_len > len) break;
        char titlebuf[256] = {0}; memcpy(titlebuf, cur, title_len < sizeof(titlebuf)-1 ? title_len : sizeof(titlebuf)-1); cur += title_len; len -= title_len;

        if (len < 1) break;
        uint8_t done = *(uint8_t*)cur; cur += 1; len -= 1;

        if (len < 4) break;
        uint32_t version;
        memcpy(&version, cur, 4); version = ntohl(version); cur += 4; len -= 4;

        // NEW: updated_by
        if (len < 4) break;
        uint32_t updby_len;
        memcpy(&updby_len, cur, 4); updby_len = ntohl(updby_len); cur += 4; len -= 4;
        if (updby_len > len) break;
        char updby[128] = {0};
        if (updby_len > 0) {
            memcpy(updby, cur, updby_len < sizeof(updby)-1 ? updby_len : sizeof(updby)-1);
            cur += updby_len; len -= updby_len;
        }

        if (len < 4) break;
        uint32_t updated_at;
        memcpy(&updated_at, cur, 4); updated_at = ntohl(updated_at); cur += 4; len -= 4;

        if (len < 4) break;
        uint32_t due_at;
        memcpy(&due_at, cur, 4); due_at = ntohl(due_at); cur += 4; len -= 4;

        task_t *n = calloc(1, sizeof(task_t));
        strncpy(n->id, idbuf, sizeof(n->id)-1);
        strncpy(n->title, titlebuf, sizeof(n->title)-1);
        n->done = done ? 1 : 0;
        n->version = (int)version;
        n->updated_at = (long)updated_at;
        n->due_at = (long)due_at;
        strncpy(n->updated_by, updby, sizeof(n->updated_by)-1);
        n->next = tasks_head;
        tasks_head = n;
    }
    pthread_mutex_unlock(&tasks_mutex);
}

/* ---------- UI helpers ---------- */
static void format_epoch(long epoch, char *buf, size_t bufsz) {
    if (epoch <= 0) { buf[0] = '\0'; return; }
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, bufsz, "%Y-%m-%d %H:%M", &tm);
}

static int count_tasks_locked() {
    int count = 0;
    task_t *cur = tasks_head;
    while (cur) { count++; cur = cur->next; }
    return count;
}
static int count_tasks() { int c; pthread_mutex_lock(&tasks_mutex); c = count_tasks_locked(); pthread_mutex_unlock(&tasks_mutex); return c; }

/* NEW: find task by title (first match) */
static int get_task_by_title(const char *title, char *idbuf, size_t idbuflen,
                             char *titlebuf, size_t titlebuflen,
                             int *done, long *due_at, int *due_present) {
    int found = 0;
    pthread_mutex_lock(&tasks_mutex);
    task_t *cur = tasks_head;
    while (cur) {
        if (strcmp(cur->title, title) == 0) {
            if (idbuf)    strncpy(idbuf, cur->id,    idbuflen-1);
            if (titlebuf) strncpy(titlebuf, cur->title, titlebuflen-1);
            if (done)     *done = cur->done;
            if (due_at)   *due_at = cur->due_at;
            if (due_present) *due_present = (cur->due_at != 0);
            found = 1;
            break;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&tasks_mutex);
    return found;
}

static int get_task_info_by_index(int idx, char *idbuf, size_t idbuflen, char *titlebuf, size_t titlebuflen, int *done, long *due_at, int *due_present) {
    int i = 0, found = 0;
    pthread_mutex_lock(&tasks_mutex);
    task_t *cur = tasks_head;
    while (cur) {
        if (i == idx) {
            if (idbuf) strncpy(idbuf, cur->id, idbuflen-1);
            if (titlebuf) strncpy(titlebuf, cur->title, titlebuflen-1);
            if (done) *done = cur->done;
            if (due_at) *due_at = cur->due_at;
            if (due_present) *due_present = (cur->due_at != 0);
            found = 1;
            break;
        }
        i++; cur = cur->next;
    }
    pthread_mutex_unlock(&tasks_mutex);
    return found;
}

static WINDOW *make_window(int h, int w, int y, int x) {
    WINDOW *win = newwin(h, w, y, x);
    if (!win) {
        endwin();
        fprintf(stderr, "Failed to create window\n");
        exit(1);
    }
    return win;
}

/* forward so toast helpers can call it */
static void draw_ui(void);

static void *toast_thread_fn(void *arg) {
    char *m = arg;
    pthread_mutex_lock(&toast_mutex);
    strncpy(toast_msg, m, sizeof(toast_msg)-1);
    toast_msg[sizeof(toast_msg)-1] = '\0';
    toast_until = time(NULL) + 3; // show for 3 seconds
    pthread_mutex_unlock(&toast_mutex);
    draw_ui();
    free(m);
    sleep(3);
    pthread_mutex_lock(&toast_mutex);
    toast_msg[0] = '\0';
    toast_until = 0;
    pthread_mutex_unlock(&toast_mutex);
    draw_ui();
    return NULL;
}

static void show_toast_nonblocking(const char *msg) {
    char *buf = strdup(msg);
    pthread_t t;
    pthread_create(&t, NULL, toast_thread_fn, buf);
    pthread_detach(t);
}

static void draw_ui() {
    int h = LINES - 4;
    int w = COLS;

    werase(mainwin);
    box(mainwin, 0, 0);

    char titlebuf[256];
    snprintf(titlebuf, sizeof(titlebuf),
             " Shared To-Do (user: %s) ",
             my_username[0] ? my_username : "anonymous");
    mvwprintw(mainwin, 0, 2, "%s", titlebuf);

    pthread_mutex_lock(&tasks_mutex);

    int row = 1;
    int idx = 0;
    int maxrows = h - 2;

    task_t *cur = tasks_head;

    while (cur && row <= maxrows) {

        char duebuf[32] = "";
        if (cur->due_at)
            format_epoch(cur->due_at, duebuf, sizeof(duebuf));

        char updatedbuf[64] = "";
        if (cur->updated_at)
            format_epoch(cur->updated_at, updatedbuf, sizeof(updatedbuf));

        char line_full[512];

        // Only selected task expands
        if (idx == selected_index && show_info) {
            snprintf(line_full, sizeof(line_full),
                "%s [%c] %s  (by: %s @ %s) %s",
                cur->id,
                cur->done ? 'x' : ' ',
                cur->title,
                cur->updated_by[0] ? cur->updated_by : "unknown",
                updatedbuf[0] ? updatedbuf : "--",
                duebuf[0] ? duebuf : ""
            );
        } else {
            snprintf(line_full, sizeof(line_full),
                "%s [%c] %s  %s",
                cur->id,
                cur->done ? 'x' : ' ',
                cur->title,
                duebuf[0] ? duebuf : ""
            );
        }

        // Prevent overflow
        char line[512];
        int maxw = w - 4;

        if ((int)strlen(line_full) > maxw) {
            strncpy(line, line_full, maxw - 3);
            line[maxw - 3] = '.';
            line[maxw - 2] = '.';
            line[maxw - 1] = '.';
            line[maxw] = '\0';
        } else {
            strcpy(line, line_full);
        }

        // highlight selected
        if (idx == selected_index) {
            wattron(mainwin, A_REVERSE);
            mvwprintw(mainwin, row, 2, "%s", line);
            wattroff(mainwin, A_REVERSE);
        } else {
            mvwprintw(mainwin, row, 2, "%s", line);
        }

        row++;
        idx++;
        cur = cur->next;
    }

    pthread_mutex_unlock(&tasks_mutex);

    wrefresh(mainwin);

    werase(statuswin);
    box(statuswin, 0, 0);

    const char *controls =
        " Controls: up/down=select | a=add | t=toggle | d=delete | i=info | q=quit ";
    mvwprintw(statuswin, 1, 2, "%.*s", COLS - 4, controls);

    pthread_mutex_lock(&toast_mutex);
    // NEW: show reminder persistent prompt (line 2)
    if (reminder_pending) {
        mvwprintw(statuswin, 2, 2, "Reminder: \"%.*s\" completed? (Y/N)",
                  COLS - 20, reminder_task_title);
    }
    // Toast on line 3 (if any)
    if (toast_msg[0] != '\0') {
        mvwprintw(statuswin, 3, 2, "%.*s", COLS - 4, toast_msg);
    }
    pthread_mutex_unlock(&toast_mutex);

    wrefresh(statuswin);
}

/* ---------- network recv thread ---------- */
static void *recv_thread(void *arg) {
    (void)arg;
    while (1) {
        uint8_t type;
        char *payload = NULL;
        uint32_t payload_len = 0;
        if (recv_msg(sockfd, &type, &payload, &payload_len) < 0) {
            move(LINES - 2, 1);
            clrtoeol();
            mvprintw(LINES - 2, 1, "Disconnected from server.");
            refresh();
            break;
        }
        if (type == MSG_SYNC_FULL) {
            if (payload) load_tasks_from_sync_payload(payload, payload_len);
            int c = count_tasks();
            if (selected_index >= c) selected_index = c > 0 ? c - 1 : 0;
            draw_ui();
        } else if (type == MSG_ADD_TASK || type == MSG_UPDATE_TASK) {
            if (payload) upsert_task_from_payload(payload, payload_len);
            int c = count_tasks();
            if (selected_index >= c) selected_index = c > 0 ? c - 1 : 0;
            draw_ui();
        } else if (type == MSG_DELETE_TASK) {
            // payload is [u32 id_len][id]
            if (payload && payload_len >= 4) {
                const char *cur = payload;
                uint32_t id_len;
                memcpy(&id_len, cur, 4); id_len = ntohl(id_len); cur += 4;
                if (id_len <= payload_len - 4) {
                    char idbuf[64] = {0};
                    memcpy(idbuf, cur, id_len < sizeof(idbuf)-1 ? id_len : sizeof(idbuf)-1);
                    // delete locally
                    pthread_mutex_lock(&tasks_mutex);
                    task_t **p = &tasks_head;
                    while (*p) {
                        if (strcmp((*p)->id, idbuf) == 0) {
                            task_t *tmp = *p;
                            *p = tmp->next;
                            free(tmp);
                            break;
                        }
                        p = &(*p)->next;
                    }
                    pthread_mutex_unlock(&tasks_mutex);
                    int c = count_tasks();
                    if (selected_index >= c) selected_index = c > 0 ? c - 1 : 0;
                    draw_ui();
                }
            }
        } else if (type == MSG_TOAST) {
            if (payload && payload_len >= 8) {
                const char *cur = payload;
                const char *end = payload + payload_len;
                uint32_t actor_len;
                memcpy(&actor_len, cur, 4); actor_len = ntohl(actor_len); cur += 4;
                if (cur + actor_len > end) { if (payload) free(payload); continue; }
                char actor[128] = {0};
                memcpy(actor, cur, actor_len < sizeof(actor)-1 ? actor_len : sizeof(actor)-1); cur += actor_len;
                if (cur + 4 > end) { if (payload) free(payload); continue; }
                uint32_t title_len;
                memcpy(&title_len, cur, 4); title_len = ntohl(title_len); cur += 4;
                if (cur + title_len > end) { if (payload) free(payload); continue; }
                char title[256] = {0};
                memcpy(title, cur, title_len < sizeof(title)-1 ? title_len : sizeof(title)-1);

                char msgbuf[512];
                snprintf(msgbuf, sizeof(msgbuf), "%s marked \"%s\" as done.", actor, title);
                show_toast_nonblocking(msgbuf);
            }

        } else if (type == MSG_REMINDER) {
            // NEW: persistent reminder instead of 3s toast
            if (payload && payload_len >= 4) {
                const char *cur = payload;
                const char *end = payload + payload_len;
                uint32_t tlen;
                memcpy(&tlen, cur, 4); tlen = ntohl(tlen); cur += 4;
                if (cur + tlen <= end) {
                    char title[256] = {0};
                    memcpy(title, cur, tlen < sizeof(title)-1 ? tlen : sizeof(title)-1);

                    pthread_mutex_lock(&toast_mutex);
                    reminder_pending = 1;
                    strncpy(reminder_task_title, title, sizeof(reminder_task_title)-1);
                    pthread_mutex_unlock(&toast_mutex);

                    draw_ui();
                }
            }
        } else if (type == MSG_AUTH_RESP) {
            // not expected here after initial auth, ignore
        } else if (type == MSG_ERROR) {
            // optional: display error in toast
            if (payload && payload_len > 0) {
                char buf[512];
                snprintf(buf, sizeof(buf), "Error: %.*s", payload_len, payload);
                show_toast_nonblocking(buf);
            }
        }
        if (payload) free(payload);
    }
    return NULL;
}

/* ---------- building outbound payloads ---------- */
static void build_and_send_auth(const char *op, const char *username, const char *password) {
    uint32_t op_len = (uint32_t)strlen(op);
    uint32_t ulen = (uint32_t)strlen(username);
    uint32_t plen = (uint32_t)strlen(password);
    uint32_t payload_len = 4 + op_len + 4 + ulen + 4 + plen;
    char *buf = malloc(payload_len);
    char *cur = buf;
    uint32_t t;

    t = htonl(op_len); memcpy(cur, &t, 4); cur += 4;
    memcpy(cur, op, op_len); cur += op_len;
    t = htonl(ulen); memcpy(cur, &t, 4); cur += 4;
    memcpy(cur, username, ulen); cur += ulen;
    t = htonl(plen); memcpy(cur, &t, 4); cur += 4;
    memcpy(cur, password, plen); cur += plen;

    send_msg(sockfd, MSG_AUTH, buf, payload_len);
    free(buf);
}

static int wait_for_auth_response() {
    uint8_t type;
    char *payload = NULL;
    uint32_t payload_len = 0;
    if (recv_msg(sockfd, &type, &payload, &payload_len) < 0) return 0;
    int ok = 0;
    if (type == MSG_AUTH_RESP && payload_len >= 1) {
        ok = payload[0] ? 1 : 0;
    }
    if (payload) free(payload);
    return ok;
}

static void send_add_request(const char *title, long due_at) {
    uint32_t tlen = (uint32_t)strlen(title);
    uint8_t due_present = due_at > 0 ? 1 : 0;
    uint32_t payload_len = 4 + tlen + 1 + 4;
    char *buf = malloc(payload_len);
    char *cur = buf;
    uint32_t tmp;
    tmp = htonl(tlen); memcpy(cur, &tmp, 4); cur+=4;
    memcpy(cur, title, tlen); cur += tlen;
    *cur++ = due_present ? 1 : 0;
    tmp = htonl((uint32_t)due_at); memcpy(cur, &tmp, 4); cur += 4;
    send_msg(sockfd, MSG_ADD_TASK, buf, payload_len);
    free(buf);
}

static void send_update_request_by_id(const char *id_str, const char *title, int done, int due_present, long due_at) {
    uint32_t idlen = (uint32_t)strlen(id_str);
    uint32_t tlen = (uint32_t)strlen(title);
    uint32_t payload_len = 4 + idlen + 4 + tlen + 1 + 1 + 4;
    char *buf = malloc(payload_len);
    char *cur = buf;
    uint32_t tmp;
    tmp = htonl(idlen); memcpy(cur, &tmp, 4); cur+=4;
    memcpy(cur, id_str, idlen); cur += idlen;
    tmp = htonl(tlen); memcpy(cur, &tmp, 4); cur+=4;
    memcpy(cur, title, tlen); cur += tlen;
    *cur++ = done ? 1 : 0;
    *cur++ = due_present ? 1 : 0;
    tmp = htonl((uint32_t)due_at); memcpy(cur, &tmp, 4); cur += 4;
    send_msg(sockfd, MSG_UPDATE_TASK, buf, payload_len);
    free(buf);
}

static void send_delete_request_by_id(const char *id_str) {
    uint32_t idlen = (uint32_t)strlen(id_str);
    uint32_t payload_len = 4 + idlen;
    char *buf = malloc(payload_len);
    char *cur = buf;
    uint32_t tmp = htonl(idlen); memcpy(cur, &tmp, 4); cur += 4;
    memcpy(cur, id_str, idlen); cur += idlen;
    send_msg(sockfd, MSG_DELETE_TASK, buf, payload_len);
    free(buf);
}

/* ---------- prompts & helpers ---------- */
static long parse_date_to_epoch(const char *s) {
    if (!s || !*s) return 0;
    int year=0, mon=0, day=0, hour=0, min=0;
    int parts = sscanf(s, "%d-%d-%d %d:%d", &year, &mon, &day, &hour, &min);
    if (parts < 3) return 0;
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    if (parts >= 5) { tm.tm_hour = hour; tm.tm_min = min; } else { tm.tm_hour = 0; tm.tm_min = 0; }
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return 0;
    return (long)t;
}

static void prompt_add() {
    echo(); curs_set(1);
    char titlebuf[256] = "";
    char duebuf[64] = "";
    move(LINES - 2, 1); clrtoeol();
    mvprintw(LINES - 2, 1, "Title: ");
    refresh();
    getnstr(titlebuf, sizeof(titlebuf)-1);
    if (strlen(titlebuf) == 0) { noecho(); curs_set(0); draw_ui(); return; }
    move(LINES - 2, 1); clrtoeol();
    mvprintw(LINES - 2, 1, "Due (YYYY-MM-DD or YYYY-MM-DD HH:MM) [optional]: ");
    refresh();
    getnstr(duebuf, sizeof(duebuf)-1);
    noecho(); curs_set(0);
    long due_epoch = parse_date_to_epoch(duebuf);
    send_add_request(titlebuf, due_epoch);
    draw_ui();
}

static void toggle_selected() {
    char idbuf[64] = "";
    char title[256] = "";
    int done = 0;
    long due_at = 0;
    int due_present = 0;
    if (!get_task_info_by_index(selected_index, idbuf, sizeof(idbuf), title, sizeof(title), &done, &due_at, &due_present)) return;
    send_update_request_by_id(idbuf, title, done ? 0 : 1, due_present, due_at);
}

static void delete_selected() {
    char idbuf[64] = "";
    if (!get_task_info_by_index(selected_index, idbuf, sizeof(idbuf), NULL, 0, NULL, NULL, NULL)) return;
    send_delete_request_by_id(idbuf);
}

/* ---------- auth ---------- */
static int send_auth_request(const char *op, const char *username, const char *password) {
    build_and_send_auth(op, username, password);
    return wait_for_auth_response();
}

/* ---------- main ---------- */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server-ip> <port>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    struct sockaddr_in servaddr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
        perror("inet_pton");
        return 1;
    }

    if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        return 1;
    }

    printf("Sign in or signup? (type 'signin' or 'signup'): ");
    char op[16];
    if (!fgets(op, sizeof(op), stdin)) { close(sockfd); return 1; }
    op[strcspn(op, "\n")] = '\0';
    if (strcmp(op, "signin") != 0 && strcmp(op, "signup") != 0) {
        printf("Invalid op. Exiting.\n");
        close(sockfd);
        return 1;
    }
    char username[128], password[128];
    printf("Username: ");
    if (!fgets(username, sizeof(username), stdin)) { close(sockfd); return 1; }
    username[strcspn(username, "\n")] = '\0';
    printf("Password: ");
    if (!fgets(password, sizeof(password), stdin)) { close(sockfd); return 1; }
    password[strcspn(password, "\n")] = '\0';

    if (!send_auth_request(op, username, password)) {
        printf("Authentication failed.\n");
        close(sockfd);
        return 1;
    }
    strncpy(my_username, username, sizeof(my_username)-1);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    int h = LINES - 4;
    int w = COLS;
    if (mainwin) delwin(mainwin);
    if (statuswin) delwin(statuswin);
    mainwin = newwin(h, w, 0, 0);
    statuswin = newwin(4, w, h, 0);
    keypad(mainwin, TRUE);

    pthread_t rt;
    pthread_create(&rt, NULL, recv_thread, NULL);

    draw_ui();

    while (1) {
        int ch = getch();

        // If a reminder is pending, intercept Y/N
        if (reminder_pending && (ch == 'y' || ch == 'Y' || ch == 'n' || ch == 'N')) {
            if (ch == 'y' || ch == 'Y') {
                // Mark the reminded task as done (if found)
                char idbuf[64] = "";
                char titlebuf[256] = "";
                int done = 0;
                long due_at = 0;
                int due_present = 0;

                pthread_mutex_lock(&toast_mutex);
                char local_title[256];
                strncpy(local_title, reminder_task_title, sizeof(local_title)-1);
                local_title[sizeof(local_title)-1] = '\0';
                pthread_mutex_unlock(&toast_mutex);

                if (get_task_by_title(local_title, idbuf, sizeof(idbuf),
                                      titlebuf, sizeof(titlebuf),
                                      &done, &due_at, &due_present)) {
                    // Force done=1
                    send_update_request_by_id(idbuf, titlebuf, 1, due_present, due_at);
                } else {
                    show_toast_nonblocking("Reminder task not found (maybe already deleted).");
                }
            }

            // Clear reminder in both cases (Y or N)
            pthread_mutex_lock(&toast_mutex);
            reminder_pending = 0;
            reminder_task_title[0] = '\0';
            pthread_mutex_unlock(&toast_mutex);

            draw_ui();
            continue;
        }

        if (ch == 'q') break;
        else if (ch == 'a') { prompt_add(); draw_ui(); }
        else if (ch == 'd') { delete_selected(); draw_ui(); }
        else if (ch == 't' || ch == '\n' || ch == KEY_ENTER) { toggle_selected(); draw_ui(); }
        else if (ch == 'i') {
            show_info = !show_info;  // toggle
            draw_ui();
        }
        else if (ch == KEY_UP) {
            if (selected_index > 0) selected_index--;
            draw_ui();
        } else if (ch == KEY_DOWN) {
            int c = count_tasks();
            if (selected_index < c - 1) selected_index++;
            draw_ui();
        }
    }

    endwin();
    close(sockfd);
    return 0;
}
