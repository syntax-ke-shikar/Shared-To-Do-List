# 📋 SharedTodo — Collaborative Terminal Task Manager

A **real-time, multi-user to-do list** built on a custom binary TCP protocol, backed by SQLite, and rendered in a full-screen ncurses TUI. Multiple users on different machines share one live task list — changes propagate to everyone instantly.

---

## Features

- **Real-time sync** — every add, toggle, or delete is broadcast to all connected clients immediately
- **Multi-user auth** — sign up or sign in; sessions are exclusive (same user can't be logged in twice)
- **Due date reminders** — server sends a persistent in-app reminder when a task is due within 24 hours
- **Live toast notifications** — when another user marks a task done, you see a 3-second pop-up
- **Info panel** — press `i` to reveal who last updated a task and when
- **Binary framing protocol** — compact, length-prefixed messages (no JSON overhead)
- **SQLite persistence** — tasks and users survive server restarts

---

## Project Structure

```
.
├── server.c       # Multi-threaded TCP server + SQLite + reminder daemon
├── client.c       # ncurses TUI client with recv thread
├── tasks.db       # Auto-created on first server run
└── users.db       # Auto-created on first server run
```

---

## Requirements

| Dependency | Purpose |
|---|---|
| `gcc` | Compile both programs |
| `libsqlite3-dev` | Server database |
| `libncurses-dev` | Client TUI |
| `pthreads` | Concurrency (included with glibc) |

Install on Debian/Ubuntu:

```bash
sudo apt install gcc libsqlite3-dev libncurses-dev
```

---

## Building

```bash
# Compile the server
gcc -o server server.c -lsqlite3 -lpthread

# Compile the client
gcc -o client client.c -lncurses -lpthread
```

---

## Running

**Start the server** (default port 8765):

```bash
./server
```

**Connect a client:**

```bash
./client 127.0.0.1 8765
```

You'll be prompted to sign in or sign up before the TUI loads.

To connect from another machine, replace `127.0.0.1` with the server's IP address.

---

## TUI Controls

| Key | Action |
|---|---|
| `↑` / `↓` | Move selection |
| `a` | Add a new task (with optional due date) |
| `t` or `Enter` | Toggle selected task done/undone |
| `d` | Delete selected task |
| `i` | Toggle info panel (shows updater + timestamp) |
| `q` | Quit |
| `Y` / `N` | Respond to a due-date reminder |

---

## Protocol Overview

All messages use a simple length-prefixed binary frame:

```
[ 4 bytes: total length (big-endian) ][ 1 byte: message type ][ payload ]
```

### Message Types

| Hex | Name | Direction | Description |
|---|---|---|---|
| `0x01` | `MSG_SYNC_FULL` | Server → Client | Full task list dump |
| `0x02` | `MSG_ADD_TASK` | Client → Server | Add a new task |
| `0x03` | `MSG_UPDATE_TASK` | Client ↔ Server | Update title / done / due |
| `0x04` | `MSG_DELETE_TASK` | Client ↔ Server | Delete by ID |
| `0x10` | `MSG_AUTH` | Client → Server | Sign in or sign up |
| `0x11` | `MSG_AUTH_RESP` | Server → Client | Auth result (1 byte: ok/fail) |
| `0x12` | `MSG_TOAST` | Server → Client | "User X marked Y as done" |
| `0x13` | `MSG_REMINDER` | Server → Client | Due-date reminder prompt |
| `0xFF` | `MSG_ERROR` | Server → Client | Error string |

---

## Database Schema

**`tasks.db`**

```sql
CREATE TABLE tasks (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    title         TEXT    NOT NULL,
    done          INTEGER NOT NULL DEFAULT 0,
    version       INTEGER NOT NULL DEFAULT 1,
    updated_by    TEXT,
    updated_at    INTEGER,           -- Unix timestamp
    due_at        INTEGER,           -- Unix timestamp (nullable)
    reminder_sent INTEGER NOT NULL DEFAULT 0
);
```

**`users.db`**

```sql
CREATE TABLE users (
    username TEXT PRIMARY KEY,
    password TEXT NOT NULL
);
```

> ⚠️ Passwords are stored in plain text. Add hashing (e.g. bcrypt) before any real-world deployment.

---

## Architecture Notes

### Server

- One thread per connected client (`client_thread`)
- A background `reminder_thread` wakes every 60 seconds and checks for tasks due within 24 hours; sends `MSG_REMINDER` to the task creator and sets `reminder_sent = 1` so it fires only once
- All DB access is serialized behind `db_mutex`; the client list is protected by `clients_mutex`
- On any task mutation the server re-sends `MSG_SYNC_FULL` to every connected client (simple and correct; optimize with delta updates if needed at scale)

### Client

- `recv_thread` runs in the background and drives all incoming message handling and UI redraws
- Toast notifications spawn a short-lived detached thread that sleeps 3 seconds then clears itself
- Reminders are **persistent** — they stay on screen until the user presses `Y` (mark done) or `N` (dismiss)
- The `tasks_mutex` guards the local linked list; `toast_mutex` guards toast/reminder state

---

## Known Limitations / TODO

- Plain-text password storage — integrate a proper hash (bcrypt / Argon2)
- No TLS — traffic is unencrypted; add OpenSSL wrapping for use over the internet
- `MSG_SYNC_FULL` on every write is fine for small lists; switch to incremental `MSG_UPDATE_TASK` broadcasts for large datasets
- No pagination — very long task lists will overflow the terminal window
- Reminder targets only `updated_by` (last editor); consider notifying all users instead

---

## License

MIT — do whatever you want, just don't blame me if your tasks don't get done.# Shared-To-Do-List
