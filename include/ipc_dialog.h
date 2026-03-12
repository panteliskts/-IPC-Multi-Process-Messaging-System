#ifndef IPC_DIALOG_H
#define IPC_DIALOG_H

#include <sys/types.h>
#include <semaphore.h>

#define MAX_DIALOGS 10
#define MAX_PROCESSES_PER_DIALOG 10
#define MAX_MESSAGES 50
#define MAX_MESSAGE_LENGTH 256
#define SHM_NAME "/ipc_dialog_shm"
#define SEM_MUTEX_NAME "/ipc_dialog_mutex"

/* Message structure */
typedef struct {
    int dialog_id;              /* Dialog identifier */
    pid_t sender_pid;           /* PID of sender process */
    char payload[MAX_MESSAGE_LENGTH]; /* Message content */
    int read_count;             /* Number of processes that have read this message */
    pid_t readers[MAX_PROCESSES_PER_DIALOG]; /* PIDs of processes that have read this message */
    int valid;                  /* 1 if message is valid, 0 if slot is free */
} Message;

/* Dialog structure */
typedef struct {
    int dialog_id;              /* Dialog identifier */
    int process_count;          /* Number of processes in this dialog */
    pid_t processes[MAX_PROCESSES_PER_DIALOG]; /* PIDs of participating processes */
    int active;                 /* 1 if dialog is active, 0 if free */
    int terminated;             /* 1 if TERMINATE was sent */
} Dialog;

/* Shared memory structure */
typedef struct {
    Dialog dialogs[MAX_DIALOGS];
    Message messages[MAX_MESSAGES];
    int initialized;            /* 1 if shared memory is initialized */
} SharedMemory;

/* Function prototypes */
int init_shared_memory(void);
void cleanup_shared_memory(void);
SharedMemory* get_shared_memory(void);
sem_t* get_mutex(void);

int create_or_join_dialog(int dialog_id);
int send_message(int dialog_id, const char* payload);
int receive_messages(int dialog_id, pid_t my_pid);
int leave_dialog(int dialog_id, pid_t my_pid);

#endif /* IPC_DIALOG_H */
