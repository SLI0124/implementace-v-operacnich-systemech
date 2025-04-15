// gthread control structures

struct gt gt_table[MAX_G_THREADS];                                                // statically allocated table for thread control
struct gt *gt_current;                                                          // pointer to current thread
enum gt_scheduler_type gt_current_scheduler = GT_SCHED_PRI;                     // current scheduler type, default is priority-based
