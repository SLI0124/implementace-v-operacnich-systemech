// gthread control structures

struct gt gt_table[MAX_G_THREADS];                                                // statically allocated table for thread control
struct gt *gt_current;                                                          // pointer to current thread
