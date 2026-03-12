#include "ipc_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

static volatile int running = 1;
static volatile int in_dialog = 0;
static int current_dialog_id = -1;
static pid_t my_pid;
static pthread_t receiver_thread;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

void signal_handler(int signum) {
    if (signum == SIGINT) {
        printf("\nReceived SIGINT. Exiting immediately...\n");
        
        /* Leave dialog if active */
        if (in_dialog && current_dialog_id != -1) {
            leave_dialog(current_dialog_id, my_pid);
        }
        
        /* Cleanup shared memory */
        cleanup_shared_memory();
        
        /* Exit immediately */
        _exit(0);
    }
}

/* Thread function for receiving messages */
void* message_receiver(void* arg) {
    (void)arg; /* Unused parameter */
    
    while (running) {
        pthread_mutex_lock(&state_mutex);
        int dialog_active = in_dialog;
        int dialog_id = current_dialog_id;
        pthread_mutex_unlock(&state_mutex);
        
        if (dialog_active) {
            int terminate = receive_messages(dialog_id, my_pid);
            if (terminate) {
                printf("\n[Dialog %d] Received TERMINATE message. Exiting dialog...\n", 
                       dialog_id);
                printf("> ");
                fflush(stdout);
                
                leave_dialog(dialog_id, my_pid);
                
                pthread_mutex_lock(&state_mutex);
                in_dialog = 0;
                current_dialog_id = -1;
                pthread_mutex_unlock(&state_mutex);
            }
        }
        
        /* Sleep to avoid busy waiting */
        usleep(50000); /* 50ms */
    }
    
    return NULL;
}

void print_usage(void) {
    printf("\n=== IPC Dialog System (Threaded) ===\n");
    printf("Commands:\n");
    printf("  JOIN <dialog_id>  - Join or create a dialog with the specified ID\n");
    printf("  SEND <message>    - Send a message to the current dialog\n");
    printf("  TERMINATE         - Send TERMINATE message and exit dialog\n");
    printf("  EXIT              - Exit the program\n");
    printf("  HELP              - Show this help message\n");
    printf("====================================\n\n");
}

int main(int argc __attribute__((unused)), char* argv[] __attribute__((unused))) {
    my_pid = getpid();
    char input[MAX_MESSAGE_LENGTH];
    
    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    
    printf("Process started with PID: %d\n", my_pid);
    
    /* Initialize shared memory */
    if (init_shared_memory() != 0) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        return 1;
    }
    
    /* Create receiver thread */
    if (pthread_create(&receiver_thread, NULL, message_receiver, NULL) != 0) {
        perror("pthread_create");
        cleanup_shared_memory();
        return 1;
    }
    
    print_usage();
    
    /* Main loop - handles user input */
    while (running) {
        pthread_mutex_lock(&state_mutex);
        int dialog_active = in_dialog;
        int dialog_id = current_dialog_id;
        pthread_mutex_unlock(&state_mutex);
        
        if (dialog_active) {
            printf("[Dialog %d] > ", dialog_id);
        } else {
            printf("> ");
        }
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        /* Remove newline */
        input[strcspn(input, "\n")] = '\0';
        
        /* Skip empty input */
        if (strlen(input) == 0) {
            continue;
        }
        
        /* Re-check state after input */
        pthread_mutex_lock(&state_mutex);
        dialog_active = in_dialog;
        dialog_id = current_dialog_id;
        pthread_mutex_unlock(&state_mutex);
        
        /* Parse command */
        if (strncmp(input, "JOIN ", 5) == 0) {
            int new_dialog_id = atoi(input + 5);
            if (new_dialog_id <= 0) {
                printf("Invalid dialog ID. Please use a positive integer.\n");
                continue;
            }
            
            if (dialog_active) {
                printf("Already in dialog %d. Please TERMINATE or EXIT first.\n", 
                       dialog_id);
                continue;
            }
            
            if (create_or_join_dialog(new_dialog_id) == 0) {
                pthread_mutex_lock(&state_mutex);
                current_dialog_id = new_dialog_id;
                in_dialog = 1;
                pthread_mutex_unlock(&state_mutex);
                printf("Successfully joined dialog %d\n", new_dialog_id);
            } else {
                printf("Failed to join dialog %d\n", new_dialog_id);
            }
        }
        else if (strcmp(input, "TERMINATE") == 0) {
            if (!dialog_active) {
                printf("Not in any dialog. Use JOIN <dialog_id> first.\n");
                continue;
            }
            
            send_message(dialog_id, "TERMINATE");
            printf("Sent TERMINATE message. Exiting dialog %d...\n", dialog_id);
            leave_dialog(dialog_id, my_pid);
            
            pthread_mutex_lock(&state_mutex);
            in_dialog = 0;
            current_dialog_id = -1;
            pthread_mutex_unlock(&state_mutex);
        }
        else if (strncmp(input, "SEND ", 5) == 0) {
            if (!dialog_active) {
                printf("Not in any dialog. Use JOIN <dialog_id> first.\n");
                continue;
            }
            
            const char* message = input + 5;
            if (strlen(message) == 0) {
                printf("Empty message. Please provide a message to send.\n");
                continue;
            }
            
            send_message(dialog_id, message);
        }
        else if (strcmp(input, "EXIT") == 0) {
            if (dialog_active) {
                printf("Leaving dialog %d...\n", dialog_id);
                leave_dialog(dialog_id, my_pid);
            }
            running = 0;
        }
        else if (strcmp(input, "HELP") == 0) {
            print_usage();
        }
        else {
            /* If in dialog, treat as message to send */
            if (dialog_active) {
                send_message(dialog_id, input);
            } else {
                printf("Unknown command: %s\n", input);
                printf("Type HELP for available commands.\n");
            }
        }
    }
    
    /* Stop receiver thread */
    running = 0;
    pthread_join(receiver_thread, NULL);
    
    /* Cleanup */
    pthread_mutex_lock(&state_mutex);
    if (in_dialog) {
        leave_dialog(current_dialog_id, my_pid);
    }
    pthread_mutex_unlock(&state_mutex);
    
    pthread_mutex_destroy(&state_mutex);
    cleanup_shared_memory();
    printf("Process %d terminated.\n", my_pid);
    
    return 0;
}
