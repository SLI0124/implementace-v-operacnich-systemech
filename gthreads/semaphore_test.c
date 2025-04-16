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

// Define shared resources that threads will compete for
#define BUFFER_SIZE 2 // 2 will consume and produce at the same time

// Shared resources
gt_semaphore_t mutex;           // Binary semaphore for mutual exclusion
gt_semaphore_t items_count;     // Counting semaphore for number of items in buffer

// Simple buffer implementation using individual variables
int shared_buffer[BUFFER_SIZE];  // Shared buffer array
int buffer_in = 0;               // Index for next write
int buffer_out = 0;              // Index for next read

// Use extern declaration - the actual definition is in gthr_params.c
extern struct thread_data thread_params[MAX_G_THREADS];

// Function to print the current state of the buffer
void print_buffer(int items) {
    printf("Buffer state: [");
    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (i < items) {
            int idx = (buffer_out + i) % BUFFER_SIZE;
            printf("%d", shared_buffer[idx]);
        } else {
            printf("_");
        }
        
        if (i < BUFFER_SIZE - 1) {
            printf(", ");
        }
    }
    printf("]\n");
}

// Producer thread function
void producer_thread(void) {
    // Each thread knows its index based on creation order
    static int thread_index = 0;
    int my_index = thread_index++;
    
    struct thread_data *data = &thread_params[my_index];
    const char* priority_label = data->label;
    int id = data->id;
    
    // Run indefinitely
    int item = 1;
    while (true) {
        printf("Producer %s (ID:%d) waiting to produce item %d\n", priority_label, id, item);
        
        // Check if buffer is full by trying to get mutex
        gt_sem_wait(&mutex);
        
        // Check if buffer is full
        int current_items;
        gt_sem_wait(&items_count);
        current_items = (buffer_in - buffer_out + BUFFER_SIZE) % BUFFER_SIZE;
        gt_sem_post(&items_count);
        
        if (current_items >= BUFFER_SIZE) {
            // Buffer is full, release mutex and wait
            printf("Producer %s (ID:%d) found buffer FULL, waiting\n", priority_label, id);
            gt_sem_post(&mutex);
            
            // Sleep a bit before trying again
            gt_uninterruptible_nanosleep(0, 100000000);  // 100ms
            continue;
        }
        
        // Critical section - produce an item
        printf("Producer %s (ID:%d) entered critical section for item %d\n", priority_label, id, item);
        
        // Simulate some work in the critical section
        gt_uninterruptible_nanosleep(0, 100000000);  // 100ms
        
        // Add the item to the buffer
        shared_buffer[buffer_in] = id * 1000 + item;
        buffer_in = (buffer_in + 1) % BUFFER_SIZE;
        
        // Update items count
        gt_sem_wait(&items_count);
        current_items = (buffer_in - buffer_out + BUFFER_SIZE) % BUFFER_SIZE;
        gt_sem_post(&items_count);
        
        // Print buffer status while still in critical section
        printf("Producer %s (ID:%d) added item %d (value: %d)\n", 
               priority_label, id, item, id * 1000 + item);
        print_buffer(current_items);
        
        // Exit critical section
        gt_sem_post(&mutex);
        
        // Wait a bit before producing next item
        gt_uninterruptible_nanosleep(0, 50000000);  // 50ms
        
        // Increment item counter (will keep growing)
        item++;
    }
}

// Consumer thread function
void consumer_thread(void) {
    // Each thread knows its index based on creation order
    static int thread_index = 0;
    int my_index = thread_index++;
    
    struct thread_data *data = &thread_params[my_index];
    const char* priority_label = data->label;
    int id = data->id;
    
    // Run indefinitely
    int i = 1;
    while (true) {
        printf("Consumer %s (ID:%d) waiting to consume item %d\n", priority_label, id, i);
        
        // Check if buffer is empty by trying to get mutex
        gt_sem_wait(&mutex);
        
        // Check if buffer is empty
        int current_items;
        gt_sem_wait(&items_count);
        current_items = (buffer_in - buffer_out + BUFFER_SIZE) % BUFFER_SIZE;
        gt_sem_post(&items_count);
        
        if (current_items <= 0) {
            // Buffer is empty, release mutex and wait
            printf("Consumer %s (ID:%d) found buffer EMPTY, waiting\n", priority_label, id);
            gt_sem_post(&mutex);
            
            // Sleep a bit before trying again
            gt_uninterruptible_nanosleep(0, 100000000);  // 100ms
            continue;
        }
        
        // Critical section - consume an item
        printf("Consumer %s (ID:%d) entered critical section for item %d\n", priority_label, id, i);
        
        // Simulate some work in the critical section
        gt_uninterruptible_nanosleep(0, 150000000);  // 150ms
        
        // Remove the item from the buffer
        int item = shared_buffer[buffer_out];
        buffer_out = (buffer_out + 1) % BUFFER_SIZE;
        
        // Update items count
        gt_sem_wait(&items_count);
        current_items = (buffer_in - buffer_out + BUFFER_SIZE) % BUFFER_SIZE;
        gt_sem_post(&items_count);
        
        // Print buffer status while still in critical section
        printf("Consumer %s (ID:%d) removed item with value: %d\n", priority_label, id, item);
        print_buffer(current_items);
        
        // Exit critical section
        gt_sem_post(&mutex);
        
        // Wait a bit before consuming next item
        gt_uninterruptible_nanosleep(0, 80000000);  // 80ms
        
        // Increment item counter
        i++;
    }
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
    
    // Initialize shared buffer
    buffer_in = 0;
    buffer_out = 0;
    for (int i = 0; i < BUFFER_SIZE; i++) {
        shared_buffer[i] = 0;
    }
    
    // Initialize semaphores
    gt_sem_init(&mutex, 1);      // Binary semaphore for mutual exclusion
    gt_sem_init(&items_count, 1); // Semaphore for safely accessing items count
    
    printf("Semaphore test started with producer-consumer problem\n");
    printf("Buffer size: %d\n", BUFFER_SIZE);
    printf("Using only 2 semaphores for demonstration\n");
    printf("-----------------------------------------------\n");
    
    // Create producer threads with different priorities
    thread_params[0].id = 1;
    thread_params[0].priority = 1;  // High priority
    thread_params[0].tickets = 50;
    thread_params[0].label = "HIGH";
    gt_create(producer_thread, &thread_params[0]);
    
    thread_params[1].id = 2;
    thread_params[1].priority = 5;  // Medium priority
    thread_params[1].tickets = 30;
    thread_params[1].label = "MED";
    gt_create(producer_thread, &thread_params[1]);
    
    // Create consumer threads with different priorities
    thread_params[2].id = 3;
    thread_params[2].priority = 3;  // Medium-high priority
    thread_params[2].tickets = 40;
    thread_params[2].label = "MED-HIGH";
    gt_create(consumer_thread, &thread_params[2]);
    
    thread_params[3].id = 4;
    thread_params[3].priority = 8;  // Low priority
    thread_params[3].tickets = 10;
    thread_params[3].label = "LOW";
    gt_create(consumer_thread, &thread_params[3]);
    
    // Wait for all threads to complete
    gt_return(1);
    
    return 0;
}