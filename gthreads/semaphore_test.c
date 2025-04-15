#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>

#include "gthr.h"

// Shared resources
gt_semaphore_t mutex;           // Binary semaphore (mutex)
gt_semaphore_t resource_slots;  // Counting semaphore for resource slots
char shared_buffer[256];        // Shared resource
int buffer_access_count = 0;    // Counter for accessing the buffer

// Use extern declaration - the actual definition is in gthr_struct.c
extern struct thread_data thread_params[MAX_G_THREADS];

// Thread function for workers
void worker_thread(void) {
    // Each thread knows its index based on creation order
    static int thread_index = 0;
    int my_index = thread_index++;
    
    if (my_index >= MAX_G_THREADS) {
        printf("ERROR: Thread index out of bounds!\n");
        return;
    }
    
    struct thread_data *data = &thread_params[my_index];
    const char* priority_label = data->label;
    int id = data->id;
    
    // Each thread will run multiple cycles
    for (int cycle = 1; cycle <= 5; cycle++) {
        printf("%s priority thread id = %d waiting for resource (cycle %d)\n", 
               priority_label, id, cycle);
        
        // Wait for an available resource slot
        gt_sem_wait(&resource_slots);
        
        // Wait for exclusive access to the shared buffer
        gt_sem_wait(&mutex);
        
        // Critical section start
        printf("%s priority thread id = %d entered critical section (cycle %d)\n", 
               priority_label, id, cycle);
               
        // Access the shared resource
        buffer_access_count++;
        snprintf(shared_buffer, sizeof(shared_buffer), 
                 "Buffer modified by thread %d (priority: %s) - access #%d", 
                 id, priority_label, buffer_access_count);
        
        // Simulate some work in the critical section
        gt_uninterruptible_nanosleep(0, 100000000);  // 100ms
        
        printf("%s priority thread id = %d says: %s\n", 
               priority_label, id, shared_buffer);
               
        printf("%s priority thread id = %d leaving critical section (cycle %d)\n", 
               priority_label, id, cycle);
        // Critical section end
        
        // Release the mutex
        gt_sem_post(&mutex);
        
        // Simulate some processing outside the critical section
        gt_uninterruptible_nanosleep(0, 50000000);  // 50ms
        
        // Release the resource slot
        gt_sem_post(&resource_slots);
        
        // Wait a bit before next cycle
        gt_uninterruptible_nanosleep(0, 20000000);  // 20ms
    }
    
    printf("%s priority thread id = %d completed all cycles\n", 
           priority_label, id);
}

int main(int argc, char *argv[]) {    
    // Set scheduler type based on command-line argument
    if (argc > 1) {
        if (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "--rr") == 0) {
            gt_set_scheduler(GT_SCHED_RR);
            printf("Using Round Robin scheduler\n");
        } else if (strcmp(argv[1], "-p") == 0 || strcmp(argv[1], "--prio") == 0) {
            gt_set_scheduler(GT_SCHED_PRI);
            printf("Using Priority-based scheduler\n");
        } else if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "--lottery") == 0) {
            gt_set_scheduler(GT_SCHED_LS);
            printf("Using Lottery Scheduling\n");
        } else {
            printf("Invalid argument. Use -r for Round Robin, -p for Priority, or -l for Lottery.\n");
            return 1;
        }
    } else {
        gt_set_scheduler(GT_SCHED_PRI);
        printf("Using default Priority-based scheduler\n");
    }
    
    // Initialize gthreads
    gt_init();
    
    // Initialize semaphores
    gt_sem_init(&mutex, 1);              // Binary semaphore for mutual exclusion
    gt_sem_init(&resource_slots, 2);     // Allow two threads to use resources at once
    
    printf("Semaphore test started\n");
    printf("Mutex initialized with value 1\n");
    printf("Resource semaphore initialized with value 2\n");
    
    // Create worker threads with different priorities
    thread_params[0].id = 1;
    thread_params[0].priority = 0;  // Highest priority
    thread_params[0].tickets = 50;  // More tickets for lottery scheduler
    thread_params[0].label = "HIGH";
    gt_create(worker_thread, &thread_params[0]);
    
    thread_params[1].id = 2;
    thread_params[1].priority = 3;  // Medium priority
    thread_params[1].tickets = 30;
    thread_params[1].label = "MED ";
    gt_create(worker_thread, &thread_params[1]);
    
    thread_params[2].id = 3;
    thread_params[2].priority = 5;  // Medium priority
    thread_params[2].tickets = 15;
    thread_params[2].label = "MED ";
    gt_create(worker_thread, &thread_params[2]);
    
    thread_params[3].id = 4;
    thread_params[3].priority = 10; // Lowest priority
    thread_params[3].tickets = 5;
    thread_params[3].label = "LOW ";
    gt_create(worker_thread, &thread_params[3]);
    
    // Wait for all threads to complete
    gt_return(1);
    
    return 0;
}