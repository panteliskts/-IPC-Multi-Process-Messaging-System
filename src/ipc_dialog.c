#include "ipc_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static SharedMemory* shm_ptr = NULL;
static sem_t* mutex = NULL;
static int shm_fd = -1;

/* Initialize shared memory and semaphore */
int init_shared_memory(void) {
    int created = 0;
    
    /* Try to open existing shared memory */
    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    
    if (shm_fd == -1) {
        /* Create new shared memory */
        shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) {
            perror("shm_open");
            return -1;
        }
        created = 1;
        
        /* Set size */
        if (ftruncate(shm_fd, sizeof(SharedMemory)) == -1) {
            perror("ftruncate");
            close(shm_fd);
            shm_unlink(SHM_NAME);
            return -1;
        }
    }
    
    /* Map shared memory */
    shm_ptr = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE,
                   MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        if (created) {
            shm_unlink(SHM_NAME);
        }
        return -1;
    }
    
    /* Open or create semaphore */
    mutex = sem_open(SEM_MUTEX_NAME, O_CREAT, 0666, 1);
    if (mutex == SEM_FAILED) {
        perror("sem_open");
        munmap(shm_ptr, sizeof(SharedMemory));
        close(shm_fd);
        if (created) {
            shm_unlink(SHM_NAME);
        }
        return -1;
    }
    
    /* Initialize shared memory if newly created */
    if (created) {
        sem_wait(mutex);
        memset(shm_ptr, 0, sizeof(SharedMemory));
        shm_ptr->initialized = 1;
        sem_post(mutex);
    }
    
    return 0;
}

/* Cleanup shared memory */
void cleanup_shared_memory(void) {
    if (shm_ptr != NULL) {
        munmap(shm_ptr, sizeof(SharedMemory));
        shm_ptr = NULL;
    }
    
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
    }
    
    if (mutex != NULL) {
        sem_close(mutex);
        mutex = NULL;
    }
}

/* Get shared memory pointer */
SharedMemory* get_shared_memory(void) {
    return shm_ptr;
}

/* Get mutex semaphore */
sem_t* get_mutex(void) {
    return mutex;
}

/* Create or join a dialog */
int create_or_join_dialog(int dialog_id) {
    pid_t my_pid = getpid();
    int dialog_idx = -1;
    int i, j;
    
    if (shm_ptr == NULL || mutex == NULL) {
        fprintf(stderr, "Shared memory not initialized\n");
        return -1;
    }
    
    sem_wait(mutex);
    
    /* Find existing dialog or free slot */
    for (i = 0; i < MAX_DIALOGS; i++) {
        if (shm_ptr->dialogs[i].active && 
            shm_ptr->dialogs[i].dialog_id == dialog_id) {
            dialog_idx = i;
            break;
        }
    }
    
    if (dialog_idx == -1) {
        /* Create new dialog */
        for (i = 0; i < MAX_DIALOGS; i++) {
            if (!shm_ptr->dialogs[i].active) {
                dialog_idx = i;
                shm_ptr->dialogs[i].dialog_id = dialog_id;
                shm_ptr->dialogs[i].process_count = 0;
                shm_ptr->dialogs[i].active = 1;
                shm_ptr->dialogs[i].terminated = 0;
                break;
            }
        }
        
        if (dialog_idx == -1) {
            fprintf(stderr, "Maximum number of dialogs reached\n");
            sem_post(mutex);
            return -1;
        }
    }
    
    /* Check if process already in dialog */
    for (j = 0; j < shm_ptr->dialogs[dialog_idx].process_count; j++) {
        if (shm_ptr->dialogs[dialog_idx].processes[j] == my_pid) {
            sem_post(mutex);
            return 0; /* Already in dialog */
        }
    }
    
    /* Add process to dialog */
    if (shm_ptr->dialogs[dialog_idx].process_count < MAX_PROCESSES_PER_DIALOG) {
        shm_ptr->dialogs[dialog_idx].processes[shm_ptr->dialogs[dialog_idx].process_count] = my_pid;
        shm_ptr->dialogs[dialog_idx].process_count++;
        printf("Joined dialog %d (Total processes: %d)\n", 
               dialog_id, shm_ptr->dialogs[dialog_idx].process_count);
    } else {
        fprintf(stderr, "Maximum processes per dialog reached\n");
        sem_post(mutex);
        return -1;
    }
    
    sem_post(mutex);
    return 0;
}

/* Send a message to a dialog */
int send_message(int dialog_id, const char* payload) {
    pid_t my_pid = getpid();
    int dialog_idx = -1;
    int msg_idx = -1;
    int i;
    
    if (shm_ptr == NULL || mutex == NULL) {
        fprintf(stderr, "Shared memory not initialized\n");
        return -1;
    }
    
    sem_wait(mutex);
    
    /* Find dialog */
    for (i = 0; i < MAX_DIALOGS; i++) {
        if (shm_ptr->dialogs[i].active && 
            shm_ptr->dialogs[i].dialog_id == dialog_id) {
            dialog_idx = i;
            break;
        }
    }
    
    if (dialog_idx == -1) {
        fprintf(stderr, "Dialog %d not found\n", dialog_id);
        sem_post(mutex);
        return -1;
    }
    
    /* Find free message slot */
    for (i = 0; i < MAX_MESSAGES; i++) {
        if (!shm_ptr->messages[i].valid) {
            msg_idx = i;
            break;
        }
    }
    
    if (msg_idx == -1) {
        fprintf(stderr, "No free message slots available\n");
        sem_post(mutex);
        return -1;
    }
    
    /* Create message */
    shm_ptr->messages[msg_idx].dialog_id = dialog_id;
    shm_ptr->messages[msg_idx].sender_pid = my_pid;
    strncpy(shm_ptr->messages[msg_idx].payload, payload, MAX_MESSAGE_LENGTH - 1);
    shm_ptr->messages[msg_idx].payload[MAX_MESSAGE_LENGTH - 1] = '\0';
    shm_ptr->messages[msg_idx].read_count = 0;
    memset(shm_ptr->messages[msg_idx].readers, 0, sizeof(shm_ptr->messages[msg_idx].readers));
    shm_ptr->messages[msg_idx].valid = 1;
    
    /* Check for TERMINATE message */
    if (strcmp(payload, "TERMINATE") == 0) {
        shm_ptr->dialogs[dialog_idx].terminated = 1;
    }
    
    printf("Message sent to dialog %d: %s\n", dialog_id, payload);
    
    sem_post(mutex);
    return 0;
}

/* Receive messages for a dialog */
int receive_messages(int dialog_id, pid_t my_pid) {
    int dialog_idx = -1;
    int i;
    int found_terminate = 0;
    int num_processes;
    
    if (shm_ptr == NULL || mutex == NULL) {
        fprintf(stderr, "Shared memory not initialized\n");
        return -1;
    }
    
    sem_wait(mutex);
    
    /* Find dialog */
    for (i = 0; i < MAX_DIALOGS; i++) {
        if (shm_ptr->dialogs[i].active && 
            shm_ptr->dialogs[i].dialog_id == dialog_id) {
            dialog_idx = i;
            break;
        }
    }
    
    if (dialog_idx == -1) {
        sem_post(mutex);
        return 0;
    }
    
    num_processes = shm_ptr->dialogs[dialog_idx].process_count;
    
    /* Check for new messages */
    for (i = 0; i < MAX_MESSAGES; i++) {
        if (shm_ptr->messages[i].valid && 
            shm_ptr->messages[i].dialog_id == dialog_id &&
            shm_ptr->messages[i].sender_pid != my_pid) {
            
            /* Check if already read by this process */
            int already_read = 0;
            int j;
            for (j = 0; j < shm_ptr->messages[i].read_count; j++) {
                if (shm_ptr->messages[i].readers[j] == my_pid) {
                    already_read = 1;
                    break;
                }
            }
            
            if (!already_read) {
                printf("[Dialog %d] Message from PID %d: %s\n", 
                       dialog_id, shm_ptr->messages[i].sender_pid,
                       shm_ptr->messages[i].payload);
                
                /* Check for TERMINATE */
                if (strcmp(shm_ptr->messages[i].payload, "TERMINATE") == 0) {
                    found_terminate = 1;
                }
                
                /* Mark as read by this process */
                shm_ptr->messages[i].readers[shm_ptr->messages[i].read_count] = my_pid;
                shm_ptr->messages[i].read_count++;
                
                /* Calculate expected reads (all processes except sender) */
                int expected_reads = num_processes - 1;
                
                /* If all processes have read, mark as invalid */
                if (shm_ptr->messages[i].read_count >= expected_reads) {
                    shm_ptr->messages[i].valid = 0;
                }
            }
        }
    }
    
    sem_post(mutex);
    return found_terminate;
}

/* Leave a dialog */
int leave_dialog(int dialog_id, pid_t my_pid) {
    int dialog_idx = -1;
    int i, j, k;
    int all_dialogs_inactive = 1;
    
    if (shm_ptr == NULL || mutex == NULL) {
        return -1;
    }
    
    sem_wait(mutex);
    
    /* Find dialog */
    for (i = 0; i < MAX_DIALOGS; i++) {
        if (shm_ptr->dialogs[i].active && 
            shm_ptr->dialogs[i].dialog_id == dialog_id) {
            dialog_idx = i;
            break;
        }
    }
    
    if (dialog_idx != -1) {
        /* Remove process from dialog */
        for (j = 0; j < shm_ptr->dialogs[dialog_idx].process_count; j++) {
            if (shm_ptr->dialogs[dialog_idx].processes[j] == my_pid) {
                /* Shift remaining processes */
                for (k = j; k < shm_ptr->dialogs[dialog_idx].process_count - 1; k++) {
                    shm_ptr->dialogs[dialog_idx].processes[k] = 
                        shm_ptr->dialogs[dialog_idx].processes[k + 1];
                }
                shm_ptr->dialogs[dialog_idx].process_count--;
                break;
            }
        }
        
        /* If no more processes, deactivate dialog */
        if (shm_ptr->dialogs[dialog_idx].process_count == 0) {
            shm_ptr->dialogs[dialog_idx].active = 0;
            
            /* Clean up messages for this dialog */
            for (i = 0; i < MAX_MESSAGES; i++) {
                if (shm_ptr->messages[i].dialog_id == dialog_id) {
                    shm_ptr->messages[i].valid = 0;
                }
            }
        }
    }
    
    /* Check if all dialogs are inactive */
    for (i = 0; i < MAX_DIALOGS; i++) {
        if (shm_ptr->dialogs[i].active) {
            all_dialogs_inactive = 0;
            break;
        }
    }
    
    /* If all dialogs inactive, cleanup shared memory */
    if (all_dialogs_inactive) {
        printf("All dialogs terminated. Cleaning up shared memory...\n");
        shm_ptr->initialized = 0;
        sem_post(mutex);
        
        /* Unlink shared memory and semaphore */
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_MUTEX_NAME);
        return 1; /* Signal that cleanup was done */
    }
    
    sem_post(mutex);
    return 0;
}
