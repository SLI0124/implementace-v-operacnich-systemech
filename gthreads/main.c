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

#include "gthr.h"

// Thread data structure to pass parameters to threads
struct thread_data {
    int id;
    int priority;
    const char* label;
};

// Array of thread data for each thread
struct thread_data thread_params[MaxGThreads];

// Thread function that works for any priority level
void worker_thread(void) {
    // Each thread knows its index based on creation order
    static int thread_index = 0;
    int my_index = thread_index++;
    
    if (my_index >= MaxGThreads) {
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

int main(void) {    
    gt_init();  // Initialize threads
    
    // Create threads with different priorities
    thread_params[0].id = 1;
    thread_params[0].priority = 0;
    thread_params[0].label = "HIGH";
    gt_create(worker_thread, 0);
    
    thread_params[1].id = 2;
    thread_params[1].priority = 0;
    thread_params[1].label = "HIGH";
    gt_create(worker_thread, 0);
    
    thread_params[2].id = 3;
    thread_params[2].priority = 5;
    thread_params[2].label = "MED ";
    gt_create(worker_thread, 5);
    
    thread_params[3].id = 4;
    thread_params[3].priority = 10;
    thread_params[3].label = "LOW ";
    gt_create(worker_thread, 10);
    
    gt_return(1);  // Wait until all threads terminate
}
