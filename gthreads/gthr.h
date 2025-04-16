#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include <bits/sigaction.h>
#include <bits/types/sigset_t.h>

enum {
	MAX_G_THREADS = 5, // Maximum number of threads, used as array size for gttbl
	STACK_SIZE = 0x400000, // Size of stack of each thread
	MAX_PRIORITY = 10, // Maximum priority value (lowest priority)
	MIN_PRIORITY = 0,  // Minimum priority value (highest priority)
	MAX_TICKETS = 100, // Maximum number of tickets per thread for lottery scheduling
	MAX_BLOCKED_THREADS = MAX_G_THREADS, // Maximum number of threads that can be blocked on a semaphore
};

// Thread data structure to pass parameters to threads
struct thread_data {
    int id;
    int priority;
	int tickets;
    const char* label;
};

// Semaphore structure with FIFO queue
typedef struct {
    int value;                          // Current value of the semaphore
    int wait_count;                     // Number of threads waiting on this semaphore
    struct gt *wait_queue[MAX_BLOCKED_THREADS]; // Queue of waiting threads (FIFO)
    int head;                           // Head of the queue
    int tail;                           // Tail of the queue
} gt_semaphore_t;

// Available scheduling algorithms
enum gt_scheduler_type {
    GT_SCHED_RR,  // Round Robin scheduler
    GT_SCHED_PRI, // Priority scheduler
    GT_SCHED_LS   // Lottery scheduler
};

// Thread performance tracking structure
struct gt_metrics {
    struct timeval creation_time;       // Thread creation timestamp
    struct timeval exec_start_time;     // Last execution start time
    struct timeval ready_start_time;    // Last time thread became ready
    
    unsigned long exec_total_time;      // Total execution time (microseconds)
    unsigned long wait_total_time;      // Total wait time (microseconds)
    
    unsigned long exec_shortest;        // Shortest execution period
    unsigned long exec_longest;         // Longest execution period
    unsigned long exec_time_sum;        // Sum for average calculation
    unsigned long exec_time_sq_sum;     // Sum of squares for variance
    unsigned int exec_periods;          // Number of execution periods
    
    unsigned long wait_shortest;        // Shortest wait period
    unsigned long wait_longest;         // Longest wait period
    unsigned long wait_time_sum;        // Sum for average calculation
    unsigned long wait_time_sq_sum;     // Sum of squares for variance
    unsigned int wait_periods;          // Number of wait periods
};

struct gt {
	// Saved context, switched by gtswtch.S (see for detail)
	struct gt_context {
		uint64_t rsp;
		uint64_t r15;
		uint64_t r14;
		uint64_t r13;
		uint64_t r12;
		uint64_t rbx;
		uint64_t rbp;
	} ctx;

	// Thread state
	enum {
		Unused,
		Running,
		Ready,
		Blocked,  // New state for blocked threads
	} state;
	
	// Thread priority (0 = highest, 10 = lowest)
	int priority;
	// Original priority assigned at creation time
	int original_priority;
	// Starvation counter - incremented when thread is not scheduled
	int starvation_count;
	// Performance tracking data
	struct gt_metrics metrics;
    
    // Number of lottery tickets for the lottery scheduler
    int tickets;
};


void gt_init(void); // initialize gttbl
void gt_return(int ret); // terminate thread
void gt_switch(struct gt_context *old, struct gt_context *new); // declaration from gtswtch.S
bool gt_schedule(void); // yield and switch to another thread
void gt_stop(void); // terminate current thread
int gt_create(void (*f)(void), struct thread_data *data); // create new thread with given thread data
void gt_reset_sig(int sig); // resets signal
void gt_alarm_handle(int sig); // periodically triggered by alarm
int gt_uninterruptible_nanosleep(time_t sec, long nanosec); // uninterruptible sleep
void gt_print_stats();
void gt_set_scheduler(enum gt_scheduler_type sched_type); // set the scheduling algorithm

// Semaphore operations
void gt_sem_init(gt_semaphore_t* sem, int initial_value); // initialize semaphore
void gt_sem_wait(gt_semaphore_t* sem);  // P operation (wait)
void gt_sem_post(gt_semaphore_t* sem);  // V operation (signal)
