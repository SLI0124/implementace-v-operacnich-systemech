enum {
	MaxGThreads = 5, // Maximum number of threads, used as array size for gttbl
	StackSize = 0x400000, // Size of stack of each thread
	MaxPriority = 10, // Maximum priority value (lowest priority)
	MinPriority = 0,  // Minimum priority value (highest priority)
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
	} state;
	
	// Thread priority (0 = highest, 10 = lowest)
	int priority;
	// Original priority assigned at creation time
	int original_priority;
	// Starvation counter - incremented when thread is not scheduled
	int starvation_count;
	// Performance tracking data
	struct gt_metrics metrics;
};


void gt_init(void); // initialize gttbl
void gt_return(int ret); // terminate thread
void gt_switch(struct gt_context *old, struct gt_context *new); // declaration from gtswtch.S
bool gt_schedule(void); // yield and switch to another thread
void gt_stop(void); // terminate current thread
int gt_create(void (*f)(void), int priority); // create new thread with given priority
void gt_reset_sig(int sig); // resets signal
void gt_alarm_handle(int sig); // periodically triggered by alarm
int gt_uninterruptible_nanosleep(time_t sec, long nanosec); // uninterruptible sleep
void gt_print_stats();
