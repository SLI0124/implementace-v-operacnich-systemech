// Based on https://c9x.me/articles/gthreads/code0.html
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

// Use extern declaration - the actual definition is in gthr_params.c for better organization
extern struct thread_data thread_params[MAX_G_THREADS];

// Thread function that works for any priority level
void worker_thread(void) {
    // Each thread knows its index based on creation order
    static int thread_index = 0;
    int my_index = thread_index++;
    
    if (my_index >= MAX_G_THREADS) {
        printf("ERROR: Thread index out of bounds!\n");
        return;
    }
    
    struct thread_data *data = &thread_params[my_index];
    
    int i = 0;
    
    // Priority-specific label for output
    const char* priority_label = data->label;
    int id = data->id;
    
    while (true) {
        printf("%s priority thread id = %d, val = %d BEGINNING\n", 
               priority_label, id, ++i);
        gt_uninterruptible_nanosleep(0, 50000000);
        printf("%s priority thread id = %d, val = %d END\n", 
               priority_label, id, ++i);
        gt_uninterruptible_nanosleep(0, 50000000);
    }
}


int main(int argc, char *argv[]) {    
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
    
    gt_init();  // Initialize threads
    
    // Create threads with different priorities and configurations
    thread_params[0].id = 1;
    thread_params[0].priority = 0;  // Highest priority
    thread_params[0].tickets = 50;  // High number of tickets for lottery scheduling
    thread_params[0].label = "HIGH";
    gt_create(worker_thread, &thread_params[0]);
    
    thread_params[1].id = 2;
    thread_params[1].priority = 0;  // Also highest priority
    thread_params[1].tickets = 30;  // Medium number of tickets
    thread_params[1].label = "HIGH";
    gt_create(worker_thread, &thread_params[1]);
    
    thread_params[2].id = 3;
    thread_params[2].priority = 5;  // Medium priority
    thread_params[2].tickets = 15;  // Fewer tickets
    thread_params[2].label = "MED ";
    gt_create(worker_thread, &thread_params[2]);
    
    thread_params[3].id = 4;
    thread_params[3].priority = 10; // Lowest priority
    thread_params[3].tickets = 5;   // Very few tickets
    thread_params[3].label = "LOW ";
    gt_create(worker_thread, &thread_params[3]);
        
    gt_return(1);  // Wait until all threads terminate
}
