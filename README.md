# IPC Multi-Process Messaging System

A multi-process messaging system in C using **POSIX shared memory** and **named semaphores**, allowing multiple independent processes to join named dialogs and exchange messages with guaranteed exactly-once delivery per participant.

> Built as part of the Operating Systems course (K22) at the National and Kapodistrian University of Athens (NKUA), 2024–2025.

---

## Overview

Multiple processes can run simultaneously and communicate through a shared memory region mapped into each process's address space. A named semaphore provides mutual exclusion, ensuring data integrity when multiple processes read and write concurrently.

Key properties:
- **Exactly-once delivery** — every message is delivered to every participant exactly once, then automatically discarded
- **Non-blocking I/O** — processes poll for new messages using `select()` with a 100ms timeout, so the terminal stays responsive while waiting
- **Graceful shutdown** — `SIGINT` (Ctrl+C) is handled cleanly, releasing all shared resources
- **Automatic cleanup** — shared memory and semaphores are unlinked when the last participant exits

---

## Architecture

### Data Structures

**`Message`** — a single message in the shared message pool:
```c
typedef struct {
    int dialog_id;       // Which dialog this message belongs to
    pid_t sender_pid;    // PID of the sender
    char payload[256];   // Message content (max 256 chars)
    int read_count;      // How many participants have read it
    pid_t readers[10];   // PIDs of participants who have read it
    int valid;           // Whether this slot is active
} Message;
```

**`Dialog`** — a named conversation between processes:
```c
typedef struct {
    int dialog_id;         // Unique dialog identifier
    int process_count;     // Number of active participants
    pid_t processes[10];   // PIDs of participants
    int active;            // Whether dialog is active
    int terminated;        // Set when TERMINATE is sent
} Dialog;
```

**`SharedMemory`** — the full shared memory layout:
```c
typedef struct {
    Dialog dialogs[10];    // Up to 10 concurrent dialogs
    Message messages[50];  // Up to 50 messages in flight
    int initialized;       // Init flag
} SharedMemory;
```

### System Limits

| Parameter | Value |
|-----------|-------|
| Max concurrent dialogs | 10 |
| Max participants per dialog | 10 |
| Max messages in flight | 50 |
| Max message length | 256 characters |

---

## IPC Mechanisms

### POSIX Shared Memory

| Call | Purpose |
|------|---------|
| `shm_open()` | Create or open the shared memory segment |
| `ftruncate()` | Set the size of the segment |
| `mmap()` | Map the segment into the process address space |
| `munmap()` | Unmap on exit |
| `shm_unlink()` | Delete the segment when the last process exits |

Shared memory name: `/ipc_dialog_shm`

### POSIX Named Semaphore

A single mutex semaphore protects all critical sections:

| Call | Purpose |
|------|---------|
| `sem_open()` | Create or open the semaphore |
| `sem_wait()` | Acquire the lock |
| `sem_post()` | Release the lock |
| `sem_close()` | Close on exit |
| `sem_unlink()` | Delete when the last process exits |

Semaphore name: `/ipc_dialog_mutex`

---

## How It Works

### Joining a Dialog
1. Lock mutex
2. Search for an existing dialog with the given ID
3. If none exists, create a new one
4. Add the calling process's PID to the participant list
5. Release mutex

### Sending a Message
1. Lock mutex
2. Find the dialog
3. Find a free message slot
4. Store the message with sender PID and metadata
5. If the message is `"TERMINATE"`, set the termination flag
6. Release mutex

### Receiving Messages
1. Lock mutex
2. Scan all messages belonging to this dialog
3. For each unread message (PID not in `readers[]`):
   - Display it to the user
   - Add PID to `readers[]`, increment `read_count`
4. If `read_count` equals participant count → mark message invalid (auto-cleanup)
5. Release mutex

### Leaving a Dialog
1. Lock mutex
2. Remove PID from participant list
3. If last participant: deactivate dialog, clear its messages
4. If all dialogs inactive: `shm_unlink` + `sem_unlink`
5. Release mutex

---

## Commands

| Command | Description |
|---------|-------------|
| `JOIN <id>` | Join or create a dialog with the given numeric ID |
| `SEND <message>` | Send a message to everyone in the current dialog |
| `TERMINATE` | Signal all participants to end the dialog |
| `QUIT` / `EXIT` | Leave the dialog and exit cleanly |
| `HELP` | Show available commands |

---

## Usage Example

Open two terminals and run the program in each:

**Terminal 1:**
```bash
./dialog
> JOIN 1
Joined dialog 1.
[Dialog 1] > Hello from process 1!
[Dialog 1] Message from PID 5679: Hi back!
```

**Terminal 2:**
```bash
./dialog
> JOIN 1
Joined dialog 1.
[Dialog 1] Message from PID 5678: Hello from process 1!
[Dialog 1] > Hi back!
```

---

## Build & Run

### Requirements
- Linux (POSIX-compliant)
- GCC
- Make

### Build
```bash
make
```

### Run
```bash
./dialog
```

### Clean
```bash
make clean
```

> **Note:** If the program crashes without cleanup, stale shared memory or semaphores may persist. Remove them manually:
> ```bash
> rm /dev/shm/ipc_dialog_shm
> rm /dev/shm/sem.ipc_dialog_mutex
> ```

---

## Project Structure

```
.
├── include/
│   └── ipc_dialog.h      # Struct definitions and function declarations
├── src/
│   ├── main.c            # User interface, command loop, signal handling
│   └── ipc_dialog.c      # Core IPC logic (join, send, receive, leave)
├── Makefile
└── README.md
```

---

## Synchronization Design

All shared memory operations are wrapped in a mutex critical section:

```c
sem_wait(mutex);
// --- critical section ---
sem_post(mutex);
```

**Message delivery guarantee:** Each message slot has a `readers[]` array and a `read_count` counter. A process only receives a message if its PID is not already in `readers[]`. Once all participants have read a message, it is invalidated and the slot is freed for reuse.

**No busy-waiting:** The main loop uses `select()` with a 100ms timeout on stdin. This allows the process to check for new messages periodically without blocking user input or spinning the CPU.

---
