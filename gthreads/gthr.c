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

#include "gthr.h"

#include <bits/sigaction.h>
#include <bits/types/sigset_t.h>

#include "gthr_struct.h"

// Calculate microseconds between two timevals
static unsigned long time_elapsed_us(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) * 1000000 + (end->tv_usec - start->tv_usec);
}

// Initialize thread performance metrics
static void init_thread_metrics(struct gt_metrics *m) {
    gettimeofday(&m->creation_time, NULL);
    gettimeofday(&m->exec_start_time, NULL);
    gettimeofday(&m->ready_start_time, NULL);
    
    m->exec_total_time = 0;
    m->wait_total_time = 0;
    
    m->exec_shortest = ULONG_MAX;
    m->exec_longest = 0;
    m->exec_time_sum = 0;
    m->exec_time_sq_sum = 0;
    m->exec_periods = 0;
    
    m->wait_shortest = ULONG_MAX;
    m->wait_longest = 0;
    m->wait_time_sum = 0;
    m->wait_time_sq_sum = 0;
    m->wait_periods = 0;
}

// function triggered periodically by timer (SIGALRM)
void gt_alarm_handle(int sig) {
	gt_schedule();
}

void gt_print_stats() {
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    
    printf("\n================ Thread Performance Report ================\n");
    printf("%-4s | %-8s | %-8s | %-8s | %-12s | %-12s | %-10s | %-10s\n", 
           "ID", "Status", "Priority", "Original", "Exec Time(μs)", "Wait Time(μs)", "Avg Exec", "Avg Wait");
    printf("---------------------------------------------------------------------\n");
    
    for (int i = 0; i < MaxGThreads; i++) {
        struct gt *thread = &gt_table[i];
        
        // Skip completely unused threads
        if (thread->state == Unused && thread->metrics.exec_periods == 0) {
            continue;
        }
        
        // Get current metrics for active threads
        unsigned long current_exec = 0;
        unsigned long current_wait = 0;
        
        if (thread->state == Running) {
            current_exec = time_elapsed_us(&thread->metrics.exec_start_time, &current_time);
        }
        else if (thread->state == Ready) {
            current_wait = time_elapsed_us(&thread->metrics.ready_start_time, &current_time);
        }
        
        // Total time including current period
        unsigned long total_exec = thread->metrics.exec_total_time + current_exec;
        unsigned long total_wait = thread->metrics.wait_total_time + current_wait;
        
        // Calculate average times
        double avg_exec = thread->metrics.exec_periods > 0 ? 
            (double)thread->metrics.exec_time_sum / thread->metrics.exec_periods : 0;
            
        double avg_wait = thread->metrics.wait_periods > 0 ? 
            (double)thread->metrics.wait_time_sum / thread->metrics.wait_periods : 0;
        
        // Thread state string
        const char *state_str = 
            thread->state == Running ? "Running" : 
            thread->state == Ready ? "Ready" : "Unused";
        
        printf("%-4d | %-8s | %-8d | %-8d | %-12lu | %-12lu | %-10.2f | %-10.2f\n", 
               i, state_str, 
               thread->priority, thread->original_priority,
               total_exec, total_wait, avg_exec, avg_wait);
    }
    
    printf("\n--- Detailed Statistics ---\n");
    for (int i = 0; i < MaxGThreads; i++) {
        struct gt *thread = &gt_table[i];
        if (thread->state != Unused || thread->metrics.exec_periods > 0) {
            // Calculate variance if we have enough data points
            double exec_variance = 0;
            if (thread->metrics.exec_periods > 1) {
                double avg = (double)thread->metrics.exec_time_sum / thread->metrics.exec_periods;
                exec_variance = ((double)thread->metrics.exec_time_sq_sum / thread->metrics.exec_periods) - (avg * avg);
            }
            
            double wait_variance = 0;
            if (thread->metrics.wait_periods > 1) {
                double avg = (double)thread->metrics.wait_time_sum / thread->metrics.wait_periods;
                wait_variance = ((double)thread->metrics.wait_time_sq_sum / thread->metrics.wait_periods) - (avg * avg);
            }
            
            printf("Thread %d:\n", i);
            printf("  Priority: %d (Original: %d), Starvation count: %d\n", 
                   thread->priority, thread->original_priority, thread->starvation_count);
            printf("  RSP: 0x%lx\n", thread->ctx.rsp);
            printf("  Execution: min=%lu μs, max=%lu μs, periods=%u, variance=%.2f\n",
                   thread->metrics.exec_shortest == ULONG_MAX ? 0 : thread->metrics.exec_shortest,
                   thread->metrics.exec_longest,
                   thread->metrics.exec_periods,
                   exec_variance);
            printf("  Wait time: min=%lu μs, max=%lu μs, periods=%u, variance=%.2f\n",
                   thread->metrics.wait_shortest == ULONG_MAX ? 0 : thread->metrics.wait_shortest,
                   thread->metrics.wait_longest,
                   thread->metrics.wait_periods,
                   wait_variance);
        }
    }
    printf("===============================================================\n");
}

// initialize first thread as current context
void gt_init(void) {
	gt_current = &gt_table[0]; // initialize current thread with thread #0
	gt_current->state = Running; // set current to running
	
	// Initialize metrics for the main thread
	init_thread_metrics(&gt_current->metrics);
	
	signal(SIGALRM, gt_alarm_handle); // register SIGALRM, signal from timer generated by alarm
	signal(SIGINT, gt_print_stats);   // register SIGINT handler for statistics display
}

// exit thread
void __attribute__((noreturn)) gt_return(int ret) {
	if (gt_current != &gt_table[0]) {
		// if not an initial thread
		struct timeval exit_time;
		gettimeofday(&exit_time, NULL);
		
		// Update final execution time
		unsigned long exec_time = time_elapsed_us(&gt_current->metrics.exec_start_time, &exit_time);
		gt_current->metrics.exec_total_time += exec_time;
		
		gt_current->state = Unused; // set current thread as unused
		free((void *) (gt_current->ctx.rsp + 16)); // free the stack
		gt_schedule(); // yield and make possible to switch to another thread
		assert(!"reachable");
		// this code should never be reachable ... (if yes, returning function on stack was corrupted)
	}
	while (gt_schedule()); // if initial thread, wait for other to terminate
	exit(ret);
}

// switch from one thread to other
bool gt_schedule(void) {
	struct gt *p;
	struct gt_context *old, *new;
	struct timeval switch_time;
	gettimeofday(&switch_time, NULL);
	bool thread_found = false;
	static int round_robin_index = 0;  // Index to start our search from (for round-robin fairness)

	gt_reset_sig(SIGALRM); // reset signal

	// Update metrics for the current running thread
	if (gt_current->state == Running) {
		unsigned long exec_time = time_elapsed_us(&gt_current->metrics.exec_start_time, &switch_time);
		gt_current->metrics.exec_total_time += exec_time;
		gt_current->metrics.exec_periods++;
		
		// Update min/max/sum metrics
		if (exec_time < gt_current->metrics.exec_shortest) {
			gt_current->metrics.exec_shortest = exec_time;
		}
		if (exec_time > gt_current->metrics.exec_longest) {
			gt_current->metrics.exec_longest = exec_time;
		}
		gt_current->metrics.exec_time_sum += exec_time;
		gt_current->metrics.exec_time_sq_sum += (exec_time * exec_time);
	}

	// First reset starvation counter for current thread if it was running
	if (gt_current->state == Running) {
		gt_current->starvation_count = 0;
		// Reset the thread's priority to its original value
		gt_current->priority = gt_current->original_priority;
	}
	
	// Increment starvation counter for all Ready threads
	for (p = &gt_table[0]; p < &gt_table[MaxGThreads]; p++) {
		if (p->state == Ready) {
			p->starvation_count++;
			// The more it starves, the higher its priority becomes
			// Starving threads now get priority boost much faster
			int new_priority = p->original_priority - p->starvation_count;
			if (new_priority < MinPriority) new_priority = MinPriority;
			p->priority = new_priority;
			
			// If a thread has been starving for too long, force it to run next
			if (p->starvation_count > 10) {
				// Super boost - give it highest priority plus a bonus
				p->priority = MinPriority - 1;  // Even higher than normal highest priority
			}
		}
	}
	
	// Find next thread to run based on priority
	struct gt *selected_thread = NULL;
	int max_starvation = -1;  // Track the thread with the most starvation
	
	// First, check if any thread has critical starvation (force it to run)
	for (p = &gt_table[0]; p < &gt_table[MaxGThreads]; p++) {
		if (p->state == Ready && p->starvation_count > 10 && p->starvation_count > max_starvation) {
			selected_thread = p;
			max_starvation = p->starvation_count;
			thread_found = true;
		}
	}
	
	// If no critically starving thread, use priority-based scheduling
	if (!thread_found) {
		// Use round-robin within each priority level for fairness
		// Start searching from the thread after the last scheduled one
		int start_idx = (round_robin_index + 1) % MaxGThreads;
		
		// First search loop - look for highest priority thread starting from round_robin_index
		for (int current_priority = MinPriority; current_priority <= MaxPriority && !thread_found; current_priority++) {
			for (int i = 0; i < MaxGThreads && !thread_found; i++) {
				int idx = (start_idx + i) % MaxGThreads;
				p = &gt_table[idx];
				if (p->state == Ready && p->priority == current_priority) {
					selected_thread = p;
					round_robin_index = idx;  // Update for next scheduling decision
					thread_found = true;
				}
			}
		}
	}
	
	// If no Ready thread was found, return false
	if (!selected_thread) {
		return false;
	}
	
	p = selected_thread;

	// Update wait time for the thread that's about to run
	if (p->state == Ready) {
		unsigned long wait_time = time_elapsed_us(&p->metrics.ready_start_time, &switch_time);
		p->metrics.wait_total_time += wait_time;
		p->metrics.wait_periods++;
		
		// Update min/max/sum metrics for wait time
		if (wait_time < p->metrics.wait_shortest) {
			p->metrics.wait_shortest = wait_time;
		}
		if (wait_time > p->metrics.wait_longest) {
			p->metrics.wait_longest = wait_time;
		}
		p->metrics.wait_time_sum += wait_time;
		p->metrics.wait_time_sq_sum += (wait_time * wait_time);
	}

	// Update current thread state
	if (gt_current->state != Unused) {
		gt_current->state = Ready;
		gettimeofday(&gt_current->metrics.ready_start_time, NULL);
	}
	
	// Update new thread state
	p->state = Running;
	gettimeofday(&p->metrics.exec_start_time, NULL);
	
	// Prepare for context switch
	old = &gt_current->ctx; // prepare pointers to context of current (will become old)
	new = &p->ctx; // and new to new thread found in previous loop
	gt_current = p; // switch current indicator to new thread
	gt_switch(old, new); // perform context switch (assembly in gtswtch.S)
	return true;
}

// return function for terminating thread
void gt_stop(void) {
	gt_return(0);
}

// create new thread by providing pointer to function that will act like "run" method
int gt_create(void (*f)(void), int priority) {
	char *stack;
	struct gt *p;

	// Validate priority
	if (priority < MinPriority) priority = MinPriority;
	if (priority > MaxPriority) priority = MaxPriority;

	for (p = &gt_table[0];; p++) // find an empty slot
		if (p == &gt_table[MaxGThreads])
			// if we have reached the end, gt_table is full and we cannot create a new thread
			return -1;
		else if (p->state == Unused)
			break; // new slot was found

	stack = malloc(StackSize); // allocate memory for stack of newly created thread
	if (!stack)
		return -1;

	*(uint64_t *) &stack[StackSize - 8] = (uint64_t) gt_stop;
	//  put into the stack returning function gt_stop in case function calls return
	*(uint64_t *) &stack[StackSize - 16] = (uint64_t) f; //  put provided function as a main "run" function
	p->ctx.rsp = (uint64_t) &stack[StackSize - 16]; //  set stack pointer
	p->state = Ready; //  set state
	p->priority = priority;              // Set the thread priority
	p->original_priority = priority;     // Remember the original priority
	p->starvation_count = 0;             // Initialize starvation counter
	
	// Initialize metrics for the new thread
	init_thread_metrics(&p->metrics);
	gettimeofday(&p->metrics.ready_start_time, NULL);
	
	return 0;
}

// resets SIGALRM signal
void gt_reset_sig(int sig) {
	if (sig == SIGALRM) {
		alarm(0); // Clear pending alarms if any
	}

	sigset_t set; // Create signal set
	sigemptyset(&set); // Clear set
	sigaddset(&set, sig); // Set signal (we use SIGALRM)
	sigprocmask(SIG_UNBLOCK, &set, NULL); // Fetch and change the signal mask

	if (sig == SIGALRM) {
		ualarm(500, 500); // Schedule signal after given number of microseconds
	}
}

// uninterruptible sleep
int gt_uninterruptible_nanosleep(time_t sec, long nanosec) {
	struct timespec req;
	req.tv_sec = sec;
	req.tv_nsec = nanosec;

	do {
		if (0 != nanosleep(&req, &req)) {
			if (errno != EINTR)
				return -1;
		} else {
			break;
		}
	} while (req.tv_sec > 0 || req.tv_nsec > 0);
	return 0;
}
