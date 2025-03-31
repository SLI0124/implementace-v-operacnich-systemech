enum {
	MaxGThreads = 5, // Maximum number of threads, used as array size for gttbl
	StackSize = 0x400000, // Size of stack of each thread
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
};


void gt_init(void); // initialize gttbl
void gt_return(int ret); // terminate thread
void gt_switch(struct gt_context *old, struct gt_context *new); // declaration from gtswtch.S
bool gt_schedule(void); // yield and switch to another thread
void gt_stop(void); // terminate current thread
int gt_create(void (*f)(void)); // create new thread and set f as new "run" function
void gt_reset_sig(int sig); // resets signal
void gt_alarm_handle(int sig); // periodically triggered by alarm
int gt_uninterruptible_nanosleep(time_t sec, long nanosec); // uninterruptible sleep
void gt_print_stats();
