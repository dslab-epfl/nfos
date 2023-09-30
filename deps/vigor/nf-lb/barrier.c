/* single-instance only, re-entrant, spinning barrier
 * Taken from https://stackoverflow.com/questions/33598686/spinning-thread-barrier-using-atomic-builtins
 */

static int P;
static volatile int bar __attribute__ ((aligned (64))) = 0; // Counter of threads, faced barrier.
static volatile int passed __attribute__ ((aligned (64))) = 0; // Number of barriers, passed by all threads.

void barrier_init(int num_cores) {
    P = num_cores;
}

void barrier_wait_slow()
{
    int passed_old = passed; // Should be evaluated before incrementing *bar*!

    if(__sync_fetch_and_add(&bar,1) == (P - 1))
    {
        // The last thread, faced barrier.
        bar = 0;
        // *bar* should be reseted strictly before updating of barriers counter.
        __sync_synchronize(); 
        passed++; // Mark barrier as passed.
    }
    else
    {
        // Not the last thread. Wait others.
        while(passed == passed_old) {};
        // Need to synchronize cache with other threads, passed barrier.
        __sync_synchronize();
    }
}

void barrier_wait()
{
    int passed_old = __atomic_load_n(&passed, __ATOMIC_RELAXED); // Should be evaluated before incrementing *bar*!

    if(__sync_fetch_and_add(&bar,1) == (P - 1))
    {
        // The last thread, faced barrier.
        bar = 0;
        // *bar* should be reseted strictly before updating of barriers counter.
        __atomic_store_n(&passed, passed_old + 1, __ATOMIC_RELEASE); // Mark barrier as passed.
    }
    else
    {
        // Not the last thread. Wait others.
        while(__atomic_load_n(&passed, __ATOMIC_RELAXED) == passed_old) {};
        // Need to synchronize cache with other threads, passed barrier.
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    }
}