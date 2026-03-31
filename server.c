
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <time.h>

#define PORT 8765
#define BACKLOG 10

                                         // Message types
#define MSG_SYNC_FULL   0x01 
#define MSG_ADD_TASK    0x02
#define MSG_UPDATE_TASK 0x03
#define MSG_DELETE_TASK 0x04
#define MSG_ERROR       0xFF
#define MSG_AUTH        0x10
#define MSG_AUTH_RESP   0x11
#define MSG_TOAST       0x12
#define MSG_REMINDER    0x13   

typedef struct client_node {                                      //head if a linked list containing all connected clients
    int fd;
    char username[128];
    struct client_node *next;
} client_node_t;

static client_node_t *clients_head = NULL;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;         //mutex is used for proctection from concurrent access

static sqlite3 *tasks_db = NULL;
static sqlite3 *users_db = NULL;
static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;

//framing helpers
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

static int send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len) { 
    uint32_t n = htonl(payload_len + 1);
    if (send_all(fd, &n, 4) != 4) return -1;
    if (send_all(fd, &type, 1) != 1) return -1;
    if (payload_len > 0 && payload) {
        if (send_all(fd, payload, payload_len) != (ssize_t)payload_len) return -1;
    }
    return 0;
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

//client list management 
static void add_client_fd_with_user(int fd, const char *username) {   
    client_node_t *n = malloc(sizeof(*n));
    n->fd = fd;
    strncpy(n->username, username ? username : "", sizeof(n->username)-1);
    n->username[sizeof(n->username)-1] = '\0';
    n->next = NULL;
    pthread_mutex_lock(&clients_mutex);
    n->next = clients_head;
    clients_head = n;
    pthread_mutex_unlock(&clients_mutex);
}

static void remove_client_fd(int fd) {
    pthread_mutex_lock(&clients_mutex);
    client_node_t **p = &clients_head;
    while (*p) {
        if ((*p)->fd == fd) {
            client_node_t *tmp = *p;
            *p = tmp->next;
            close(tmp->fd);
            free(tmp);
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void broadcast_msg(uint8_t type, const char *payload, uint32_t payload_len) {
    pthread_mutex_lock(&clients_mutex);
    client_node_t *cur = clients_head;
    while (cur) {
        if (send_msg(cur->fd, type, payload, payload_len) < 0) {
            int fd = cur->fd;
            client_node_t *next = cur->next;
            cur = next;
            remove_client_fd(fd);
            continue;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}

// helper: is a username already logged in? 
static int is_user_logged_in(const char *username) {
    if (!username || !*username) return 0;
    int found = 0;
    pthread_mutex_lock(&clients_mutex);
    client_node_t *cur = clients_head;
    while (cur) {
        if (strcmp(cur->username, username) == 0) { found = 1; break; }
        cur = cur->next;
    }
    pthread_mutex_unlock(&clients_mutex);
    return found;
}

//send to everyone except one fd 
static void send_to_all_except(int except_fd, uint8_t type, const char *payload, uint32_t payload_len) {
    pthread_mutex_lock(&clients_mutex);
    client_node_t *cur = clients_head;
    while (cur) {
        if (cur->fd != except_fd) {
            if (send_msg(cur->fd, type, payload, payload_len) < 0) {
                int fd = cur->fd;
                client_node_t *next = cur->next;
                cur = next;
                remove_client_fd(fd);
                continue;
            }
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}

// helper to send to a specific user (if connected) 
static void send_to_user(const char *username, uint8_t type, const char *payload, uint32_t payload_len) {
    if (!username || !*username) return;

    pthread_mutex_lock(&clients_mutex);
    client_node_t *cur = clients_head;
    while (cur) {
        if (strcmp(cur->username, username) == 0) {
            /* ignore send errors here; connection cleanup is handled elsewhere */
            send_msg(cur->fd, type, payload, payload_len);
            break;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}
/*DATABASE INITIALIZATION*/
/* ---------- DB init ---------- */
static int init_dbs(const char *tasks_path, const char *users_path) {
    if (sqlite3_open(tasks_path, &tasks_db) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open tasks error: %s\n", sqlite3_errmsg(tasks_db));
        return -1;
    }
    if (sqlite3_open(users_path, &users_db) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open users error: %s\n", sqlite3_errmsg(users_db));
        return -1;
    }
    const char *sql_tasks = "CREATE TABLE IF NOT EXISTS tasks ("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "title TEXT NOT NULL,"
                            "done INTEGER NOT NULL DEFAULT 0,"
                            "version INTEGER NOT NULL DEFAULT 1,"
                            "updated_by TEXT,"
                            "updated_at INTEGER,"
                            "due_at INTEGER"
                            ");";
    char *err = NULL;
    if (sqlite3_exec(tasks_db, sql_tasks, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_exec create tasks error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }

    /* NEW: ensure reminder_sent column exists (ignore error if already there) */
    const char *sql_tasks_rem = "ALTER TABLE tasks ADD COLUMN reminder_sent INTEGER NOT NULL DEFAULT 0;";
    if (sqlite3_exec(tasks_db, sql_tasks_rem, NULL, NULL, &err) != SQLITE_OK) {
        /* likely "duplicate column name: reminder_sent" on existing DB; safe to ignore */
        sqlite3_free(err);
    }

    const char *sql_users = "CREATE TABLE IF NOT EXISTS users ("
                            "username TEXT PRIMARY KEY,"
                            "password TEXT NOT NULL"
                            ");";
    if (sqlite3_exec(users_db, sql_users, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_exec create users error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* ---------- task -> payload helpers ---------- */
static char *task_from_stmt_payload(sqlite3_stmt *stmt, uint32_t *out_len) {
    int id = sqlite3_column_int(stmt, 0);
    const unsigned char *title = sqlite3_column_text(stmt, 1);
    int done = sqlite3_column_int(stmt, 2);
    int version = sqlite3_column_int(stmt, 3);
    const unsigned char *updated_by = sqlite3_column_text(stmt, 4);
    int updated_at = sqlite3_column_int(stmt, 5);
    int due_at = sqlite3_column_type(stmt, 6) == SQLITE_NULL ? 0 : sqlite3_column_int(stmt, 6);

    char idstr[64];
    snprintf(idstr, sizeof(idstr), "%d", id);
    uint32_t idlen = (uint32_t)strlen(idstr);
    uint32_t titlelen = title ? (uint32_t)strlen((const char*)title) : 0;
    uint32_t updby_len = updated_by ? (uint32_t)strlen((const char*)updated_by) : 0;

    // payload fields:
    // [u32 idlen][id][u32 titlelen][title][u8 done][u32 version][u32 updated_by_len][updated_by][u32 updated_at][u32 due_at]
    uint32_t payload_len = 4 + idlen + 4 + titlelen + 1 + 4 + 4 + updby_len + 4 + 4;
    char *buf = malloc(payload_len);
    char *cur = buf;
    uint32_t tmp;

    tmp = htonl(idlen); memcpy(cur, &tmp, 4); cur += 4;
    memcpy(cur, idstr, idlen); cur += idlen;

    tmp = htonl(titlelen); memcpy(cur, &tmp, 4); cur += 4;
    if (titlelen) { memcpy(cur, title, titlelen); cur += titlelen; }

    *cur++ = done ? 1 : 0;

    tmp = htonl((uint32_t)version); memcpy(cur, &tmp, 4); cur += 4;

    // NEW: updated_by length + bytes
    tmp = htonl(updby_len); memcpy(cur, &tmp, 4); cur += 4;
    if (updby_len) { memcpy(cur, updated_by, updby_len); cur += updby_len; }

    tmp = htonl((uint32_t)updated_at); memcpy(cur, &tmp, 4); cur += 4;
    tmp = htonl((uint32_t)due_at); memcpy(cur, &tmp, 4); cur += 4;

    if (out_len) *out_len = payload_len;
    return buf;
}

/* send SYNC_FULL to fd: [u32 count] then count * task_payload */
static void send_sync_full(int fd) {
    pthread_mutex_lock(&db_mutex);
    const char *q = "SELECT id, title, done, version, updated_by, updated_at, due_at FROM tasks ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(tasks_db, q, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_mutex);
        return;
    }
    uint32_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) count++;
    sqlite3_finalize(stmt);

    if (count == 0) {
        uint32_t zero = htonl(0);
        send_msg(fd, MSG_SYNC_FULL, &zero, 4);
        pthread_mutex_unlock(&db_mutex);
        return;
    }

    if (sqlite3_prepare_v2(tasks_db, q, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_mutex);
        return;
    }
    uint32_t total_len = 4;
    char **parts = calloc(count, sizeof(char*));
    uint32_t *parts_len = calloc(count, sizeof(uint32_t));
    uint32_t idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < count) {
        parts[idx] = task_from_stmt_payload(stmt, &parts_len[idx]);
        total_len += parts_len[idx];
        idx++;
    }
    sqlite3_finalize(stmt);
    char *buf = malloc(total_len);
    char *cur = buf;
    uint32_t tmp = htonl(count); memcpy(cur, &tmp, 4); cur += 4;
    for (uint32_t i = 0; i < count; ++i) {
        memcpy(cur, parts[i], parts_len[i]);
        cur += parts_len[i];
        free(parts[i]);
    }
    free(parts); free(parts_len);
    pthread_mutex_unlock(&db_mutex);

    send_msg(fd, MSG_SYNC_FULL, buf, total_len);
    free(buf);
}

/* ---------- DB manipulation helpers ---------- */
static char *server_add_task(const char *title, int due_at, const char *username) {
    pthread_mutex_lock(&db_mutex);
    const char *sql = "INSERT INTO tasks(title, done, version, updated_by, updated_at, due_at) VALUES(?,0,1,?,strftime('%s','now'),?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(tasks_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_mutex);
        return NULL;
    }
    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username ? username : "", -1, SQLITE_TRANSIENT);
    if (due_at > 0) sqlite3_bind_int(stmt, 3, due_at);
    else sqlite3_bind_null(stmt, 3);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db_mutex);
        return NULL;
    }
    sqlite3_finalize(stmt);
    long long rowid = sqlite3_last_insert_rowid(tasks_db);

    const char *q2 = "SELECT id, title, done, version, updated_by, updated_at, due_at FROM tasks WHERE id = ?;";
    if (sqlite3_prepare_v2(tasks_db, q2, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_mutex);
        return NULL;
    }
    sqlite3_bind_int64(stmt, 1, rowid);
    char *out = NULL;
    uint32_t out_len = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = task_from_stmt_payload(stmt, &out_len);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_mutex);
    return out; // caller must free
}

static char *server_update_task(const char *id_str, const char *title, int done, int due_at, int due_present, const char *username) {
    int id = atoi(id_str);
    pthread_mutex_lock(&db_mutex);
    const char *sql_base = "UPDATE tasks SET title = ?, done = ?, version = version + 1, updated_by = ?, updated_at = strftime('%s','now')";
    char sql[512];
    strcpy(sql, sql_base);
    if (due_present) strcat(sql, ", due_at = ?");
    strcat(sql, " WHERE id = ?;");
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(tasks_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_mutex);
        return NULL;
    }
    int bind_index = 1;
    sqlite3_bind_text(stmt, bind_index++, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, bind_index++, done);
    sqlite3_bind_text(stmt, bind_index++, username ? username : "", -1, SQLITE_TRANSIENT);
    if (due_present) {
        if (due_at > 0) sqlite3_bind_int(stmt, bind_index++, due_at);
        else sqlite3_bind_null(stmt, bind_index++);
    }
    sqlite3_bind_int(stmt, bind_index++, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db_mutex);
        return NULL;
    }
    sqlite3_finalize(stmt);

    const char *q2 = "SELECT id, title, done, version, updated_by, updated_at, due_at FROM tasks WHERE id = ?;";
    if (sqlite3_prepare_v2(tasks_db, q2, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_mutex);
        return NULL;
    }
    sqlite3_bind_int(stmt, 1, id);
    char *out = NULL;
    uint32_t out_len = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = task_from_stmt_payload(stmt, &out_len);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_mutex);
    return out; // caller must free
}

static int server_delete_task(const char *id_str) {
    int id = atoi(id_str);
    pthread_mutex_lock(&db_mutex);
    const char *sql = "DELETE FROM tasks WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(tasks_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ---------- users helpers (SQL) ---------- */
static int user_exists(const char *username) {
    sqlite3_stmt *stmt = NULL;
    const char *q = "SELECT 1 FROM users WHERE username = ?;";
    if (sqlite3_prepare_v2(users_db, q, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW;
}
static int check_user_password(const char *username, const char *password) {
    sqlite3_stmt *stmt = NULL;
    const char *q = "SELECT password FROM users WHERE username = ?;";
    if (sqlite3_prepare_v2(users_db, q, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int ok = 0;
    if (rc == SQLITE_ROW) {
        const unsigned char *pw = sqlite3_column_text(stmt, 0);
        if (pw && strcmp((const char*)pw, password) == 0) ok = 1;
    }
    sqlite3_finalize(stmt);
    return ok;
}
static int create_user(const char *username, const char *password) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO users(username, password) VALUES(?,?);";
    if (sqlite3_prepare_v2(users_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

/* ---------- client handler ---------- */
static void send_auth_response(int fd, int ok) {
    char b = ok ? 1 : 0;
    send_msg(fd, MSG_AUTH_RESP, &b, 1);
}

static void *client_thread(void *arg) {             

    int fd = (int)(intptr_t)arg;

    uint8_t atype;
    char *payload = NULL;
    uint32_t payload_len = 0;
    if (recv_msg(fd, &atype, &payload, &payload_len) < 0) {
        if (payload) free(payload);
        close(fd);
        return NULL;
    }
    char username[128] = "";
    int auth_ok = 0;
    if (atype == MSG_AUTH && payload && payload_len >= 12) {
        const char *cur = payload;
        const char *end = payload + payload_len;
        if (cur + 4 <= end) {
            uint32_t op_len = ntohl(*(uint32_t*)cur); cur += 4;
            if (cur + op_len <= end) {
                char opbuf[16] = {0};
                memcpy(opbuf, cur, op_len < sizeof(opbuf)-1 ? op_len : sizeof(opbuf)-1);
                cur += op_len;
                if (cur + 4 <= end) {
                    uint32_t ulen = ntohl(*(uint32_t*)cur); cur += 4;
                    if (cur + ulen <= end) {
                        char userbuf[128] = {0};
                        memcpy(userbuf, cur, ulen < sizeof(userbuf)-1 ? ulen : sizeof(userbuf)-1);
                        cur += ulen;
                        if (cur + 4 <= end) {
                            uint32_t plen = ntohl(*(uint32_t*)cur); cur += 4;
                            if (cur + plen <= end) {
                                char pwbuf[128] = {0};
                                memcpy(pwbuf, cur, plen < sizeof(pwbuf)-1 ? plen : sizeof(pwbuf)-1);
                                cur += plen;
                                if (strcmp(opbuf, "signin") == 0) {
                                    if (check_user_password(userbuf, pwbuf)) {
                                        auth_ok = 1; strncpy(username, userbuf, sizeof(username)-1);
                                    }
                                } else if (strcmp(opbuf, "signup") == 0) {
                                    if (!user_exists(userbuf)) {
                                        if (create_user(userbuf, pwbuf)) {
                                            auth_ok = 1; strncpy(username, userbuf, sizeof(username)-1);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (auth_ok && is_user_logged_in(username)) {
        const char *errmsg = "user already in use";
        fprintf(stderr, "Auth attempt rejected for '%s': %s\n", username, errmsg);
        send_auth_response(fd, 0);
        send_msg(fd, MSG_ERROR, errmsg, (uint32_t)strlen(errmsg));
        if (payload) free(payload);
        close(fd);
        return NULL;
    }

    if (payload) free(payload);
    send_auth_response(fd, auth_ok);
    if (!auth_ok) { close(fd); return NULL; }

    add_client_fd_with_user(fd, username);      
    send_sync_full(fd);

    while (1) {
        uint8_t type;
        char *payload2 = NULL;
        uint32_t payload_len2 = 0;
        if (recv_msg(fd, &type, &payload2, &payload_len2) < 0) break;
        if (type == MSG_ADD_TASK) {
            if (payload2 && payload_len2 >= 4) {
                const char *cur = payload2;
                const char *end = payload2 + payload_len2;
                uint32_t tlen = ntohl(*(uint32_t*)cur); cur += 4;
                if (cur + tlen <= end && cur + tlen + 1 + 4 <= end) {
                    char titlebuf[1024] = {0};
                    memcpy(titlebuf, cur, tlen < sizeof(titlebuf)-1 ? tlen : sizeof(titlebuf)-1); cur += tlen;
                    uint8_t due_present = *(uint8_t*)cur; cur += 1;
                    uint32_t due_at = ntohl(*(uint32_t*)cur); cur += 4;
                    char *task_json = server_add_task(titlebuf, due_present ? (int)due_at : 0, username);
                    if (task_json) {
                        free(task_json);
                        pthread_mutex_lock(&clients_mutex);
                        client_node_t *c2 = clients_head;
                        while (c2) {
                            send_sync_full(c2->fd);
                            c2 = c2->next;
                        }
                        pthread_mutex_unlock(&clients_mutex);
                    }
                }
            }
        } else if (type == MSG_UPDATE_TASK) {
            if (payload2 && payload_len2 >= 4) {
                const char *cur = payload2;
                const char *end = payload2 + payload_len2;
                if (cur + 4 > end) { free(payload2); continue; }
                uint32_t idlen = ntohl(*(uint32_t*)cur); cur += 4;
                if (cur + idlen + 4 + 1 + 1 + 4 > end) { free(payload2); continue; }
                char idbuf[64] = {0};
                memcpy(idbuf, cur, idlen < sizeof(idbuf)-1 ? idlen : sizeof(idbuf)-1); cur += idlen;
                uint32_t tlen = ntohl(*(uint32_t*)cur); cur += 4;
                if (tlen > 4096 || cur + tlen + 1 + 1 + 4 > end) { free(payload2); continue; }
                char titlebuf[1024] = {0};
                memcpy(titlebuf, cur, tlen < sizeof(titlebuf)-1 ? tlen : sizeof(titlebuf)-1); cur += tlen;
                uint8_t done = *(uint8_t*)cur; cur += 1;
                uint8_t due_present = *(uint8_t*)cur; cur += 1;
                uint32_t due_at = ntohl(*(uint32_t*)cur); cur += 4;

                int prev_done = 0;
                sqlite3_stmt *stmt = NULL;
                const char *qprev = "SELECT done FROM tasks WHERE id = ?;";
                if (sqlite3_prepare_v2(tasks_db, qprev, -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, atoi(idbuf));
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        prev_done = sqlite3_column_int(stmt, 0);
                    }
                    sqlite3_finalize(stmt);
                }

                char *task_payload = server_update_task(idbuf, titlebuf, done ? 1 : 0, due_at, due_present ? 1 : 0, username);
                if (task_payload) {
                    free(task_payload);

                    if (prev_done == 0 && done == 1) {
                        uint32_t actor_len = (uint32_t)strlen(username);
                        uint32_t title_len2 = (uint32_t)strlen(titlebuf);
                        uint32_t buf_len = 4 + actor_len + 4 + title_len2;
                        char *buf = malloc(buf_len);
                        char *p = buf;
                        uint32_t tmp = htonl(actor_len); memcpy(p, &tmp, 4); p += 4;
                        if (actor_len) { memcpy(p, username, actor_len); p += actor_len; }
                        tmp = htonl(title_len2); memcpy(p, &tmp, 4); p += 4;
                        if (title_len2) { memcpy(p, titlebuf, title_len2); p += title_len2; }

                        send_to_all_except(fd, MSG_TOAST, buf, buf_len);
                        free(buf);
                    }

                    pthread_mutex_lock(&clients_mutex);
                    client_node_t *c2 = clients_head;
                    while (c2) {
                        send_sync_full(c2->fd);
                        c2 = c2->next;
                    }
                    pthread_mutex_unlock(&clients_mutex);
                }
            }
        } else if (type == MSG_DELETE_TASK) {
            if (payload2 && payload_len2 >= 4) {
                const char *cur = payload2;
                uint32_t idlen = ntohl(*(uint32_t*)cur); cur += 4;
                if (idlen <= payload_len2 - 4) {
                    char idbuf[64] = {0};
                    memcpy(idbuf, cur, idlen < sizeof(idbuf)-1 ? idlen : sizeof(idbuf)-1);
                    if (server_delete_task(idbuf) == 0) {
                        pthread_mutex_lock(&clients_mutex);
                        client_node_t *c2 = clients_head;
                        while (c2) {
                            send_msg(c2->fd, MSG_DELETE_TASK, payload2, payload_len2);
                            c2 = c2->next;
                        }
                        pthread_mutex_unlock(&clients_mutex);
                    }
                }
            }
        } else {
            // ignore
        }
        if (payload2) free(payload2);
    }

    remove_client_fd(fd);
    return NULL;
}

/* ---------- reminder thread ---------- */       
static void *reminder_thread(void *arg) {
    (void)arg;

    while (1) {
        time_t now = time(NULL);

        pthread_mutex_lock(&db_mutex);
        const char *q = "SELECT id, title, due_at, updated_by, reminder_sent "
                        "FROM tasks WHERE done = 0 AND due_at IS NOT NULL;";
        sqlite3_stmt *stmt = NULL;

        if (sqlite3_prepare_v2(tasks_db, q, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int id = sqlite3_column_int(stmt, 0);
                const unsigned char *title = sqlite3_column_text(stmt, 1);
                int due_at = sqlite3_column_int(stmt, 2);
                const unsigned char *upd_by = sqlite3_column_text(stmt, 3);
                int reminder_sent = sqlite3_column_int(stmt, 4);

                if (!title || !upd_by) continue;

                time_t diff = (time_t)due_at - now;

                // If not reminded yet, and due in <= 24h but still in the future
                if (!reminder_sent && diff > 0 && diff <= 24 * 60 * 60) {
                    const char *title_str = (const char*)title;
                    const char *user_str  = (const char*)upd_by;

                    uint32_t tlen = (uint32_t)strlen(title_str);
                    uint32_t payload_len = 4 + tlen;
                    char *buf = malloc(payload_len);
                    if (buf) {
                        char *p = buf;
                        uint32_t tmp = htonl(tlen);
                        memcpy(p, &tmp, 4); p += 4;
                        if (tlen) memcpy(p, title_str, tlen);

                        // send reminder to the user who last updated/created the task
                        send_to_user(user_str, MSG_REMINDER, buf, payload_len);
                        free(buf);
                    }

                    sqlite3_stmt *ustmt = NULL;
                    const char *u_sql = "UPDATE tasks SET reminder_sent = 1 WHERE id = ?;";
                    if (sqlite3_prepare_v2(tasks_db, u_sql, -1, &ustmt, NULL) == SQLITE_OK) {
                        sqlite3_bind_int(ustmt, 1, id);
                        sqlite3_step(ustmt);
                        sqlite3_finalize(ustmt);
                    }
                }
            }
            sqlite3_finalize(stmt);
        }
        pthread_mutex_unlock(&db_mutex);

        sleep(60); // check every 60 seconds
    }

    return NULL;
}

int main(int argc, char **argv) {
    if (init_dbs("tasks.db", "users.db") != 0) {
        fprintf(stderr, "Failed to init DBs\n");
        return 1;
    }

    /* NEW: start reminder thread */
    pthread_t rtid;
    pthread_create(&rtid, NULL, reminder_thread, NULL);
    pthread_detach(rtid);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(PORT);
    if (bind(listenfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, BACKLOG) < 0) { perror("listen"); return 1; }

    printf("Server listening on 0.0.0.0:%d\n", PORT);

    while (1) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int connfd = accept(listenfd, (struct sockaddr*)&cli, &clilen);
        if (connfd < 0) {
            perror("accept");
            continue;
        }
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, (void*)(intptr_t)connfd);
        pthread_detach(tid);
    }

    sqlite3_close(tasks_db);
    sqlite3_close(users_db);
    return 0;
}
